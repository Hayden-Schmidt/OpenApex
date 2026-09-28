package org.openapex.protocol

import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue
import kotlin.test.Test

class RiderMeshTest {
    private val id = ByteArray(8) { it.toByte() }
    private val key = ByteArray(16) { (0x10 + it).toByte() }

    @Test
    fun identityLayoutMatchesFirmware() {
        val b = RiderIdentity(id, key, "Hay").encode()
        assertEquals(1 + 8 + 16 + 1 + 3, b.size)
        assertEquals(1, b[0].toInt())
        assertContentEquals(id, b.copyOfRange(1, 9))
        assertContentEquals(key, b.copyOfRange(9, 25))
        assertEquals(3, b[25].toInt())
        assertEquals("Hay", b.copyOfRange(26, 29).decodeToString())
    }

    @Test
    fun nameCutsOnCharacterBoundary() {
        // 11 ASCII bytes then a 2-byte character: the character must not be split.
        assertEquals(11, RiderMesh.nameBytes("abcdefghijké").size)
        assertEquals(12, RiderMesh.nameBytes("abcdefghijklmnop").size)
    }

    @Test
    fun friendRecordRoundTrips() {
        val art = ByteArray(80) { (it * 3).toByte() }
        for (f in listOf(
            Friend(id, key, "Sam", 4, 0x12AB34, null),
            Friend(id, key, "Custom", 0xFF, 0xFFE800, art),
        )) {
            val b = f.encode()
            assertEquals(122, b.size)
            assertEquals(f.art != null, b[28].toInt() == 1)
            assertTrue(Friend.decode(b)!!.sameAs(f))
        }
    }

    @Test
    fun friendRecordLayout() {
        val b = Friend(id, key, "Sam", 4, 0x12AB34, null).encode()
        assertEquals(4, b[24].toInt())
        assertContentEquals(byteArrayOf(0x12, 0xAB.toByte(), 0x34), b.copyOfRange(25, 28))
        assertEquals(3, b[29].toInt())
        assertEquals("Sam", b.copyOfRange(30, 33).decodeToString())
    }

    @Test
    fun badRecordRejected() {
        assertNull(Friend.decode(ByteArray(121)))
        assertNull(Friend.decode(ByteArray(122).also { it[29] = 13 }))
    }

    @Test
    fun mergeAddsTerminalOnlyAndPrefersTerminalLook() {
        val a = Friend(ByteArray(8) { 1 }, key, "A", 1, 0, null)
        val b = Friend(ByteArray(8) { 2 }, key, "B", 2, 0, null)
        val b2 = Friend(ByteArray(8) { 2 }, key, "B", 5, 0, null)
        val c = Friend(ByteArray(8) { 3 }, key, "C", 3, 0, null)
        val m = RiderMesh.merge(listOf(a, b), listOf(b2, c))
        assertEquals(listOf("A", "B", "C"), m.map { it.name })
        assertEquals(5, m[1].avatar)
    }

    @Test
    fun mergeCaps() {
        val many = (0 until 20).map { i -> Friend(ByteArray(8) { i.toByte() }, key, "F$i", 0, 0, null) }
        assertEquals(16, RiderMesh.merge(emptyList(), many).size)
    }

    @Test
    fun ops() {
        assertContentEquals(byteArrayOf(4, 7), RiderMesh.cursor(7))
        assertContentEquals(byteArrayOf(3), RiderMesh.clear())
        assertEquals(123, RiderMesh.upsert(Friend(id, key, "x", 0, 0, null)).size)
    }
}
