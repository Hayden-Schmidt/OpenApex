package org.openapex.androidauto

import android.app.Activity
import android.bluetooth.le.ScanFilter
import android.companion.AssociationRequest
import android.companion.BluetoothLeDeviceFilter
import android.companion.CompanionDeviceManager
import android.content.Intent
import android.content.IntentSender
import android.os.Bundle
import android.os.Handler
import android.os.ParcelUuid
import android.provider.Settings
import android.util.Log

// First-run setup: request runtime permissions RelayService needs, then associate with the
// terminal via CompanionDeviceManager observer mode (so the OS wakes RelayService whenever the
// terminal is advertising, even from a killed app process), then send the user to grant
// notification listener access, since none of this can be requested from the CarAppService alone.
class MainActivity : Activity() {
    private val requiredPermissions = arrayOf(
        android.Manifest.permission.ACCESS_FINE_LOCATION,
        android.Manifest.permission.BLUETOOTH_CONNECT,
        android.Manifest.permission.BLUETOOTH_SCAN,
        android.Manifest.permission.POST_NOTIFICATIONS,
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i("OpenApexMain", "onCreate")
        requestPermissions(requiredPermissions, 0)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        Log.i("OpenApexMain", "permissions result: ${permissions.zip(grantResults.toList())}")
        associateWithTerminal()
    }

    private fun associateWithTerminal() {
        val deviceManager = getSystemService(CompanionDeviceManager::class.java)
        if (deviceManager == null) {
            finishSetup()
            return
        }
        val deviceFilter = BluetoothLeDeviceFilter.Builder()
            .setScanFilter(
                ScanFilter.Builder().setServiceUuid(ParcelUuid(RelayBleClient.SERVICE_UUID)).build(),
            )
            .build()
        val request = AssociationRequest.Builder()
            .addDeviceFilter(deviceFilter)
            .setSingleDevice(false)
            .build()
        // Handler-based overload (not the API 33+ Executor-based one) — this project's minSdk is 26.
        deviceManager.associate(
            request,
            object : CompanionDeviceManager.Callback() {
                override fun onAssociationPending(intentSender: IntentSender) {
                    startIntentSenderForResult(intentSender, REQUEST_ASSOCIATE, null, 0, 0, 0)
                }

                override fun onFailure(error: CharSequence?) {
                    Log.e("OpenApexMain", "CDM association failed: $error")
                    finishSetup()
                }
            },
            Handler(mainLooper),
        )
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_ASSOCIATE) {
            Log.i("OpenApexMain", "association result: resultCode=$resultCode")
            finishSetup()
        }
    }

    private fun finishSetup() {
        startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        finish()
    }

    companion object {
        private const val REQUEST_ASSOCIATE = 1
    }
}
