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
            // MacAddress.toString() is always lowercase; BluetoothAdapter.getRemoteDevice()
            // throws IllegalArgumentException on anything but uppercase hex, which was crashing
            // RelayService.onStartCommand (and the whole app process) on every CDM presence event.
            ?.toString()
            ?.uppercase()
    }

    private fun relayStart(deviceAddress: String) {
        startForegroundService(
            Intent(this, RelayService::class.java)
                .putExtra(RelayService.EXTRA_DEVICE_ADDRESS, deviceAddress),
        )
    }

    /**
     * A CDM "disappeared" event must NOT tear the relay down.
     *
     * It used to call stopService(), and the 2026-09-24 capture shows what that cost: a momentary
     * BLE drop produced bleDisconnected -> serviceDestroy within 35 ms, then an immediate restart
     * from the background on the next "appeared". Seven service lifetimes in 70 minutes, and each
     * restart was a fresh chance to lose two things permanently for the rest of the ride:
     *
     *   - GNSS: a background-started service cannot promote itself to the "location" FGS type
     *     without ACCESS_BACKGROUND_LOCATION, so speed/heading/compass stayed dead (4 of 7
     *     sessions had zero telemetry).
     *   - MTU: the fresh GATT connection renegotiates, and where it didn't, every 146-byte packet
     *     was truncated to 20 bytes and rejected by the terminal (3 of 7 sessions decoded nothing).
     *
     * CDM presence is a hint that the terminal is out of radio range, not an instruction to
     * discard state. RelayService already re-scans and reconnects itself on BLE disconnect, so the
     * correct response here is to let it keep running: it holds the GNSS stream, the location FGS
     * promotion and the packet sequence across the gap, and picks the link back up when the
     * terminal returns. The service is stopped only by the user, or by the OS.
     */
    private fun relayStop() {
        Log.i(TAG, "terminal out of range; leaving RelayService running to reconnect itself")
    }

    companion object {
        private const val TAG = "OpenApexCompanion"
    }
}
