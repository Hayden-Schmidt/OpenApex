package org.openapex.androidauto

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Drive recorder — DEVELOPMENT ONLY.
 *
 * Captures what the phone actually read from the notification and what it sent over BLE, to a
 * JSONL file in app-private external storage, so a drive can be taken with no laptop attached and
 * the whole session pulled afterwards:
 *
 *     adb pull /sdcard/Android/data/org.openapex.androidauto/files/drivelogs/
 *     python tools/decode_drive.py esp_log.bin --phone drive-<timestamp>.jsonl
 *
 * The terminal writes its own log to flash (firmware/main/drive_log.h); the two are joined on the
 * packet `seq`, which both sides record. The phone supplies wall-clock time and the terminal
 * supplies uptime, so a merged timeline needs no clock sync.
 *
 * Why the notification is captured wholesale rather than just the fields we parse: moving to Waze
 * and Apple Maps means not yet knowing which extras matter. Recording only today's known keys
 * would make the first Waze drive tell us nothing we didn't already ask for — and cost another
 * drive. [NavNotificationRelayService] is likewise package-agnostic about what it records.
 *
 * GATED OFF IN RELEASE BUILDS. [enabled] is `BuildConfig.DEBUG`, so every call below returns
 * immediately in a production build and R8 strips the machinery. This writes the rider's route,
 * addresses and timestamps to disk — it must never ship on.
 */
object RelayRecorder {

    private const val TAG = "RelayRecorder"
    private const val DIR_NAME = "drivelogs"
    private const val MAX_FILE_BYTES = 8L * 1024 * 1024
    private const val MAX_FILES = 6

    /** The single production gate. Everything else in this file is unreachable when false. */
    val enabled: Boolean get() = BuildConfig.DEBUG

    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var file: File? = null
    private var dir: File? = null
    private var started = false

    @Synchronized
    fun start(context: Context) {
        if (!enabled || started) return
        val logDir = File(context.getExternalFilesDir(null), DIR_NAME)
        if (!logDir.exists() && !logDir.mkdirs()) {
            Log.w(TAG, "cannot create $logDir; recording disabled")
            return
        }
        dir = logDir
        // Dedicated thread: file IO must never run on the notification-listener callback or the
        // location callback, both of which are on the main thread.
        val t = HandlerThread("RelayRecorder").apply { start() }
        thread = t
        handler = Handler(t.looper)
        file = File(logDir, "drive-${timestamp()}.jsonl")
        started = true
        prune()
        event("session") {
            put("device", android.os.Build.MODEL)
            put("sdk", android.os.Build.VERSION.SDK_INT)
            put("appVersion", BuildConfig.VERSION_NAME)
        }
        Log.i(TAG, "recording to ${file?.absolutePath}")
    }

    @Synchronized
    fun stop() {
        if (!started) return
        event("lifecycle") { put("what", "recorderStop") }
        handler?.post { thread?.quitSafely() }
        started = false
    }

    /**
     * Appends one event. Never throws and never blocks the caller: a failed log line must not be
     * able to take navigation down with it.
     */
    fun event(type: String, build: JSONObject.() -> Unit = {}) {
        if (!enabled || !started) return
        val json = JSONObject()
        val wallClock = System.currentTimeMillis()
        runCatching {
            json.put("t", type)
            json.put("wallClockMs", wallClock)
            json.put("iso", isoFormat.format(Date(wallClock)))
            json.build()
        }.onFailure { return }
        val line = json.toString()
        handler?.post { append(line) }
    }

    /** The exact bytes handed to the BLE write, plus the `seq` that joins them to the ESP log. */
    fun packet(seq: Int, bytes: ByteArray, connected: Boolean) {
        event("packet") {
            put("seq", seq)
            put("connected", connected)
            put("len", bytes.size)
            put("hex", bytes.joinToString("") { "%02x".format(it) })
        }
    }

