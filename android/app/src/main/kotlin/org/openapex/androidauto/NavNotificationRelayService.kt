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

        /**
         * Packages the dev recorder captures wholesale. `null` means EVERY package -- the default,
         * because the point of a capture is to see what we are not yet parsing, and a filter built
         * from today's knowledge cannot show that.
         *
         * Set this to a concrete set (e.g. the nav packages below) before taking a capture that
         * will leave this machine: `null` records message and email bodies too.
         */
        private val RECORD_PACKAGES: Set<String>? = null

        /** Known navigation packages, for narrowing RECORD_PACKAGES when a capture must be shared. */
        @Suppress("unused")
        private val NAV_PACKAGES = setOf(
            MAPS_PACKAGE,
            "com.waze",
            "com.google.android.apps.maps.car",
        )
        // Below this, the mask is noise (anti-aliasing fringe, empty icon) — not a real arrow shape.
        private const val MIN_MASK_PIXELS = 40L
    }

    // Last IDENT line logged, minus the title, so re-posts of an unchanged notification don't
    // flood the log. See logManeuverIdentity().
    private var lastIdentityKey: String? = null
    private var dumpedProgressStyle = false

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
        recordWholesale(sbn, "posted")
        relayIfNav(sbn)
    }

    /**
     * Dev-only capture of EVERY notification, from every package, before any filtering.
     *
     * Deliberately ahead of [relayIfNav]'s package and shape checks. Those checks encode what we
     * already know about Google Maps; a capture taken behind them can only ever confirm what we
     * built for. Waze and Apple Maps ship different package names and different extras, so a Waze
     * drive recorded behind the MAPS_PACKAGE check would produce an empty file and cost another
     * drive to discover why.
     *
     * Recording unconditionally also captures the notifications we *should* have relayed but
     * didn't -- a nav update dropped by the shape check at line ~68 is invisible in a filtered log
     * precisely when it matters most.
     *
     * Privacy: this writes every notification on the phone to disk, including message and email
     * bodies. It is why the recorder is BuildConfig.DEBUG-only and must never ship enabled. Narrow
     * [RECORD_PACKAGES] to a set of nav packages if a capture is going anywhere but this machine.
     */
    private fun recordWholesale(sbn: StatusBarNotification, event: String) {
        if (!RelayRecorder.enabled) return
        val allowed = RECORD_PACKAGES
        if (allowed != null && sbn.packageName !in allowed) return
        runCatching {
            val extras = sbn.notification.extras
            RelayRecorder.notificationRaw(
                pkg = sbn.packageName,
                event = event,
                key = sbn.key,
                postTimeMs = sbn.postTime,
                template = extras.getString("android.template"),
                extras = bundleToJson(extras),
                icons = iconsToJson(sbn.notification, extras),
            )
        }
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
            iconRotationDeg = extractIconRotationDeg(sbn.notification, isRoundabout = extras.getCharSequence("android.title")
                ?.toString()?.contains("roundabout", ignoreCase = true) == true),
            segments = extractProgressSegments(sbn.notification),
        )
        if (nav.title.isNullOrBlank() && nav.distanceText.isNullOrBlank() && nav.progress == null) {
            return // not a navigation notification worth relaying
        }
        if (extras.getString("android.template") == "android.app.Notification\$ProgressStyle") {
            logProgressStyleDiagnostics(sbn.notification, extras)
        }
        logManeuverIdentity(sbn.notification, extras, nav.title)
        // The wholesale dump already happened in recordWholesale(), ahead of every filter above.
        // This records only the PARSED result, so a capture shows both what arrived and what we
        // made of it -- the two halves that have to be compared when Waze parsing goes wrong.
        RelayRecorder.parsed(
            pkg = sbn.packageName,
            title = nav.title,
            etaText = nav.etaText,
            distanceText = nav.distanceText,
            progress = nav.progress,
            progressMax = nav.progressMax,
            iconRotationDeg = nav.iconRotationDeg,
            segments = nav.segments,
        )
        Log.d(TAG, "relay nav: title=${nav.title} dist=${nav.distanceText} progress=${nav.progress}/${nav.progressMax} iconRotationDeg=${nav.iconRotationDeg}")
        RelayStateHolder.updateNav(nav)
    }

    /**
     * Google Maps' progress-bar segments (traffic colouring along the route), via the public
     * ProgressStyle API rather than the undocumented Bundle keys the diagnostic dump walks.
     * Empty below Android 16 or when Maps posts no ProgressStyle.
     */
    private fun extractProgressSegments(notification: android.app.Notification): List<ProgressSegment> {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.BAKLAVA) return emptyList()
        return runCatching {
            val style = android.app.Notification.Builder.recoverBuilder(this, notification).style
            (style as? android.app.Notification.ProgressStyle)?.progressSegments
                ?.map { ProgressSegment(it.length, it.color) }
        }.getOrNull().orEmpty()
    }

    /**
     * One-shot capture of what a real Android 16 ProgressStyle nav notification actually contains,
     * since there's no documented public extras key for its Points/Segments/ProgressTrackerIcon
     * Parcelables. Logs every extras key + a safe stringified value, plus Icon.getType() for both
     * the large and small icon -- if either comes back TYPE_RESOURCE (not TYPE_BITMAP), Maps is
     * choosing the maneuver glyph from a drawable resource rather than rendering a rotated bitmap,
     * which would let us match against a resource name instead of PCA-guessing an angle.
     */
    private fun logProgressStyleDiagnostics(notification: android.app.Notification, extras: Bundle) {
        // Genuinely one-shot: Maps re-posts several times a second and this dump is now recursive,
        // so without the guard it buries the per-maneuver IDENT lines it exists to support.
        if (dumpedProgressStyle) return
        dumpedProgressStyle = true
        dumpBundle(extras, prefix = "  extra")
        logIconMetadata("largeIcon", notification.extras.getParcelable("android.largeIcon", android.graphics.drawable.Icon::class.java)
            ?: notification.getLargeIcon())
        logIconMetadata("smallIcon", notification.smallIcon)
        // Every Icon anywhere in the notification, not just the two we already relay -- including
        // android.progressTrackerIcon and any icon nested inside the ProgressStyle Parcelables or
        // the notification's actions. Any of these coming back TYPE_RESOURCE is a candidate for
        // identifying the maneuver by resource id instead of parsing bitmap geometry.
        for ((label, icon) in collectIcons(notification, extras)) {
            logIconMetadata(label, icon)
        }
    }

    /**
     * Recursively logs a Bundle, descending into nested Bundles and Lists. The ProgressStyle
     * Parcelables (android.progressSegments / android.progressPoints) stringify only as
     * "Bundle[mParcelledData.dataSize=124]" until something forces them to unparcel, which is why
     * the earlier capture (docs/Android16_ProgressStyle_Notification_Extras.md) never saw inside
     * them. Touching keySet() unparcels; it can throw for classes we don't have, hence the catch.
     */
    private fun dumpBundle(bundle: Bundle, prefix: String, depth: Int = 0) {
        if (depth > 3) return
        val keys = runCatching { bundle.keySet().sorted() }.getOrNull()
        if (keys == null) {
            Log.d(TAG, "$prefix <unparcelable bundle>")
            return
        }
        for (key in keys) {
            val value = runCatching { @Suppress("DEPRECATION") bundle.get(key) }.getOrNull()
            Log.d(TAG, "$prefix[$key] (${value?.javaClass?.simpleName}) = $value")
            when (value) {
                is Bundle -> dumpBundle(value, "$prefix[$key]", depth + 1)
                is List<*> -> value.forEachIndexed { i, item ->
                    if (item is Bundle) dumpBundle(item, "$prefix[$key][$i]", depth + 1)
                    else Log.d(TAG, "$prefix[$key][$i] (${item?.javaClass?.simpleName}) = $item")
                }
            }
        }
    }

    /** Every Icon reachable from the notification, labelled by where it was found. */
    private fun collectIcons(
        notification: android.app.Notification,
        extras: Bundle,
    ): List<Pair<String, android.graphics.drawable.Icon>> {
        val found = mutableListOf<Pair<String, android.graphics.drawable.Icon>>()
        val keys = runCatching { extras.keySet() }.getOrNull().orEmpty()
        for (key in keys) {
            val value = runCatching { @Suppress("DEPRECATION") extras.get(key) }.getOrNull()
            if (value is android.graphics.drawable.Icon) found += key to value
            if (value is List<*>) {
                value.forEachIndexed { i, item ->
                    if (item is Bundle) {
                        val nested = runCatching { item.keySet() }.getOrNull().orEmpty()
                        for (nk in nested) {
                            val nv = runCatching { @Suppress("DEPRECATION") item.get(nk) }.getOrNull()
                            if (nv is android.graphics.drawable.Icon) found += "$key[$i].$nk" to nv
                        }
                    }
                }
            }
        }
        notification.actions?.forEachIndexed { i, action ->
            runCatching { action.getIcon() }.getOrNull()?.let { found += "action[$i]" to it }
        }
        return found
    }

    /**
     * One compact, de-duplicated line per distinct maneuver rendering, correlating the title text
     * (ground truth for what the maneuver actually is) against every id-like handle we could key
     * off instead of PCA-parsing the arrow bitmap:
     *
     *  - `tracker=` android.progressTrackerIcon. The prior capture recorded this as TYPE_RESOURCE
     *    (id=0x7f0805b9) — the one resource id in the whole notification. Unknown so far is
     *    whether it *varies by maneuver*: if the id differs between a left turn, a right turn and
     *    a roundabout, a resId→maneuver table replaces bitmap parsing outright. If it's constant
     *    across a drive it's just a static "navigation" badge and this lead is dead.
     *  - `small=` / `large=` the icons we already handle, re-logged here for the correlation.
     *  - `mask=` a rotation-*sensitive* fingerprint of the large icon's alpha mask, and `shape=`
     *    a rotation-*insensitive* one (radial histogram about the centroid). If `shape` repeats
     *    across a drive while `mask` doesn't, Maps is rotating one arrow glyph and only geometry
     *    can recover direction; if `shape` takes a small number of distinct values matching the
     *    maneuver set, a fingerprint→maneuver table also replaces PCA.
     *
     * De-duplicated because Maps re-posts the same notification several times a second; only a
     * changed fingerprint/id/maneuver-clause logs a new line, so a drive yields a readable table.
     */
    private fun logManeuverIdentity(
        notification: android.app.Notification,
        extras: Bundle,
        title: String?,
    ) {
        val tracker = runCatching {
            extras.getParcelable("android.progressTrackerIcon", android.graphics.drawable.Icon::class.java)
        }.getOrNull()
        val large = extras.getParcelable("android.largeIcon", android.graphics.drawable.Icon::class.java)
            ?: notification.getLargeIcon()
        val bitmap = large?.let { renderIconBitmap(it) }
        val line = "IDENT title=\"$title\"" +
            " tracker=${iconIdentity(tracker)}" +
            " small=${iconIdentity(notification.smallIcon)}" +
            " large=${iconIdentity(large)}" +
            " size=${bitmap?.width}x${bitmap?.height}" +
            " mask=${bitmap?.let { maskFingerprint(it) }}" +
            " shape=${bitmap?.let { radialFingerprint(it) }}"
        val dedupeKey = line.substringAfter(" tracker=")
        if (dedupeKey != lastIdentityKey) {
            lastIdentityKey = dedupeKey
            Log.i(TAG, line)
        }
    }

    /** "RESOURCE:0x7f0805b9:ic_foo" / "BITMAP" / "null" — the id-like part of an Icon. */
    private fun iconIdentity(icon: android.graphics.drawable.Icon?): String {
        if (icon == null) return "null"
        if (icon.type != android.graphics.drawable.Icon.TYPE_RESOURCE) return "type${icon.type}"
        val resId = icon.resId
        val resName = runCatching {
            packageManager.getResourcesForApplication(MAPS_PACKAGE).getResourceEntryName(resId)
        }.getOrNull()
        return "RESOURCE:0x${Integer.toHexString(resId)}:$resName"
    }

    /**
     * Rotation-sensitive hash: the alpha mask resampled onto a fixed 16x16 grid, so it's stable
     * against icon size and anti-aliasing but changes as the arrow rotates.
     */
    private fun maskFingerprint(bitmap: android.graphics.Bitmap): String {
        var hash = 0x811C9DC5.toInt()
        for (gy in 0 until 16) {
            for (gx in 0 until 16) {
                val x = gx * bitmap.width / 16
                val y = gy * bitmap.height / 16
                val on = android.graphics.Color.alpha(bitmap.getPixel(x, y)) >= 128
                hash = (hash xor if (on) 1 else 0) * 16777619
            }
        }
        return "0x${Integer.toHexString(hash)}"
    }

    /**
     * Rotation-insensitive shape descriptor: a coarse histogram of mask-pixel distance from the
     * centroid, normalized by the mask's max radius. Two renderings of the same glyph at different
     * rotations hash the same; a genuinely different glyph does not.
     */
    private fun radialFingerprint(bitmap: android.graphics.Bitmap): String {
        val w = bitmap.width
        val h = bitmap.height
        var count = 0
        var sumX = 0.0
        var sumY = 0.0
        for (y in 0 until h) for (x in 0 until w) {
            if (android.graphics.Color.alpha(bitmap.getPixel(x, y)) >= 128) {
                count++; sumX += x; sumY += y
            }
        }
        if (count == 0) return "empty"
        val cx = sumX / count
        val cy = sumY / count
        var maxR = 0.0
        val radii = DoubleArray(count)
        var i = 0
        for (y in 0 until h) for (x in 0 until w) {
            if (android.graphics.Color.alpha(bitmap.getPixel(x, y)) >= 128) {
                val r = kotlin.math.hypot(x - cx, y - cy)
                radii[i++] = r
                if (r > maxR) maxR = r
            }
        }
        if (maxR <= 0.0) return "point"
        val bins = IntArray(8)
        for (r in radii) {
            val b = ((r / maxR) * 8).toInt().coerceIn(0, 7)
            bins[b]++
        }
        // Quantize each bin to a percentage of the mask, so minor rendering differences don't
        // change the descriptor while real shape differences do.
        return bins.joinToString("-") { ((it * 100L) / count).toString() }
    }

    /**
     * Serializes a Bundle for the drive recorder, recursing into nested Bundles and Lists. Values
     * with no JSON equivalent are stringified rather than dropped — for an unexplored source like
     * Waze, "there was an opaque Parcelable under this key" is itself the finding.
     */
    private fun bundleToJson(bundle: Bundle, depth: Int = 0): org.json.JSONObject {
        val json = org.json.JSONObject()
        if (depth > 3) return json
        val keys = runCatching { bundle.keySet() }.getOrNull() ?: return json
        for (key in keys) {
            val value = runCatching { @Suppress("DEPRECATION") bundle.get(key) }.getOrNull()
            val encoded: Any? = when (value) {
                null -> org.json.JSONObject.NULL
                is Bundle -> bundleToJson(value, depth + 1)
                is CharSequence -> value.toString()
                is Number, is Boolean -> value
                is List<*> -> org.json.JSONArray().apply {
                    value.forEach { put(if (it is Bundle) bundleToJson(it, depth + 1) else it?.toString()) }
                }
                else -> value.toString()
            }
            runCatching { json.put(key, encoded) }
        }
        return json
    }

    /** Icon identity + fingerprints for every Icon in the notification, for the drive recorder. */
    private fun iconsToJson(notification: android.app.Notification, extras: Bundle): org.json.JSONObject {
        val json = org.json.JSONObject()
        val entries = collectIcons(notification, extras).toMutableList()
        notification.smallIcon?.let { entries += "smallIcon" to it }
        for ((label, icon) in entries) {
            val entry = org.json.JSONObject()
            runCatching {
                entry.put("identity", iconIdentity(icon))
                val bitmap = renderIconBitmap(icon)
                if (bitmap != null) {
                    entry.put("size", "${bitmap.width}x${bitmap.height}")
                    entry.put("mask", maskFingerprint(bitmap))
                    entry.put("shape", radialFingerprint(bitmap))
                }
                json.put(label, entry)
            }
        }
        return json
    }

    /** Rasterizes an Icon to a bitmap, or null if it has no intrinsic size / won't load. */
    private fun renderIconBitmap(icon: android.graphics.drawable.Icon): android.graphics.Bitmap? {
        val drawable = runCatching { icon.loadDrawable(this) }.getOrNull() ?: return null
        val w = drawable.intrinsicWidth
        val h = drawable.intrinsicHeight
        if (w <= 0 || h <= 0) return null
        val bitmap = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888)
        drawable.setBounds(0, 0, w, h)
        drawable.draw(android.graphics.Canvas(bitmap))
        return bitmap
    }

    private fun logIconMetadata(label: String, icon: android.graphics.drawable.Icon?) {
        if (icon == null) {
            Log.d(TAG, "  icon[$label] = null")
            return
        }
        val typeName = when (icon.type) {
            android.graphics.drawable.Icon.TYPE_BITMAP -> "BITMAP"
            android.graphics.drawable.Icon.TYPE_RESOURCE -> "RESOURCE"
            android.graphics.drawable.Icon.TYPE_DATA -> "DATA"
            android.graphics.drawable.Icon.TYPE_URI -> "URI"
            android.graphics.drawable.Icon.TYPE_ADAPTIVE_BITMAP -> "ADAPTIVE_BITMAP"
            else -> "UNKNOWN(${icon.type})"
        }
        val resId = if (icon.type == android.graphics.drawable.Icon.TYPE_RESOURCE) icon.resId else null
        val resName = resId?.let {
            runCatching { packageManager.getResourcesForApplication(MAPS_PACKAGE).getResourceEntryName(it) }.getOrNull()
        }
        Log.d(TAG, "  icon[$label] type=$typeName resId=$resId resName=$resName")
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
    private fun extractIconRotationDeg(notification: android.app.Notification, isRoundabout: Boolean): Int? {
        val icon = notification.extras.getParcelable("android.largeIcon", android.graphics.drawable.Icon::class.java)
            ?: notification.getLargeIcon()
            ?: return null
        val bitmap = renderIconBitmap(icon) ?: return null
        val w = bitmap.width
        val h = bitmap.height

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

        if (isRoundabout) {
            // The roundabout glyph is a near-circular ring (rotationally symmetric-ish) with one
            // small exit tick protruding from it. PCA's principal axis is dominated by the
            // symmetric ring and is unstable/unreliable here (confirmed: a real right-hand exit
            // measured 263deg via PCA, which the turn-severity bucketing below misread as LEFT).
            // The exit tick's tip is reliably the mask pixel farthest from the centroid, since it
            // sticks out past the ring -- use its direction directly instead of PCA.
            var bestDistSq = -1.0
            var bestDx = 0.0
            var bestDy = 0.0
            for (y in 0 until h) {
                for (x in 0 until w) {
                    if (android.graphics.Color.alpha(bitmap.getPixel(x, y)) >= alphaThreshold) {
                        val dx = x - meanX
                        val dy = y - meanY
                        val distSq = dx * dx + dy * dy
                        if (distSq > bestDistSq) {
                            bestDistSq = distSq
                            bestDx = dx
                            bestDy = dy
                        }
                    }
                }
            }
            if (bestDistSq < 0.0) return null
            val angleRad = kotlin.math.atan2(bestDx, -bestDy)
            var angleDeg = Math.toDegrees(angleRad).toInt()
            angleDeg = ((angleDeg % 360) + 360) % 360
            return angleDeg
        }

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
        // Recorded too: a nav notification disappearing is the event behind "the display went
        // blank", and it is invisible if only posts are captured.
        recordWholesale(sbn, "removed")
        if (sbn.packageName == MAPS_PACKAGE) {
            RelayStateHolder.updateNav(null)
        }
    }
}

private fun Bundle.getCharSequence(key: String): CharSequence? =
    if (containsKey(key)) getCharSequence(key) else null
