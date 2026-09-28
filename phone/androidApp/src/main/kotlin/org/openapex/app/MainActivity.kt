package org.openapex.app

import android.app.Activity
import android.bluetooth.le.ScanFilter
import android.companion.AssociationRequest
import android.companion.BluetoothLeDeviceFilter
import android.companion.CompanionDeviceManager
import android.companion.ObservingDevicePresenceRequest
import android.content.Intent
import android.content.IntentSender
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
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
        // Optional: only the fallback for answering a carrier call whose notification has no
        // answer action (CallRelay.acceptViaTelecom). Declining it leaves call display working.
        android.Manifest.permission.ANSWER_PHONE_CALLS,
        // Optional: resolves a saved contact's number for the call page (CallRelay.contactNumber).
        // Declining it shows the contact name alone.
        android.Manifest.permission.READ_CONTACTS,
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i("OpenApexMain", "onCreate")
        requestPermissions(requiredPermissions, REQUEST_FOREGROUND_PERMISSIONS)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        Log.i("OpenApexMain", "permissions result: ${permissions.zip(grantResults.toList())}")
        when (requestCode) {
            REQUEST_FOREGROUND_PERMISSIONS -> requestBackgroundLocation()
            REQUEST_BACKGROUND_LOCATION -> onBackgroundLocationResult()
            else -> associateWithTerminal()
        }
    }

    /**
     * Second-stage request for ACCESS_BACKGROUND_LOCATION, which decides whether the rider gets a
     * compass at all.
     *
     * Without it, the OS treats location as foreground-only, and RelayService — which is normally
     * started from the background by CDM presence or the boot fallback — cannot promote itself to
     * the "location" foreground-service type. The promotion throws, speed/heading never arrive,
     * and the terminal shows a dead compass ring for the whole ride. The 2026-09-24 capture had
     * four sessions like that and nothing on screen said why.
     *
     * Android 11+ refuses to grant it in the same dialog as the foreground permissions, and from
     * API 30 the system dialog for it is one-shot: once the user has answered, requestPermissions
     * returns immediately without showing anything. So ask once, and thereafter send them to the
     * settings page where "Allow all the time" actually lives, rather than firing an invisible
     * request and silently carrying on degraded.
     */
    private fun requestBackgroundLocation() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            associateWithTerminal()
            return
        }
        if (hasBackgroundLocation()) {
            associateWithTerminal()
            return
        }
        // Always ask first, and decide from the RESULT rather than from
        // shouldShowRequestPermissionRationale(). That flag is false both before the first ask and
        // after a permanent denial, so branching on it sends a first-run user straight to Settings
        // and they never see the dialog at all. Asking is harmless when the dialog is spent --
        // requestPermissions() just returns the existing answer immediately.
        requestPermissions(arrayOf(BACKGROUND_LOCATION), REQUEST_BACKGROUND_LOCATION)
    }

    /** Called with the outcome of the background-location ask; Settings is the fallback. */
    private fun onBackgroundLocationResult() {
        if (!hasBackgroundLocation()) {
            // Either denied, or the one-shot dialog was already spent and nothing was shown.
            // "Allow all the time" only exists in Settings, so that is the only place left to go.
            Log.w("OpenApexMain", "background location not granted; opening settings")
            openAppSettings()
        }
        associateWithTerminal()
    }

    private fun hasBackgroundLocation(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.Q ||
            checkSelfPermission(BACKGROUND_LOCATION) == PackageManager.PERMISSION_GRANTED

    private fun openAppSettings() {
        startActivity(
            Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
                .setData(Uri.fromParts("package", packageName, null)),
        )
    }

    private fun associateWithTerminal() {
        val deviceManager = getSystemService(CompanionDeviceManager::class.java)
        if (deviceManager == null) {
            finishSetup()
            return
        }
        // Re-running associate() on every launch piled up duplicate associations for the same
        // terminal (six seen on 2026-09-27), each adding its own CDM presence scan filter. Reuse
        // the existing one and just kick RelayService straight at it.
        val existing = existingTerminalAddress(deviceManager)
        if (existing != null) {
            Log.i("OpenApexMain", "already associated with $existing, skipping association")
            startForegroundService(
                Intent(this, RelayService::class.java)
                    .putExtra(RelayService.EXTRA_DEVICE_ADDRESS, existing),
            )
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

    // Android 16 (API 36) deprecated startObservingDevicePresence(String) in favor of the
    // ObservingDevicePresenceRequest overload, and on-device testing found the old overload's
    // callbacks silently never fire on API 36 (see docs/OpenApex_SPEC.md §13 item 9). Both paths
    // are registered here so minSdk 26..35 devices keep working via the legacy overload while
    // API 36+ devices use the one that's actually reliable.
    @Suppress("DEPRECATION") // getAssociations()/startObservingDevicePresence(String) are the pre-36 APIs
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQUEST_ASSOCIATE) {
            Log.i("OpenApexMain", "association result: resultCode=$resultCode")
            // associate() alone does not enable presence callbacks; each associated device's
            // presence must be observed explicitly, and this registration persists with the
            // association (survives app kill/reboot), so it only needs to run once.
            val deviceManager = getSystemService(CompanionDeviceManager::class.java)
            if (deviceManager != null) {
                if (Build.VERSION.SDK_INT >= 36) {
                    deviceManager.myAssociations.forEach { info ->
                        try {
                            deviceManager.startObservingDevicePresence(
                                ObservingDevicePresenceRequest.Builder()
                                    .setAssociationId(info.id)
                                    .build(),
                            )
                            Log.i("OpenApexMain", "observing device presence: associationId=${info.id}")
                        } catch (e: IllegalArgumentException) {
                            Log.w("OpenApexMain", "already observing associationId=${info.id}")
                        }
                    }
                } else {
                    deviceManager.associations.forEach { mac ->
                        try {
                            deviceManager.startObservingDevicePresence(mac)
                            Log.i("OpenApexMain", "observing device presence: $mac")
                        } catch (e: IllegalArgumentException) {
                            Log.w("OpenApexMain", "already observing $mac")
                        }
                    }
                }
            }
            finishSetup()
        }
    }

    /** Uppercase MAC of an existing association (dropping duplicates on API 33+), or null. */
    @Suppress("DEPRECATION") // getAssociations() is the pre-33 API
    private fun existingTerminalAddress(deviceManager: CompanionDeviceManager): String? {
        if (Build.VERSION.SDK_INT < 33) return deviceManager.associations.firstOrNull()?.uppercase()
        val associations = deviceManager.myAssociations.filter { it.deviceMacAddress != null }
        associations.groupBy { it.deviceMacAddress }.values.forEach { sameMac ->
            sameMac.sortedByDescending { it.id }.drop(1).forEach { dup ->
                Log.i("OpenApexMain", "removing duplicate association id=${dup.id}")
                deviceManager.disassociate(dup.id)
            }
        }
        // MacAddress.toString() is lowercase; BluetoothAdapter.getRemoteDevice() needs uppercase.
        return associations.maxByOrNull { it.id }?.deviceMacAddress?.toString()?.uppercase()
    }

    private fun finishSetup() {
        startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        finish()
    }

    companion object {
        private const val REQUEST_ASSOCIATE = 1
        private const val REQUEST_FOREGROUND_PERMISSIONS = 2
        private const val REQUEST_BACKGROUND_LOCATION = 3
        private const val BACKGROUND_LOCATION = android.Manifest.permission.ACCESS_BACKGROUND_LOCATION
    }
}