    /**
     * A notification, dumped wholesale. [extras] is produced by the caller (which owns the Bundle
     * traversal) so this object stays free of Android notification specifics and works unchanged
     * for Waze and, later, the iOS/ANCS path.
     */
    fun notification(pkg: String, seq: Int?, extras: JSONObject, icons: JSONObject) {
        event("notif") {
            put("pkg", pkg)
            if (seq != null) put("seq", seq)
            put("extras", extras)
            put("icons", icons)
        }
    }

    /**
     * One notification, captured whole and unfiltered — every extras key, nested bundles included,
     * plus an identity for every Icon it carries. Package-agnostic by construction: the same call
     * serves Google Maps, Waze and Apple Maps without knowing anything about their extras.
     */
    fun notificationRaw(
        pkg: String,
        event: String,
        key: String?,
        postTimeMs: Long,
        template: String?,
        extras: JSONObject,
        icons: JSONObject,
    ) {
        event("notifRaw") {
            put("pkg", pkg)
            put("event", event)
            put("key", key ?: JSONObject.NULL)
            put("postTimeMs", postTimeMs)
            put("template", template ?: JSONObject.NULL)
            put("extras", extras)
            put("icons", icons)
        }
    }

    /** What the phone made of a notification it accepted as navigation, before packing it. */
    fun parsed(
        pkg: String,
        title: String?,
        etaText: String?,
        distanceText: String?,
        progress: Int?,
        progressMax: Int?,
        iconRotationDeg: Int?,
        segments: List<ProgressSegment> = emptyList(),
    ) {
        event("parsed") {
            put("pkg", pkg)
            put("title", title ?: JSONObject.NULL)
            put("etaText", etaText ?: JSONObject.NULL)
            put("distanceText", distanceText ?: JSONObject.NULL)
            put("progress", progress ?: JSONObject.NULL)
            put("progressMax", progressMax ?: JSONObject.NULL)
            put("iconRotationDeg", iconRotationDeg ?: JSONObject.NULL)
            // Raw ARGB kept as hex: the colour -> traffic-level table on the terminal is only as
            // good as the captures checked against it.
            put("segments", org.json.JSONArray(segments.map { "${it.length}:#%08X".format(it.color) }))
        }
    }

