package org.openapex.androidauto

import android.Manifest
import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
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
 * the BLE GATT server. Publishes a new [RawNotifPacket] whenever any input changes.
 */
class RelayService : Service() {

    private lateinit var ble: RelayBleServer
    private lateinit var fused: FusedLocationProviderClient
    private var locationCallback: LocationCallback? = null
    private var speedKmh: Float? = null
    private var headingDeg: Int? = null
    private var fixValid = false
    private var nav: RawNavNotification? = null
    private var listening = false
    private var subscriberCount = 0
    private val sequence = AtomicInteger(0)
    private var latestPacket: ByteArray = ByteArray(RAW_NOTIF_PACKET_SIZE)

    override fun onCreate() {
        super.onCreate()
        ble = RelayBleServer(this)
        fused = LocationServices.getFusedLocationProviderClient(this)
        startForegroundWithNotification()
        RelayStateHolder.attach { event -> handle(event) }
        ble.start()
        startGnss()
    }

    override fun onDestroy() {
        stopGnss()
        ble.stop()
        RelayStateHolder.detach()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun handle(event: RelayStateEvent) {
        when (event) {
            is RelayStateEvent.NavUpdated -> nav = event.nav
            is RelayStateEvent.ListenerConnected -> listening = true
            is RelayStateEvent.ListenerDisconnected -> listening = false
            is RelayStateEvent.BleConnected -> subscriberCount += 1
            is RelayStateEvent.BleDisconnected -> subscriberCount = 0
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
        val packet = packRawNotifPacket(sequence.incrementAndGet(), currentNav, telemetry)
        latestPacket = packet
        RelayStateHolder.setLatestPacket(packet)
        if (subscriberCount > 0) {
            ble.notify(packet)
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

    private fun batteryPercent(): Int? {
        val bm = getSystemService(BATTERY_SERVICE) as android.os.BatteryManager
        return bm.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY).takeIf { it in 0..100 }
    }

    private fun startForegroundWithNotification() {
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
        startForeground(1, notification)
    }
}
