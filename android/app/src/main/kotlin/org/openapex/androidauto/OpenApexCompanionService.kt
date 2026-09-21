package org.openapex.androidauto

import android.companion.CompanionDeviceService
import android.content.Intent
import android.util.Log

/**
 * CompanionDeviceManager observer-mode hook: the OS scans for the terminal's bonded BLE
 * advertisement (see MainActivity's association request) and wakes this service — including from
 * a fully killed app process — whenever the terminal appears or disappears. This is what gives
 * the "bike on -> device boots -> phone reconnects" flow without the user opening the app.
 */
class OpenApexCompanionService : CompanionDeviceService() {

    @Suppress("OVERRIDE_DEPRECATION") // onDeviceAppeared(String) is the only signature available on minSdk 26
    override fun onDeviceAppeared(deviceAddress: String) {
        Log.i(TAG, "terminal appeared: $deviceAddress")
        startForegroundService(
            Intent(this, RelayService::class.java)
                .putExtra(RelayService.EXTRA_DEVICE_ADDRESS, deviceAddress),
        )
    }

    @Suppress("OVERRIDE_DEPRECATION") // onDeviceDisappeared(String) is the only signature available on minSdk 26
    override fun onDeviceDisappeared(deviceAddress: String) {
        Log.i(TAG, "terminal disappeared: $deviceAddress")
        stopService(Intent(this, RelayService::class.java))
    }

    companion object {
        private const val TAG = "OpenApexCompanion"
    }
}
