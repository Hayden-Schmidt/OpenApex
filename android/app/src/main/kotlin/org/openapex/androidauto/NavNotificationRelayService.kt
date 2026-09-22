package org.openapex.androidauto

import android.content.Intent
import android.os.Bundle
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification
import android.util.Log

/**
 * Relays Google Maps navigation notifications to the ESP32 terminal. This listener does NO
 * parsing — it reads raw notification extras and forwards them (plus phone GNSS) through
 * [RelayStateHolder]. Maneuver/distance/street extraction lives in the ESP32 normalizer, shared
 * with the iOS/ANCS path where raw text arrives on the terminal directly.
 *
 * Notification access is NOT a normal runtime permission. The user grants it manually via
 * Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS, and it resets on every reinstall during
 * development.
 */
class NavNotificationRelayService : NotificationListenerService() {

    companion object {
        private const val TAG = "OpenApexRelay"
        private const val MAPS_PACKAGE = "com.google.android.apps.maps"
        // Below this, the mask is noise (anti-aliasing fringe, empty icon) — not a real arrow shape.
        private const val MIN_MASK_PIXELS = 40L
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        RelayStateHolder.noteListenerConnected()
        // A navigation notification already posted before this listener (re)connected never
        // fires onNotificationPosted again on its own — seed from whatever is currently active.
        activeNotifications?.firstOrNull { it.packageName == MAPS_PACKAGE }?.let { relayIfNav(it) }
        // System-forced rebind after process death is a more reliable wake signal on this OEM
        // than CDM cold-wake (see docs/CDM_Cold_Wake_Investigation.md) — use it to also restart
        // RelayService, same as BootCompletedReceiver, in case the process was killed without an
        // intervening reboot. No-op if RelayService is already running (own-scan guards on
        // `scanning`, ble.connect() guards on existing connection).
        startForegroundService(Intent(this, RelayService::class.java))
    }

    override fun onListenerDisconnected() {
        super.onListenerDisconnected()
        RelayStateHolder.noteListenerDisconnected()
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        relayIfNav(sbn)
    }

    private fun relayIfNav(sbn: StatusBarNotification) {
        if (sbn.packageName != MAPS_PACKAGE) return
        val extras = sbn.notification.extras
        val nav = RawNavNotification(
            title = extras.getCharSequence("android.title")?.toString(),
            etaText = extras.getCharSequence("android.subText")?.toString(),
            distanceText = extras.getCharSequence("android.shortCriticalText")?.toString(),
            progress = extras.getInt("android.progress", -1).takeIf { it >= 0 },
            progressMax = extras.getInt("android.progressMax", -1).takeIf { it >= 0 },
            iconRotationDeg = extractIconRotationDeg(sbn.notification),
        )
        if (nav.title.isNullOrBlank() && nav.distanceText.isNullOrBlank() && nav.progress == null) {
            return // not a navigation notification worth relaying
        }
        if (extras.getString("android.template") == "android.app.Notification\$ProgressStyle") {
            Log.d(TAG, "nav notification using ProgressStyle template")
        }
        Log.d(TAG, "relay nav: title=${nav.title} dist=${nav.distanceText} progress=${nav.progress}/${nav.progressMax} iconRotationDeg=${nav.iconRotationDeg}")
        RelayStateHolder.updateNav(nav)
    }

    /**
     * Extracts the maneuver arrow's rotation angle from the notification's large icon bitmap, in
     * degrees (0 = up/straight, clockwise positive), or null if unavailable/unextractable. This is
     * raw geometry, not maneuver classification — Google Maps' turn-by-turn icon is a single arrow
     * glyph (arrowhead + trailing dash tail) that rotates to indicate the upcoming turn; bucketing
     * the angle into a maneuver is the ESP32 normalizer's job (see RawNotifPacket.kt KDoc).
     *
     * Approach: build a white/opaque pixel mask from the icon bitmap, find its principal axis via
     * image moments (PCA), then disambiguate the 180-degree axis ambiguity by picking the end with
     * the larger perpendicular spread — the arrowhead triangle is wider than the dash tail.
     */
    private fun extractIconRotationDeg(notification: android.app.Notification): Int? {
        val icon = notification.extras.getParcelable("android.largeIcon", android.graphics.drawable.Icon::class.java)
            ?: notification.getLargeIcon()
            ?: return null
        val drawable = runCatching { icon.loadDrawable(this) }.getOrNull() ?: return null
        val w = drawable.intrinsicWidth
        val h = drawable.intrinsicHeight
        if (w <= 0 || h <= 0) return null
        val bitmap = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888)
        val canvas = android.graphics.Canvas(bitmap)
        drawable.setBounds(0, 0, w, h)
        drawable.draw(canvas)

        val alphaThreshold = 128
        var count = 0L
        var sumX = 0.0
        var sumY = 0.0
        for (y in 0 until h) {
            for (x in 0 until w) {
                if (android.graphics.Color.alpha(bitmap.getPixel(x, y)) >= alphaThreshold) {
                    count++
                    sumX += x
                    sumY += y
                }
            }
        }
        if (count < MIN_MASK_PIXELS) return null
        val meanX = sumX / count
        val meanY = sumY / count

        var sumXX = 0.0
        var sumYY = 0.0
        var sumXY = 0.0
        for (y in 0 until h) {
            for (x in 0 until w) {
                if (android.graphics.Color.alpha(bitmap.getPixel(x, y)) >= alphaThreshold) {
                    val dx = x - meanX
                    val dy = y - meanY
                    sumXX += dx * dx
                    sumYY += dy * dy
                    sumXY += dx * dy
                }
            }
        }
        val muXX = sumXX / count
        val muYY = sumYY / count
        val muXY = sumXY / count
        val axisTheta = 0.5 * kotlin.math.atan2(2.0 * muXY, muXX - muYY)
        val ux = kotlin.math.cos(axisTheta)
        val uy = kotlin.math.sin(axisTheta)

        // Head/tail disambiguation: compare max perpendicular spread on each side of the axis.
        var maxPerpPos = 0.0
        var maxPerpNeg = 0.0
        for (y in 0 until h) {
            for (x in 0 until w) {
                if (android.graphics.Color.alpha(bitmap.getPixel(x, y)) >= alphaThreshold) {
                    val dx = x - meanX
                    val dy = y - meanY
                    val proj = dx * ux + dy * uy
                    val perp = kotlin.math.abs(-dx * uy + dy * ux)
                    if (proj >= 0) {
                        if (perp > maxPerpPos) maxPerpPos = perp
                    } else {
                        if (perp > maxPerpNeg) maxPerpNeg = perp
                    }
                }
            }
        }
        val headSign = if (maxPerpPos >= maxPerpNeg) 1.0 else -1.0
        val headDx = ux * headSign
        val headDy = uy * headSign

        // Image coords: x right, y down. Convention: 0deg = up, clockwise positive (matches
        // heading_deg elsewhere in the protocol).
        val angleRad = kotlin.math.atan2(headDx, -headDy)
        var angleDeg = Math.toDegrees(angleRad).toInt()
        angleDeg = ((angleDeg % 360) + 360) % 360
        return angleDeg
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (sbn.packageName == MAPS_PACKAGE) {
            RelayStateHolder.updateNav(null)
        }
    }
}

private fun Bundle.getCharSequence(key: String): CharSequence? =
    if (containsKey(key)) getCharSequence(key) else null
