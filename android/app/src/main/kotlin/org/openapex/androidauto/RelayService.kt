package org.openapex.androidauto

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.ParcelUuid
import android.util.Log
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.location.Location
import android.os.Build
import android.os.IBinder
import android.os.Looper
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import com.google.android.gms.location.FusedLocationProviderClient
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority
import java.util.concurrent.atomic.AtomicInteger

/**
 * Foreground service owning the relay lifecycle: GNSS sampling, notification relay state, and
 * the BLE GATT client connection to the terminal. Publishes a new [RawNotifPacket] whenever any
 * input changes. Started by [OpenApexCompanionService] with the terminal's bonded device address
 * once CompanionDeviceManager observer mode sees it advertising.
 */
class RelayService : Service() {

    private lateinit var ble: RelayBleClient
    private lateinit var fused: FusedLocationProviderClient
    private lateinit var sensorManager: SensorManager
    private var locationCallback: LocationCallback? = null
    private var speedKmh: Float? = null
    private var headingDeg: Int? = null
    private var fixValid = false
    private var nav: RawNavNotification? = null
    private var listening = false
    private var connected = false
    private var scanning = false
    private var gnssStarted = false
    private val sequence = AtomicInteger(0)

    // Latest raw motion samples. Diagnostic/future-use passthrough only — never classified here;
    // see RawNotifPacket.kt KDoc and docs/OpenApex_SPEC.md §2.4.
    private var lastAccel: FloatArray? = null
    private var lastGyro: FloatArray? = null
    private val motionListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            when (event.sensor.type) {
                Sensor.TYPE_ACCELEROMETER -> lastAccel = event.values.copyOf()
                Sensor.TYPE_GYROSCOPE -> lastGyro = event.values.copyOf()
            }
        }
        override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {}
    }

    override fun onCreate() {
        super.onCreate()
        ble = RelayBleClient(this)
        fused = LocationServices.getFusedLocationProviderClient(this)
        sensorManager = getSystemService(Context.SENSOR_SERVICE) as SensorManager
        // Only the "connectedDevice" FGS type on first start: since API 34, background-starting a
        // "location" type foreground service (e.g. from BootCompletedReceiver) is restricted
        // unless specifically exempted, and boot isn't one of the exemptions (a CDM
        // presence-triggered start would be, but we can't rely on that firing - see
        // docs/OpenApex_SPEC.md §13 item 9). GNSS/location starts once actually BLE-connected,
        // when the service is already legitimately foregrounded.
        startForegroundWithNotification(ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        RelayStateHolder.attach { event -> handle(event) }
        startMotionSensors()
    }

    @SuppressLint("MissingPermission")
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val address = intent?.getStringExtra(EXTRA_DEVICE_ADDRESS)
        Log.i(TAG, "onStartCommand: address=$address")
        val adapter = (getSystemService(BluetoothManager::class.java))?.adapter
        if (address != null) {
            adapter?.getRemoteDevice(address)?.let { ble.connect(it) }
        } else {
            // No address (e.g. started from BootCompletedReceiver, not CompanionDeviceManager):
            // CDM cold-wake is unreliable on some OEMs (see docs/OpenApex_SPEC.md §13 item 9), so
            // this service runs continuously from boot and scans for the terminal itself instead
            // of waiting on a CDM presence callback that may never fire.
            startOwnScan(adapter)
        }
        return START_STICKY
    }

    @SuppressLint("MissingPermission")
    private fun startOwnScan(adapter: BluetoothAdapter?) {
        if (scanning) {
            Log.i(TAG, "startOwnScan: already scanning, skipping")
            return
        }
        val scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            Log.w(TAG, "startOwnScan: no BluetoothLeScanner available (adapter=$adapter)")
            return
        }
        val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(RelayBleClient.SERVICE_UUID)).build()
        val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
        scanning = true
        Log.i(TAG, "startOwnScan: starting scan for ${RelayBleClient.SERVICE_UUID}")
        scanner.startScan(listOf(filter), settings, scanCallback)
    }

    @SuppressLint("MissingPermission")
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            Log.i(TAG, "onScanResult: found ${result.device.address}")
            val adapter = (getSystemService(BluetoothManager::class.java))?.adapter ?: return
            adapter.bluetoothLeScanner?.stopScan(this)
            scanning = false
            ble.connect(result.device)
        }

        override fun onScanFailed(errorCode: Int) {
            Log.w(TAG, "onScanFailed: errorCode=$errorCode")
            scanning = false
        }
    }

    override fun onDestroy() {
        stopGnss()
        sensorManager.unregisterListener(motionListener)
        if (scanning) {
            @SuppressLint("MissingPermission")
            (getSystemService(BluetoothManager::class.java))?.adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        }
        ble.disconnect()
        RelayStateHolder.detach()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun handle(event: RelayStateEvent) {
        when (event) {
            is RelayStateEvent.NavUpdated -> nav = event.nav
            is RelayStateEvent.ListenerConnected -> listening = true
            is RelayStateEvent.ListenerDisconnected -> listening = false
            is RelayStateEvent.BleConnected -> {
                connected = true
                if (!gnssStarted) {
                    try {
                        startForegroundWithNotification(
                            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or
                                ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION,
                        )
                        startGnss()
                        gnssStarted = true
                    } catch (e: SecurityException) {
                        // Background FGS-location-start restriction (API 34+): if this connect was
                        // triggered from a killed/background process (boot fallback, CDM cold-wake),
                        // the app may not yet hold the exemption needed to promote to the "location"
                        // FGS type. Leave gnssStarted false so the next BleConnected retries GNSS
                        // once the app has been foregrounded by the user; BLE relay still works.
                        Log.w(TAG, "startGnss: location FGS promotion denied, retrying on next connect", e)
                    }
                }
            }
            is RelayStateEvent.BleDisconnected -> {
                connected = false
                startOwnScan((getSystemService(BluetoothManager::class.java))?.adapter)
            }
        }
        publish()
    }

    private fun publish() {
        val currentNav = nav ?: return
        val telemetry = GnssTelemetry(
            speedKmh = speedKmh,
            headingDeg = headingDeg,
            fixValid = fixValid,
            batteryPercent = batteryPercent(),
        )
        val motion = MotionTelemetry(accelMs2 = lastAccel, gyroRadS = lastGyro)
        val packet = packRawNotifPacket(sequence.incrementAndGet(), currentNav, telemetry, motion)
        Log.i(TAG, "publish: connected=$connected packetLen=${packet.size}")
        if (connected) {
            ble.write(packet)
        }
    }

    @SuppressLint("MissingPermission")
    private fun startGnss() {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) return
        val request = LocationRequest.Builder(Priority.PRIORITY_HIGH_ACCURACY, 1000L)
            .setMinUpdateIntervalMillis(500L)
            .build()
        locationCallback = object : LocationCallback() {
            override fun onLocationResult(result: LocationResult) {
                val loc: Location? = result.lastLocation
                if (loc != null) {
                    speedKmh = if (loc.hasSpeed()) loc.speed * 3.6f else null
                    headingDeg = if (loc.hasBearing()) loc.bearing.toInt().mod(360) else null
                    fixValid = loc.hasSpeed() || loc.hasBearing()
                    publish()
                }
            }
        }
        fused.requestLocationUpdates(request, locationCallback!!, Looper.getMainLooper())
    }

    private fun stopGnss() {
        locationCallback?.let { fused.removeLocationUpdates(it) }
        locationCallback = null
    }

    private fun startMotionSensors() {
        val accel = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val gyro = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        accel?.let { sensorManager.registerListener(motionListener, it, SensorManager.SENSOR_DELAY_GAME) }
        gyro?.let { sensorManager.registerListener(motionListener, it, SensorManager.SENSOR_DELAY_GAME) }
    }

    private fun batteryPercent(): Int? {
        val bm = getSystemService(BATTERY_SERVICE) as android.os.BatteryManager
        return bm.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY).takeIf { it in 0..100 }
    }

    private fun startForegroundWithNotification(type: Int) {
        val channelId = "relay"
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(channelId, "Relay", NotificationManager.IMPORTANCE_LOW),
            )
        }
        val intent = packageManager.getLaunchIntentForPackage(packageName)
        val pi = PendingIntent.getActivity(this, 0, intent, PendingIntent.FLAG_IMMUTABLE)
        val notification: Notification = NotificationCompat.Builder(this, channelId)
            .setContentTitle("OpenApex relay")
            .setContentText("Relaying navigation to the terminal")
            .setSmallIcon(android.R.drawable.ic_menu_compass)
            .setContentIntent(pi)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(1, notification, type)
        } else {
            startForeground(1, notification)
        }
    }

    companion object {
        private const val TAG = "RelayService"
        const val EXTRA_DEVICE_ADDRESS = "org.openapex.androidauto.extra.DEVICE_ADDRESS"
    }
}
