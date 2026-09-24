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
import android.net.Uri
import android.provider.Settings
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
    private val handler by lazy { android.os.Handler(Looper.getMainLooper()) }
    private val gnssRetry = Runnable { tryStartGnss() }

    // Identifies this object in the drive log. Android can leave an older instance alive while a
    // new one starts; both write to the same RelayRecorder singleton, so without this the two
    // streams are indistinguishable. See the instanceId note in RelayRecorder.telemetry().
    private val instanceId = instanceCounter.incrementAndGet()

    // Latest raw motion samples. Diagnostic/future-use passthrough only — never classified here;
    // see RawNotifPacket.kt KDoc and docs/OpenApex_SPEC.md §2.4.
    private var lastAccel: FloatArray? = null
    private var lastGyro: FloatArray? = null

    // Raw heading-fusion inputs, recorded but NOT yet used to drive the display. The fusion filter
    // itself is planned for the terminal, not here -- see docs/Heading_Sensor_Fusion_Plan.md. These
    // exist so the filter can be developed and tuned against recorded rides instead of by guessing.
    private var lastYawDeg: Float? = null       // phone yaw from TYPE_ROTATION_VECTOR, tilt-compensated
    private var lastPitchDeg: Float? = null
    private var lastRollDeg: Float? = null
    private var lastYawRateDps: Float? = null   // gyro Z, the axis the compass actually turns on
    private var lastOrientationLogMs = 0L
    private val rotationMatrix = FloatArray(9)
    private val orientation = FloatArray(3)
    private val motionListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            when (event.sensor.type) {
                Sensor.TYPE_ACCELEROMETER -> lastAccel = event.values.copyOf()
                Sensor.TYPE_GYROSCOPE -> {
                    lastGyro = event.values.copyOf()
                    lastYawRateDps = Math.toDegrees(event.values[2].toDouble()).toFloat()
                }
                Sensor.TYPE_ROTATION_VECTOR -> {
                    SensorManager.getRotationMatrixFromVector(rotationMatrix, event.values)
                    SensorManager.getOrientation(rotationMatrix, orientation)
                    lastYawDeg = Math.toDegrees(orientation[0].toDouble()).toFloat().mod(360f)
                    lastPitchDeg = Math.toDegrees(orientation[1].toDouble()).toFloat()
                    lastRollDeg = Math.toDegrees(orientation[2].toDouble()).toFloat()
                    // Throttled to ~10Hz: fast enough to resolve jacket flap (a few Hz), slow
                    // enough not to flood the log. The sensor itself runs at GAME rate (~50Hz).
                    val now = android.os.SystemClock.elapsedRealtime()
                    if (now - lastOrientationLogMs >= ORIENTATION_LOG_MS) {
                        lastOrientationLogMs = now
                        RelayRecorder.orientation(
                            lastYawDeg ?: 0f, lastPitchDeg ?: 0f, lastRollDeg ?: 0f, lastYawRateDps,
                        )
                    }
                }
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
        // Dev-only drive capture; no-ops entirely in a release build (RelayRecorder.enabled).
        RelayRecorder.start(this)
        RelayRecorder.lifecycle("serviceCreate")
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
        // A restart from MainActivity means the app is foregrounded, which is exactly the state
        // that lifts the API 34+ background location-FGS restriction — retake the GNSS attempt
        // now rather than waiting out the retry timer.
        tryStartGnss()
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
        handler.removeCallbacks(gnssRetry)
        RelayRecorder.lifecycle("serviceDestroy")
        RelayRecorder.stop()
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
            is RelayStateEvent.ListenerConnected -> {
                listening = true
                RelayRecorder.lifecycle("listenerConnected")
            }
            is RelayStateEvent.ListenerDisconnected -> {
                listening = false
                RelayRecorder.lifecycle("listenerDisconnected")
            }
            is RelayStateEvent.BleConnected -> {
                connected = true
                RelayRecorder.lifecycle("bleConnected")
                tryStartGnss()
            }
            is RelayStateEvent.BleDisconnected -> {
                connected = false
                RelayRecorder.lifecycle("bleDisconnected", "status=${event.status}")
                startOwnScan((getSystemService(BluetoothManager::class.java))?.adapter)
            }
        }
        publish()
    }

    private fun publish() {
        // No active navigation is NOT a reason to stop transmitting. The terminal is a compass and
        // speed display first and a nav display second: gating the whole packet on `nav` meant the
        // compass died whenever Maps wasn't routing, which is most riding. Send telemetry with an
        // empty maneuver instead, and let the terminal decide what to show (a packet with no title,
        // no distance and no progress normalizes to VIEW_IDLE, which is the compass view).
        val currentNav = nav ?: EMPTY_NAV
        val telemetry = GnssTelemetry(
            speedKmh = speedKmh,
            headingDeg = headingDeg,
            fixValid = fixValid,
            batteryPercent = batteryPercent(),
        )
        val motion = MotionTelemetry(accelMs2 = lastAccel, gyroRadS = lastGyro)
        val seq = sequence.incrementAndGet()
        val packet = packRawNotifPacket(seq, currentNav, telemetry, motion)
        Log.i(TAG, "publish: connected=$connected packetLen=${packet.size}")
        // Recorded before the write, and regardless of `connected`: a packet the phone built but
        // could not send is exactly the case the ESP-side log cannot show on its own.
        RelayRecorder.packet(seq, packet, connected)
        if (connected) {
            ble.write(packet)
        }
    }

    /**
     * Promotes the foreground service to the "location" type and starts GNSS sampling, retrying
     * on a timer until it succeeds.
     *
     * Background FGS-location-start restriction (API 34+): when the connect was triggered from a
     * killed/background process (boot fallback, CDM cold-wake), the app may not yet hold the
     * exemption needed for the "location" type and the promotion throws. The old code gave up
     * until the *next* BleConnected, which on a normal drive never comes — the terminal stays
     * connected the whole time. That is why speed/heading (and with them the compass ring, and
     * the distance label via the staleness path) were missing on most boots and present only on
     * the few where the user had happened to open the app first. Retry on a timer instead; the
     * BLE relay keeps working throughout either way.
     */
    private fun tryStartGnss() {
        if (gnssStarted || !connected) return
        try {
            startForegroundWithNotification(
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION,
            )
            if (!startGnss()) {
                // Location permission not granted yet — nothing to retry on a timer, it needs the
                // user. Next BleConnected/onStartCommand (i.e. after they grant it) tries again.
                Log.w(TAG, "tryStartGnss: ACCESS_FINE_LOCATION not granted")
                RelayRecorder.lifecycle("gnssPermissionMissing")
                return
            }
            gnssStarted = true
            Log.i(TAG, "tryStartGnss: GNSS started")
            RelayRecorder.lifecycle("gnssStarted")
            clearDegradedNotification()
        } catch (e: SecurityException) {
            // Two very different failures land here, and the 2026-09-24 capture showed the cost of
            // treating them the same: 30-second retries, denied identically every time, for a whole
            // ride, with nothing on screen or in the notification saying the compass was dead.
            //
            //  - ACCESS_BACKGROUND_LOCATION missing: PERMANENT while the service runs from the
            //    background. The OS treats location as foreground-only, so no amount of retrying
            //    will ever promote this service. Only the user can fix it, in Settings.
            //  - Permission held, but the process is momentarily not in an eligible state: genuinely
            //    transient, and the timer is the right answer.
            val backgroundLocationMissing = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q &&
                ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_BACKGROUND_LOCATION) !=
                PackageManager.PERMISSION_GRANTED
            if (backgroundLocationMissing) {
                Log.e(TAG, "tryStartGnss: ACCESS_BACKGROUND_LOCATION not granted — no speed/heading/compass this ride", e)
                RelayRecorder.lifecycle("gnssBackgroundPermissionMissing", e.message)
                showDegradedNotification()
                // Still retry, but slowly: the user may grant it mid-ride, and a 30-second hammer
                // on a permission only a human can change is just wasted wakeups.
                handler.removeCallbacks(gnssRetry)
                handler.postDelayed(gnssRetry, GNSS_PERMISSION_RETRY_MS)
                return
            }
            Log.w(TAG, "tryStartGnss: location FGS promotion denied, retrying in ${GNSS_RETRY_MS}ms", e)
            RelayRecorder.lifecycle("gnssPromotionDenied", e.message)
            handler.removeCallbacks(gnssRetry)
            handler.postDelayed(gnssRetry, GNSS_RETRY_MS)
        }
    }

    /**
     * A separate, high-importance notification for "the relay is running but the compass is dead".
     *
     * Deliberately NOT folded into the ongoing relay notification, which is IMPORTANCE_LOW and by
     * design says nothing interesting — the rider would never see it. This one is meant to be
     * noticed before setting off, and taps through to the settings page that can fix it.
     */
    private fun showDegradedNotification() {
        val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(
                    DEGRADED_CHANNEL_ID,
                    "Relay problems",
                    NotificationManager.IMPORTANCE_HIGH,
                ),
            )
        }
        val settings = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS)
            .setData(Uri.fromParts("package", packageName, null))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        val pi = PendingIntent.getActivity(this, 1, settings, PendingIntent.FLAG_IMMUTABLE)
        nm.notify(
            DEGRADED_NOTIFICATION_ID,
            NotificationCompat.Builder(this, DEGRADED_CHANNEL_ID)
                .setContentTitle("No speed or compass")
                .setContentText("Set Location to \"Allow all the time\" for OpenApex")
                .setStyle(
                    NotificationCompat.BigTextStyle().bigText(
                        "OpenApex only has location while the app is open, so the terminal gets no " +
                            "speed, heading or compass while riding. Tap to open Settings and choose " +
                            "\"Allow all the time\".",
                    ),
                )
                .setSmallIcon(android.R.drawable.stat_sys_warning)
                .setPriority(NotificationCompat.PRIORITY_HIGH)
                .setCategory(NotificationCompat.CATEGORY_ERROR)
                .setContentIntent(pi)
                .setAutoCancel(true)
                .build(),
        )
    }

    private fun clearDegradedNotification() {
        (getSystemService(NOTIFICATION_SERVICE) as NotificationManager).cancel(DEGRADED_NOTIFICATION_ID)
    }

    @SuppressLint("MissingPermission")
    private fun startGnss(): Boolean {
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) return false
        // Drop any callback this instance already has registered before requesting another. The
        // gnssStarted flag guards the common path, but it is set only after this function returns,
        // and requestLocationUpdates with a fresh callback object registers a *second* stream
        // rather than replacing the first -- one duplicated fix per second, forever.
        stopGnss()
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
                    // Log the RAW inputs separately, never pre-fused: the terminal-side filter
                    // needs GPS course and phone yaw as distinct quantities in distinct frames.
                    RelayRecorder.telemetry(
                        speedKmh = speedKmh,
                        headingDeg = headingDeg,
                        fixValid = fixValid,
                        batteryPercent = batteryPercent(),
                        bearingDeg = if (loc.hasBearing()) loc.bearing else null,
                        bearingAccuracyDeg = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
                            loc.hasBearingAccuracy()) loc.bearingAccuracyDegrees else null,
                        speedAccuracyMps = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O &&
                            loc.hasSpeedAccuracy()) loc.speedAccuracyMetersPerSecond else null,
                        accuracyM = if (loc.hasAccuracy()) loc.accuracy else null,
                        hasBearing = loc.hasBearing(),
                        hasSpeed = loc.hasSpeed(),
                        yawDeg = lastYawDeg,
                        pitchDeg = lastPitchDeg,
                        rollDeg = lastRollDeg,
                        yawRateDps = lastYawRateDps,
                        accel = lastAccel,
                        gyro = lastGyro,
                        latDeg = loc.latitude,
                        lonDeg = loc.longitude,
                        altitudeM = if (loc.hasAltitude()) loc.altitude else null,
                        fixElapsedRealtimeNanos = loc.elapsedRealtimeNanos,
                        instanceId = instanceId,
                    )
                    publish()
                }
            }
        }
        fused.requestLocationUpdates(request, locationCallback!!, Looper.getMainLooper())
        return true
    }

    private fun stopGnss() {
        locationCallback?.let { fused.removeLocationUpdates(it) }
        locationCallback = null
    }

    private fun startMotionSensors() {
        val accel = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        val gyro = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
        // Android-fused and tilt-compensated -- the hard part on a leaning motorcycle. Raw
        // magnetometer is deliberately not used; see docs/Heading_Sensor_Fusion_Plan.md.
        val rotation = sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
        accel?.let { sensorManager.registerListener(motionListener, it, SensorManager.SENSOR_DELAY_GAME) }
        gyro?.let { sensorManager.registerListener(motionListener, it, SensorManager.SENSOR_DELAY_GAME) }
        rotation?.let { sensorManager.registerListener(motionListener, it, SensorManager.SENSOR_DELAY_GAME) }
        Log.i(TAG, "startMotionSensors: accel=${accel != null} gyro=${gyro != null} rotationVector=${rotation != null}")
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
        private const val ORIENTATION_LOG_MS = 100L

        /** Process-wide, so a second instance gets a different id than the one it overlapped with. */
        private val instanceCounter = AtomicInteger(0)

        /**
         * Stand-in maneuver for "riding, but not navigating". Every field null so the terminal's
         * normalizer sees no title, no distance and no progress and settles on its idle/compass
         * view -- no fabricated zeros, matching the sentinel discipline in RawNotifPacket.
         */
        private val EMPTY_NAV = RawNavNotification(
            title = null,
            etaText = null,
            distanceText = null,
            progress = null,
            progressMax = null,
            iconRotationDeg = null,
        )
        const val EXTRA_DEVICE_ADDRESS = "org.openapex.androidauto.extra.DEVICE_ADDRESS"
        // Retry cadence for the location-FGS promotion. Long enough not to churn, short enough
        // that GNSS is live well inside a drive if the exemption arrives late.
        private const val GNSS_RETRY_MS = 30_000L

        /** Only a human can grant background location, so poll for it slowly, not every 30 s. */
        private const val GNSS_PERMISSION_RETRY_MS = 5 * 60_000L
        private const val DEGRADED_CHANNEL_ID = "relay_degraded"
        private const val DEGRADED_NOTIFICATION_ID = 2
    }
}
