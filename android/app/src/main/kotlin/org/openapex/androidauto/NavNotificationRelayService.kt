package org.openapex.androidauto

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
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        RelayStateHolder.noteListenerConnected()
    }

    override fun onListenerDisconnected() {
        super.onListenerDisconnected()
        RelayStateHolder.noteListenerDisconnected()
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (sbn.packageName != MAPS_PACKAGE) return
        val extras = sbn.notification.extras
        val nav = RawNavNotification(
            title = extras.getCharSequence("android.title")?.toString(),
            etaText = extras.getCharSequence("android.subText")?.toString(),
            distanceText = extras.getCharSequence("android.shortCriticalText")?.toString(),
            progress = extras.getInt("android.progress", -1).takeIf { it >= 0 },
            progressMax = extras.getInt("android.progressMax", -1).takeIf { it >= 0 },
        )
        if (nav.title.isNullOrBlank() && nav.distanceText.isNullOrBlank() && nav.progress == null) {
            return // not a navigation notification worth relaying
        }
        Log.d(TAG, "relay nav: title=${nav.title} dist=${nav.distanceText} progress=${nav.progress}/${nav.progressMax}")
        RelayStateHolder.updateNav(nav)
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (sbn.packageName == MAPS_PACKAGE) {
            RelayStateHolder.updateNav(null)
        }
    }
}

private fun Bundle.getCharSequence(key: String): CharSequence? =
    if (containsKey(key)) getCharSequence(key) else null
