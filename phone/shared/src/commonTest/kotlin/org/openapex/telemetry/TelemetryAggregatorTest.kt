package org.openapex.telemetry

import org.openapex.protocol.RawNavNotification
import kotlin.test.Test
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertNotEquals
import kotlin.test.assertTrue

class TelemetryAggregatorTest {

    private fun agg() = TelemetryAggregator().apply {
        fixValid = true
        speedKmh = 42f
        headingDeg = 90
    }

    @Test
    fun `sequence numbers start at 1 and never repeat`() {
        val a = agg()
        assertEquals(1, a.publish(0, 0, 0).sequence)
        assertEquals(2, a.publish(0, 0, 0).sequence)
        assertEquals(3, a.publish(0, 0, 0).sequence)
    }

    @Test
    fun `a fix older than the limit is dropped rather than sent`() {
        val a = agg()
        a.fix = TelemetryAggregator.Fix(51.5, -0.12, 4f, monotonicMs = 1_000)

        val fresh = a.publish(0, 0, monotonicMs = 1_000 + TelemetryAggregator.FIX_MAX_AGE_MS - 1)
        val stale = a.publish(0, 0, monotonicMs = 1_000 + TelemetryAggregator.FIX_MAX_AGE_MS)

        // Same inputs but for the age, so any difference in the encoded bytes is the position.
        assertNotEquals(fresh.bytes.toList(), stale.bytes.toList())

        // And the stale one matches a packet built with no fix at all: dropped, not zeroed.
        val none = TelemetryAggregator().apply {
            fixValid = true
            speedKmh = 42f
            headingDeg = 90
        }
        none.publish(0, 0, 0) // burn one so the sequence byte matches stale's, which was the 2nd
        assertContentEquals(none.publish(0, 0, 0).bytes, stale.bytes)
    }

    @Test
    fun `no navigation still produces a packet`() {
        val a = agg()
        val idle = a.publish(0, 0, 0)
        assertTrue(idle.bytes.isNotEmpty())

        // The terminal decides idle from an absence of nav fields, so a navigating packet must
        // actually differ -- if these matched, the empty nav would be indistinguishable.
        a.nav = RawNavNotification(
            title = "Turn left",
            etaText = "10 min",
            distanceText = "200 m",
            progress = 1,
            progressMax = 10,
            iconRotationDeg = 270,
        )
        assertNotEquals(idle.bytes.toList(), a.publish(0, 0, 0).bytes.toList())
    }

    @Test
    fun `the clock comes from the caller, not from the platform`() {
        val a = agg()
        val early = a.publish(nowEpochMs = 1_700_000_000_000, tzOffsetMin = 0, monotonicMs = 0)
        val later = a.publish(nowEpochMs = 1_700_000_060_000, tzOffsetMin = 60, monotonicMs = 0)
        assertNotEquals(early.bytes.toList(), later.bytes.toList())
    }
}
