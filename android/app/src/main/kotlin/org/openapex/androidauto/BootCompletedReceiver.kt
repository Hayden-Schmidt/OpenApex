package org.openapex.androidauto

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * Starts [RelayService] on boot so the phone reconnects to the terminal without depending on
 * CompanionDeviceManager's cold-process wake callback, which testing found unreliable on some
 * OEMs (see docs/OpenApex_SPEC.md §13 item 9). RelayService scans for the terminal itself when
 * started with no device address.
 */
class BootCompletedReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_BOOT_COMPLETED) return
        Log.i(TAG, "boot completed, starting RelayService")
        context.startForegroundService(Intent(context, RelayService::class.java))
    }

    companion object {
        private const val TAG = "OpenApexBootReceiver"
    }
}
