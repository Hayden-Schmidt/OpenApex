package org.openapex.telemetry

import org.openapex.protocol.GnssTelemetry
import org.openapex.protocol.MotionTelemetry
import org.openapex.protocol.RawNavNotification
import org.openapex.protocol.packRawNotifPacket
import org.openapex.protocol.telemetryOnlyPacket

/**
 * Holds the latest of every input the terminal is sent and turns them into a packet on demand.
 *
 * The inputs arrive from unrelated places at unrelated rates -- a notification listener, fused
 * location, two sensors, a battery broadcast -- and a packet is a snapshot of all of them at one
 * instant. That merging, the staleness rule on the fix and the sequence numbering are the same on
 * both platforms; only the sources differ, which is why they are plain properties set from
 * outside rather than anything this class subscribes to.
 *
 * **Not thread-safe.** The Android service runs on the main looper throughout.
 */
class TelemetryAggregator {

    /**
     * A position with the monotonic time it was taken. Monotonic, not wall clock: a fix is judged
     * by how long ago it arrived, and the wall clock can step.
     */
    class Fix(
        val latitudeDeg: Double,
        val longitudeDeg: Double,
        val accuracyM: Float?,
        val monotonicMs: Long,
    )

    var nav: RawNavNotification? = null
    var fix: Fix? = null
    var fixValid: Boolean = false
    var speedKmh: Float? = null
    var headingDeg: Int? = null
    var bearingAccuracyDeg: Float? = null
    var yawDeg: Float? = null
    var yawRateDps: Float? = null
    var batteryPercent: Int? = null
    var accelMs2: FloatArray? = null
    var gyroRadS: FloatArray? = null

    /**
     * Set on iOS, where nav reaches the terminal over ANCS and the app never sees it. Packets then
     * flag their nav fields absent rather than empty, so they do not erase a turn the terminal
     * already has. Android leaves this false: there the app is the nav source.
     */
    var telemetryOnly: Boolean = false

    private var sequence = 0

    /** A built packet and the sequence number it carries, which the drive log records alongside. */
    class Published(val sequence: Int, val bytes: ByteArray)

    /**
     * Builds the next packet. [nowEpochMs] and [tzOffsetMin] are passed in because commonMain has
     * no ambient clock, and because on iOS the terminal takes its time from CTS instead.
     * [monotonicMs] is only compared against [Fix.monotonicMs], so any monotonic source will do.
     */
    fun publish(nowEpochMs: Long, tzOffsetMin: Int, monotonicMs: Long): Published {
        // No active navigation is not a reason to stop transmitting. The terminal is a compass and
        // speed display first and a nav display second; gating the packet on nav meant the compass
        // died whenever Maps was not routing, which is most riding. An empty maneuver normalizes
        // to the idle/compass view on the terminal -- no fabricated zeros, per the sentinel
        // discipline in RawNotifPacket.
        val currentNav = nav ?: EMPTY_NAV

        // Only a recent fix goes out: the terminal shares it with friends as "where I am now", so
        // a stale position is worse than none.
        val recent = fix?.takeIf { monotonicMs - it.monotonicMs < FIX_MAX_AGE_MS }

        val seq = ++sequence
        val gnss = GnssTelemetry(
            speedKmh = speedKmh,
            headingDeg = headingDeg,
            fixValid = fixValid,
            batteryPercent = batteryPercent,
            bearingAccuracyDeg = bearingAccuracyDeg,
            yawDeg = yawDeg,
            yawRateDps = yawRateDps,
            latitudeDeg = recent?.latitudeDeg,
            longitudeDeg = recent?.longitudeDeg,
            accuracyM = recent?.accuracyM,
        )
        val motion = MotionTelemetry(accelMs2 = accelMs2, gyroRadS = gyroRadS)
        val bytes = if (telemetryOnly) {
            telemetryOnlyPacket(seq, gnss, motion, nowEpochMs = nowEpochMs, tzOffsetMin = tzOffsetMin)
        } else {
            packRawNotifPacket(seq, currentNav, gnss, motion, nowEpochMs = nowEpochMs, tzOffsetMin = tzOffsetMin)
        }
        return Published(seq, bytes)
    }

    companion object {
        /** Older than this and the position is dropped rather than sent. */
        const val FIX_MAX_AGE_MS = 5_000L

        /**
         * Sent when nothing is navigating. Every field null, so the terminal's normalizer sees no
         * title, no distance and no progress and settles on its idle/compass view.
         */
        val EMPTY_NAV = RawNavNotification(
            title = null,
            etaText = null,
            distanceText = null,
            progress = null,
            progressMax = null,
            iconRotationDeg = null,
        )
    }
}
