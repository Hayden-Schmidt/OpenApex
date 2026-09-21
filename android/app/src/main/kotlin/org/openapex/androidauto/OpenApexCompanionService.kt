package org.openapex.androidauto

import android.companion.CompanionDeviceManager
import android.companion.CompanionDeviceService
import android.companion.DevicePresenceEvent
import android.content.Intent
import android.os.Build
import android.util.Log

/**
 * CompanionDeviceManager observer-mode hook: the OS scans for the terminal's bonded BLE
 * advertisement (see MainActivity's association request) and wakes this service — including from
 * a fully killed app process — whenever the terminal appears or disappears. This is what gives
 * the "bike on -> device boots -> phone reconnects" flow without the user opening the app.
 *
 * Android 16 (API 36) deprecated the String-address callbacks below in favor of
 * onDevicePresenceEvent(DevicePresenceEvent), and on-device testing on a real API 36 phone (see
 * docs/OpenApex_SPEC.md §13 item 9) found the old callbacks silently never fire on that OS
 * version. Both are implemented: the new callback runs on API 36+, the old ones remain for
 * minSdk 26..35 devices where onDevicePresenceEvent is never invoked by the OS.
 */
class OpenApexCompanionService : CompanionDeviceService() {

    @Suppress("OVERRIDE_DEPRECATION") // onDeviceAppeared(String) is the only signature on API < 36
    override fun onDeviceAppeared(deviceAddress: String) {
        Log.i(TAG, "terminal appeared (legacy callback): $deviceAddress")
        relayStart(deviceAddress)
    }

    @Suppress("OVERRIDE_DEPRECATION") // onDeviceDisappeared(String) is the only signature on API < 36
    override fun onDeviceDisappeared(deviceAddress: String) {
        Log.i(TAG, "terminal disappeared (legacy callback): $deviceAddress")
        relayStop()
    }

    @SuppressWarnings("MissingPermission")
    override fun onDevicePresenceEvent(event: DevicePresenceEvent) {
        Log.i(TAG, "presence event: associationId=${event.associationId} event=${event.event}")
        when (event.event) {
            DevicePresenceEvent.EVENT_BLE_APPEARED -> {
                val mac = associationIdToMac(event.associationId)
                Log.i(TAG, "terminal appeared (API 36 callback): $mac")
                if (mac != null) relayStart(mac)
            }
            DevicePresenceEvent.EVENT_BLE_DISAPPEARED -> {
                Log.i(TAG, "terminal disappeared (API 36 callback)")
                relayStop()
            }
        }
    }

    private fun associationIdToMac(associationId: Int): String? {
        val deviceManager = getSystemService(CompanionDeviceManager::class.java) ?: return null
        return deviceManager.myAssociations
            .firstOrNull { it.id == associationId }
            ?.deviceMacAddress
            ?.toString()
    }

    private fun relayStart(deviceAddress: String) {
        startForegroundService(
            Intent(this, RelayService::class.java)
                .putExtra(RelayService.EXTRA_DEVICE_ADDRESS, deviceAddress),
        )
    }

    private fun relayStop() {
        stopService(Intent(this, RelayService::class.java))
    }

    companion object {
        private const val TAG = "OpenApexCompanion"
    }
}
