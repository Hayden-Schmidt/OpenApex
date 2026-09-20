package org.openapex.androidauto

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.util.Log

// First-run setup: request runtime permissions RelayService needs, then send the user to grant
// notification listener access, since neither can be requested from the CarAppService alone.
class MainActivity : Activity() {
    private val requiredPermissions = arrayOf(
        android.Manifest.permission.ACCESS_FINE_LOCATION,
        android.Manifest.permission.BLUETOOTH_CONNECT,
        android.Manifest.permission.BLUETOOTH_ADVERTISE,
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
        try {
            startForegroundService(Intent(this, RelayService::class.java))
        } catch (e: Exception) {
            Log.e("OpenApexMain", "startForegroundService(RelayService) failed", e)
        }
        startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        finish()
    }
}