    /**
     * One telemetry sample, with the heading-fusion inputs recorded as SEPARATE RAW QUANTITIES.
     *
     * [bearingDeg] (GPS course over ground, the bike's direction of travel) and [yawDeg] (the
     * phone's own orientation) are deliberately not combined here. They live in different frames
     * separated by an unknown mount offset, so any pre-fusion destroys the very information the
     * planned filter needs to learn that offset. See docs/Heading_Sensor_Fusion_Plan.md.
     *
     * [headingDeg] is what we currently *send* to the terminal, kept alongside so a capture shows
     * both the raw inputs and today's naive output for comparison.
     */
    fun telemetry(
        speedKmh: Float?,
        headingDeg: Int?,
        fixValid: Boolean,
        batteryPercent: Int?,
        bearingDeg: Float? = null,
        bearingAccuracyDeg: Float? = null,
        speedAccuracyMps: Float? = null,
        accuracyM: Float? = null,
        hasBearing: Boolean? = null,
        hasSpeed: Boolean? = null,
        yawDeg: Float? = null,
        pitchDeg: Float? = null,
        rollDeg: Float? = null,
        yawRateDps: Float? = null,
        accel: FloatArray? = null,
        gyro: FloatArray? = null,
        latDeg: Double? = null,
        lonDeg: Double? = null,
        altitudeM: Double? = null,
        fixElapsedRealtimeNanos: Long? = null,
        instanceId: Int? = null,
    ) {
        event("telemetry") {
            put("speedKmh", speedKmh ?: JSONObject.NULL)
            put("headingDeg", headingDeg ?: JSONObject.NULL)
            put("fixValid", fixValid)
            put("batteryPercent", batteryPercent ?: JSONObject.NULL)
            // GNSS raw
            put("bearingDeg", bearingDeg ?: JSONObject.NULL)
            put("bearingAccuracyDeg", bearingAccuracyDeg ?: JSONObject.NULL)
            put("speedAccuracyMps", speedAccuracyMps ?: JSONObject.NULL)
            put("accuracyM", accuracyM ?: JSONObject.NULL)
            put("hasBearing", hasBearing ?: JSONObject.NULL)
            put("hasSpeed", hasSpeed ?: JSONObject.NULL)
            // Position. Recorded so a capture can be replayed geometrically: without lat/lon the
            // learned mount offset in docs/Heading_Sensor_Fusion_Plan.md can only be tuned against
            // the same GPS course it is meant to be checked against. [fixElapsedRealtimeNanos] is
            // the fix's own monotonic timestamp -- wall clock is not usable for interpolation.
            put("latDeg", latDeg ?: JSONObject.NULL)
            put("lonDeg", lonDeg ?: JSONObject.NULL)
            put("altitudeM", altitudeM ?: JSONObject.NULL)
            put("fixElapsedRealtimeNanos", fixElapsedRealtimeNanos ?: JSONObject.NULL)
            // Which RelayService object emitted this. The 2026-09-23 capture logged every fix twice
            // ~7 ms apart, one copy carrying a yaw frozen at 249.10715 -- the signature of a second,
            // orphaned service instance with a dead sensor listener. Stamping the instance makes
            // that diagnosable from the log instead of inferable.
            put("instanceId", instanceId ?: JSONObject.NULL)
            // Phone orientation raw
            put("yawDeg", yawDeg ?: JSONObject.NULL)
            put("pitchDeg", pitchDeg ?: JSONObject.NULL)
            put("rollDeg", rollDeg ?: JSONObject.NULL)
            put("yawRateDps", yawRateDps ?: JSONObject.NULL)
            put("accel", accel?.let { jsonArrayOf(*it.toTypedArray()) } ?: JSONObject.NULL)
            put("gyro", gyro?.let { jsonArrayOf(*it.toTypedArray()) } ?: JSONObject.NULL)
        }
    }

    /**
     * High-rate phone orientation. Separate from [telemetry] because jacket flap is a several-Hz
     * oscillation: sampled at the 1 Hz location callback it aliases into noise, and the whole point
     * of the capture is to measure its amplitude and frequency so the filter can reject it.
     */
    fun orientation(yawDeg: Float, pitchDeg: Float, rollDeg: Float, yawRateDps: Float?) {
        event("orient") {
            put("yawDeg", yawDeg)
            put("pitchDeg", pitchDeg)
            put("rollDeg", rollDeg)
            put("yawRateDps", yawRateDps ?: JSONObject.NULL)
        }
    }

    fun lifecycle(what: String, detail: String? = null) {
        event("lifecycle") {
            put("what", what)
            if (detail != null) put("detail", detail)
        }
    }

    // --- internals, all on the recorder thread ---

    private fun append(line: String) {
        val target = file ?: return
        runCatching {
            if (target.length() > MAX_FILE_BYTES) {
                file = File(dir, "drive-${timestamp()}.jsonl")
                prune()
            }
            // Append-and-flush per line: a process kill mid-drive (which is exactly the failure
            // we're trying to diagnose) must not take the tail of the log with it.
            (file ?: target).appendText("$line\n")
        }.onFailure { Log.w(TAG, "append failed", it) }
    }

    /** Keeps the newest [MAX_FILES] captures; older drives are dropped. */
    private fun prune() {
        val files = dir?.listFiles { f -> f.name.startsWith("drive-") } ?: return
        files.sortedByDescending { it.lastModified() }.drop(MAX_FILES).forEach { it.delete() }
    }

    private fun timestamp(): String =
        SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())

    private val isoFormat = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSSZ", Locale.US)
}

/** Convenience for building the JSON arrays the recorder takes. */
fun jsonArrayOf(vararg values: Any?): JSONArray = JSONArray().apply { values.forEach { put(it) } }
