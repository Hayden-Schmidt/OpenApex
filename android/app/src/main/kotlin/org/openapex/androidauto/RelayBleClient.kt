package org.openapex.androidauto

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.UUID

/**
 * BLE central: connects to the terminal's bonded GATT service and writes each [RawNotifPacket]
 * into its nav characteristic. The ESP32 terminal is peripheral/advertiser/GATT server; the phone
 * is central/client. Mirrors the OpenApex BLE identifiers from docs/OpenApex_SPEC.md (same pair
 * the terminal firmware decodes against).
 */
class RelayBleClient(private val context: Context) {

    private var gatt: BluetoothGatt? = null
    private var characteristic: BluetoothGattCharacteristic? = null
    private var callEventChar: BluetoothGattCharacteristic? = null
    private var connectedAddress: String? = null
    private var bondReceiver: BroadcastReceiver? = null

    // Wraps 0-255, matching the firmware's uint8_t packet id (ble_link.c). Identifies which
    // fragments belong to the same RawNotifPacket on the reassembling side.
    private var nextPacketId = 0

    // gatt.writeCharacteristic() queues into a shallow controller-side buffer even for
    // WRITE_TYPE_NO_RESPONSE; firing all 9 fragments synchronously overflowed it and
    // writeCharacteristic() returned false from fragment 1 onward on every single packet
    // (2026-09-24: "packet=N fragment 1/9 queue failed" on every publish). Fragments are now
    // paced one at a time, kicked off again from onCharacteristicWrite once the controller has
    // drained the previous one.
    //
    // Call-event fragments share the same queue (one GATT operation at a time, whatever the
    // target) but jump ahead of nav fragments: a ring must not wait behind a nav packet.
    private class Frame(val call: Boolean, val bytes: ByteArray)
    private val pendingFragments = ArrayDeque<Frame>()
    private var writeInFlight = false
    private val mainHandler = Handler(Looper.getMainLooper())

    // Settings / device-info / avatar operations (spec §5.6). Android allows one GATT operation in
    // flight, so these share writeInFlight with the nav fragments and go ahead of them in
    // pumpQueue(): they are rare and user-driven, while a delayed nav packet is superseded anyway.
    private sealed class ControlOp {
        class Write(val uuid: UUID, val value: ByteArray, val done: (Boolean) -> Unit) : ControlOp()
        class Read(val uuid: UUID, val done: (ByteArray?) -> Unit) : ControlOp()
        class Subscribe(val uuid: UUID) : ControlOp()
    }
    private val controlOps = ArrayDeque<ControlOp>()
    private var controlInFlight: ControlOp? = null

    // True while a background (autoConnect=true) request is parked in the controller waiting for
    // the terminal to come back. A direct connect() for the same device may replace it.
    private var backgroundPending = false

    // Set once this connection has dropped Android's GATT cache (onServicesDiscovered).
    private var cacheRefreshed = false

    /**
     * [background] = true parks the request in the controller's accept list (autoConnect=true):
     * it connects whenever the terminal next advertises, needs no app-side scan and survives
     * screen-off. Used for every reconnect to a known terminal -- the app's own filtered scan was
     * found suspended by OxygenOS for hours with 0 results (2026-09-27 dumpsys), so scanning
     * cannot be the reconnect path. [background] = false is a fast direct connect for
     * user/CDM-initiated starts.
     */
    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice, background: Boolean = false) {
        // Multiple wake paths (RelayService's own scan, plus CDM's legacy onDeviceAppeared and
        // API 36 onDevicePresenceEvent callbacks, which can both fire for the same appearance)
        // can each call connect() for the same device within milliseconds. Without this guard,
        // every call tore down the in-flight/just-established GATT client and opened a new one,
        // so the connection never survived long enough for characteristic writes to succeed.
        if (gatt != null && connectedAddress == device.address && (background || !backgroundPending)) {
            Log.i(TAG, "connect: already connecting/connected to ${device.address}, skipping")
            return
        }
        // The nav characteristic requires an encrypted link (BLE_GATT_CHR_F_WRITE_ENC on the
        // firmware side). CompanionDeviceManager association (MainActivity) only grants
        // background-scan permission -- it does NOT perform GATT bonding. Without bonding, every
        // write silently vanishes (WRITE_TYPE_NO_RESPONSE means the app never sees the
        // insufficient-authentication error), so bonding must be triggered explicitly here.
        if (device.bondState == BluetoothDevice.BOND_NONE) {
            Log.i(TAG, "device not bonded, creating bond before connecting: ${device.address}")
            registerBondReceiver(device)
            device.createBond()
            return
        }
        Log.i(TAG, "connecting to ${device.address} background=$background")
        gatt?.close()
        connectedAddress = device.address
        backgroundPending = background
        // autoConnect=true was once avoided because its reused link state broke the MTU exchange
        // (2026-09-24). write() now fragments to the default MTU, so that no longer matters.
        gatt = device.connectGatt(context, background, callback, BluetoothDevice.TRANSPORT_LE)
    }

    @SuppressLint("MissingPermission")
    private fun registerBondReceiver(device: BluetoothDevice) {
        if (bondReceiver != null) return
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(ctx: Context, intent: Intent) {
                val bondedDevice: BluetoothDevice? =
                    intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
                if (bondedDevice?.address != device.address) return
                val state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1)
                Log.i(TAG, "bond state change: ${device.address} state=$state")
                if (state == BluetoothDevice.BOND_BONDED) {
                    context.unregisterReceiver(this)
                    bondReceiver = null
                    connect(device)
                } else if (state == BluetoothDevice.BOND_NONE) {
                    context.unregisterReceiver(this)
                    bondReceiver = null
                    Log.w(TAG, "bonding failed/removed for ${device.address}")
                }
            }
        }
        bondReceiver = receiver
        context.registerReceiver(receiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED))
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        characteristic = null
        callEventChar = null
        connectedAddress = null
        bondReceiver?.let { context.unregisterReceiver(it) }
        bondReceiver = null
    }

    /**
     * Sends [packet] as a run of small fixed-size fragments instead of one large write.
     *
     * The 2026-09-24 captures killed the MTU-negotiation approach that used to gate this: on that
     * ride every connection's [BluetoothGatt.requestMtu] came back GATT_REQUEST_NOT_SUPPORTED
     * (status 6) while the terminal logged a successful exchange to 256 on its side, and every
     * write was still silently truncated by Android's stack to 20 bytes on the wire regardless --
     * the two sides disagreed about the real ATT MTU and there was no way from here to tell.
     * WRITE_TYPE_NO_RESPONSE gives no delivery feedback either, so a write that Android truncates
     * just reports `queued=true` and vanishes.
     *
     * Chunking to [FRAG_WRITE_BYTES] sidesteps the whole question: that size fits inside the
     * BLE-spec default 23-byte ATT MTU (20-byte payload) on every stack, negotiated or not, so
     * correctness no longer depends on requestMtu() ever succeeding. The terminal reassembles in
     * ble_link.c. A dropped fragment costs one packet, superseded by the next publish (~3Hz).
     */
    fun write(packet: ByteArray) {
        val packetId = nextPacketId
        nextPacketId = (nextPacketId + 1) and 0xFF
        val frames = fragment(packet, packetId, FRAG_COUNT).map { Frame(call = false, bytes = it) }
        synchronized(pendingFragments) {
            // A newer publish (~3Hz) supersedes whatever of the previous packet hasn't gone out
            // yet; the reassembler on the terminal keys off packet_id so a partial old packet is
            // harmless, not corrupting. Queued call fragments are kept.
            pendingFragments.removeAll { !it.call }
            pendingFragments.addAll(frames)
        }
        Log.i(TAG, "write: packet=$packetId len=${packet.size} frags=$FRAG_COUNT queued")
        pumpQueue()
    }

    /**
     * Sends a CALL_EVENT packet (CallPacket.kt) to the terminal's call characteristic, fragmented
     * the same way as nav packets and queued ahead of them. Dropped while not connected: the
     * ring refresh (CallRelay) re-sends once the link is back.
     */
    fun writeCall(packet: ByteArray) {
        val packetId = nextPacketId
        nextPacketId = (nextPacketId + 1) and 0xFF
        val frames = fragment(packet, packetId, CALL_FRAG_COUNT).map { Frame(call = true, bytes = it) }
        synchronized(pendingFragments) {
            // Only the newest call event matters (a CLEAR supersedes a queued RING).
            pendingFragments.removeAll { it.call }
            val firstNav = pendingFragments.indexOfFirst { !it.call }.let { if (it < 0) pendingFragments.size else it }
            pendingFragments.addAll(firstNav, frames)
        }
        Log.i(TAG, "writeCall: packet=$packetId frags=$CALL_FRAG_COUNT queued")
        pumpQueue()
    }

    private fun fragment(packet: ByteArray, packetId: Int, fragCount: Int): List<ByteArray> {
        val frames = mutableListOf<ByteArray>()
        var offset = 0
        var fragIndex = 0
        while (offset < packet.size) {
            val chunkLen = minOf(FRAG_CHUNK_BYTES, packet.size - offset)
            val frame = ByteArray(FRAG_HEADER_BYTES + chunkLen)
            frame[0] = packetId.toByte()
            frame[1] = fragIndex.toByte()
            frame[2] = fragCount.toByte()
            System.arraycopy(packet, offset, frame, FRAG_HEADER_BYTES, chunkLen)
            frames += frame
            offset += chunkLen
            fragIndex++
        }
        return frames
    }

    /** Reads the terminal's settings; [done] gets null if not connected or unreadable. */
    fun readSettings(done: (TerminalSettings?) -> Unit) =
        enqueue(ControlOp.Read(SETTINGS_UUID) { done(it?.let { b -> SettingsTlv.decode(b) }) })

    /** Reads the device-info characteristic, which says the board and avatar size. */
    fun readDeviceInfo(done: (DeviceInfo?) -> Unit) =
        enqueue(ControlOp.Read(DEVICE_INFO_UUID) { done(it?.let(DeviceInfo::parse)) })

    /** Writes every setting, split into MTU-sized partial writes. [done] is false if any failed. */
    fun writeSettings(settings: TerminalSettings, done: (Boolean) -> Unit = {}) =
        writeAll(SETTINGS_UUID, SettingsTlv.encodeWrites(settings), done)

    /**
     * Stores custom avatar art ([AvatarArt]) on the terminal. Show it by writing settings with
     * avatar = [TerminalSettings.AVATAR_CUSTOM]. One 80-byte write, which Android sends as an ATT
     * long write, so the terminal gets all of it or none.
     */
    fun uploadAvatar(art: AvatarArt, done: (Boolean) -> Unit = {}) =
        writeAll(AVATAR_UUID, listOf(art.bytes), done)

    /** Reads the stored custom avatar; [done] gets null if none is stored or the read failed. */
    fun readAvatar(done: (AvatarArt?) -> Unit) =
        enqueue(ControlOp.Read(AVATAR_UUID) { done(it?.let(AvatarArt::fromBytes)) })

    fun clearAvatar(done: (Boolean) -> Unit = {}) =
        writeAll(AVATAR_UUID, listOf(ByteArray(0)), done)

    // With-response writes in order; the first failure drops the rest of this batch.
    private fun writeAll(uuid: UUID, frames: List<ByteArray>, done: (Boolean) -> Unit) {
        if (characteristic == null) {
            done(false)
            return
        }
        var failed = false
        val batch = frames.mapIndexed { i, frame ->
            ControlOp.Write(uuid, frame) { ok ->
                if (failed) return@Write
                if (!ok) failed = true
                if (!ok || i == frames.lastIndex) done(ok)
            }
        }
        synchronized(pendingFragments) { controlOps.addAll(batch) }
        pumpQueue()
    }

    private fun enqueue(op: ControlOp.Read) {
        if (characteristic == null) {
            op.done(null)
            return
        }
        synchronized(pendingFragments) { controlOps.addLast(op) }
        pumpQueue()
    }

    private fun failOp(op: ControlOp) = when (op) {
        is ControlOp.Write -> op.done(false)
        is ControlOp.Read -> op.done(null)
        is ControlOp.Subscribe -> Unit
    }

    /**
     * Rider mesh sync on connect (spec §5.7): push this rider's identity, read the terminal's
     * friends (it may have made some with no phone connected), merge them into ours, write the
     * merged list back, then subscribe so friendships made later reach us too.
     */
    private fun syncRiderMesh() {
        writeAll(IDENTITY_UUID, listOf(RiderStore.identity(context).encode())) { ok ->
            Log.i(TAG, "identity push ok=$ok")
        }
        readTerminalFriends(0, mutableListOf())
    }

    private fun readTerminalFriends(index: Int, acc: MutableList<Friend>) {
        writeAll(FRIENDS_UUID, listOf(RiderMesh.cursor(index))) { ok ->
            if (!ok) return@writeAll // old firmware or link gone; next connect retries
            enqueue(ControlOp.Read(FRIENDS_UUID) { rec ->
                val f = rec?.takeIf { it.isNotEmpty() }?.let(Friend::decode)
                if (f != null && index < RiderMesh.FRIEND_CAP) {
                    acc += f
                    readTerminalFriends(index + 1, acc)
                } else if (rec != null) {
                    pushFriends(acc)
                }
            })
        }
    }

    private fun pushFriends(terminal: List<Friend>) {
        val merged = RiderMesh.merge(RiderStore.friends(context), terminal)
        RiderStore.setFriends(context, merged)
        Log.i(TAG, "friends sync: terminal=${terminal.size} merged=${merged.size}")
        writeAll(FRIENDS_UUID, listOf(RiderMesh.clear()) + merged.map(RiderMesh::upsert)) { ok ->
            Log.i(TAG, "friends push ok=$ok")
        }
        synchronized(pendingFragments) { controlOps.addLast(ControlOp.Subscribe(FRIENDS_UUID)) }
        pumpQueue()
    }

    /** Pushes the phone's friend list again, e.g. after the rider removes one in the app. */
    fun pushFriends() = writeAll(
        FRIENDS_UUID, listOf(RiderMesh.clear()) + RiderStore.friends(context).map(RiderMesh::upsert),
    ) { ok -> Log.i(TAG, "friends push ok=$ok") }

    // A friendship completed on the terminal: `[op=UPSERT][record]`.
    private fun onFriendNotify(char: BluetoothGattCharacteristic, value: ByteArray?) {
        if (char.uuid != FRIENDS_UUID || value == null || value.isEmpty()) return
        if (value[0].toInt() != RiderMesh.OP_UPSERT) return
        val f = Friend.decode(value.copyOfRange(1, value.size)) ?: return
        Log.i(TAG, "terminal added friend ${f.name}")
        RiderStore.setFriends(context, RiderMesh.merge(RiderStore.friends(context), listOf(f)))
    }

    // Starts the next control op, if any. Returns true when one was pending, so nav waits.
    @SuppressLint("MissingPermission")
    @Suppress("DEPRECATION") // read/writeCharacteristic(char, ...) overloads need API 33; minSdk is 26
    private fun pumpControl(g: BluetoothGatt): Boolean {
        val op = controlOps.firstOrNull() ?: return false
        val uuid = when (op) {
            is ControlOp.Write -> op.uuid
            is ControlOp.Read -> op.uuid
            is ControlOp.Subscribe -> op.uuid
        }
        val char = g.getService(SERVICE_UUID)?.getCharacteristic(uuid)
        if (char == null) {
            // Firmware older than spec §5.6.
            controlOps.removeFirst()
            mainHandler.post { failOp(op) }
            mainHandler.post { pumpQueue() }
            return true
        }
        val started = when (op) {
            is ControlOp.Write -> {
                char.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                char.value = op.value
                g.writeCharacteristic(char)
            }
            is ControlOp.Read -> g.readCharacteristic(char)
            is ControlOp.Subscribe -> subscribe(g, char)
        }
        if (started) {
            writeInFlight = true
            controlInFlight = op
            controlOps.removeFirst()
        } else {
            mainHandler.postDelayed({ pumpQueue() }, FRAG_RETRY_DELAY_MS)
        }
        return true
    }

    @SuppressLint("MissingPermission")
    private fun pumpQueue() {
        val g = gatt ?: return
        synchronized(pendingFragments) {
            if (writeInFlight) return
            if (pumpControl(g)) return
            val frame = pendingFragments.firstOrNull() ?: return
            val char = (if (frame.call) callEventChar else characteristic) ?: run {
                // Old firmware without the call characteristic: drop call frames, keep nav going.
                if (frame.call) {
                    pendingFragments.removeFirst()
                    mainHandler.post { pumpQueue() }
                }
                return
            }
            if (writeNoResponse(g, char, frame.bytes)) {
                writeInFlight = true
                pendingFragments.removeFirst()
            } else {
                // Controller-side write buffer is momentarily full; retry shortly instead of
                // dropping the fragment outright.
                mainHandler.postDelayed({ pumpQueue() }, FRAG_RETRY_DELAY_MS)
            }
        }
    }

    /** API 33 replaced the mutate-then-write characteristic API; both paths kept for minSdk 26. */
    @SuppressLint("MissingPermission")
    private fun writeNoResponse(g: BluetoothGatt, char: BluetoothGattCharacteristic, value: ByteArray): Boolean {
        val type = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            return g.writeCharacteristic(char, value, type) == BluetoothStatusCodes.SUCCESS
        }
        @Suppress("DEPRECATION")
        run {
            char.writeType = type
            char.value = value
            return g.writeCharacteristic(char)
        }
    }

    /**
     * Subscribes to CALL_ACTION so the terminal's Accept reaches us. The CCCD write is a GATT
     * operation like any other, so it holds [writeInFlight] until onDescriptorWrite releases it.
     */
    @SuppressLint("MissingPermission")
    private fun subscribeCallAction(g: BluetoothGatt): Boolean {
        val char = g.getService(SERVICE_UUID)?.getCharacteristic(CALL_ACTION_UUID) ?: return false
        return subscribe(g, char)
    }

    @SuppressLint("MissingPermission")
    private fun subscribe(g: BluetoothGatt, char: BluetoothGattCharacteristic): Boolean {
        if (!g.setCharacteristicNotification(char, true)) return false
        val cccd = char.getDescriptor(CCCD_UUID) ?: return false
        val value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeDescriptor(cccd, value) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                cccd.value = value
                g.writeDescriptor(cccd)
            }
        }
    }

    private fun onCallAction(char: BluetoothGattCharacteristic, value: ByteArray?) {
        if (char.uuid != CALL_ACTION_UUID) return
        val callId = parseCallAccept(value) ?: return
        Log.i(TAG, "terminal accepted call id=$callId")
        mainHandler.post { CallRelay.onAccept(callId) }
    }

    private val callback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            Log.i(TAG, "connection state change: status=$status newState=$newState")
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                backgroundPending = false
                cacheRefreshed = false
                saveAddress(context, g.device.address)
                // write() fragments to a fixed size that fits the BLE-spec default MTU regardless
                // of negotiation (see write() KDoc), so nothing here depends on an MTU exchange.
                g.discoverServices()
            } else {
                characteristic = null
                callEventChar = null
                connectedAddress = null
                backgroundPending = false
                // A link dropped mid-write never gets its onCharacteristicWrite, and a flag left
                // set would stall pumpQueue() on every later connection: packets queue, none send.
                val dropped = synchronized(pendingFragments) {
                    writeInFlight = false
                    val ops = listOfNotNull(controlInFlight) + controlOps
                    controlInFlight = null
                    controlOps.clear()
                    ops
                }
                dropped.forEach { failOp(it) }
                RelayStateHolder.noteBleDisconnected(status)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(SERVICE_UUID)
            // Android caches a bonded device's GATT table and the terminal sends no Service
            // Changed, so after a firmware update adds characteristics the phone keeps seeing the
            // old table (2026-09-28: call and identity missing). Drop the cache and rediscover,
            // once per connection.
            if (service != null && service.getCharacteristic(FRIENDS_UUID) == null && !cacheRefreshed) {
                cacheRefreshed = true
                val refreshed = runCatching {
                    g.javaClass.getMethod("refresh").invoke(g) as Boolean
                }.getOrDefault(false)
                Log.i(TAG, "stale GATT cache, refresh=$refreshed")
                if (refreshed && g.discoverServices()) return
            }
            characteristic = service?.getCharacteristic(CHAR_UUID)
            callEventChar = service?.getCharacteristic(CALL_EVENT_UUID)
            Log.i(TAG, "services discovered: status=$status characteristicFound=${characteristic != null} " +
                "callEvent=${callEventChar != null}")
            if (characteristic != null) {
                // Held through the CCCD write when there is one; onDescriptorWrite releases it.
                val subscribing = subscribeCallAction(g)
                Log.i(TAG, "call action subscribe started=$subscribing")
                synchronized(pendingFragments) { writeInFlight = subscribing }
                RelayStateHolder.noteBleConnected()
                syncRiderMesh()
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            Log.i(TAG, "descriptor write ${descriptor.characteristic.uuid} status=$status")
            synchronized(pendingFragments) {
                writeInFlight = false
                controlInFlight = null
            }
            pumpQueue()
        }

        // API 33+ delivers the value here; the deprecated overload below is only called before 33.
        override fun onCharacteristicChanged(g: BluetoothGatt, char: BluetoothGattCharacteristic, value: ByteArray) {
            onCallAction(char, value)
            onFriendNotify(char, value)
        }

        @Deprecated("Pre-API 33 path")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, char: BluetoothGattCharacteristic) {
            onCallAction(char, char.value)
            onFriendNotify(char, char.value)
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            char: BluetoothGattCharacteristic,
            status: Int,
        ) {
            val op = synchronized(pendingFragments) {
                writeInFlight = false
                controlInFlight.also { controlInFlight = null }
            }
            (op as? ControlOp.Write)?.done?.invoke(status == BluetoothGatt.GATT_SUCCESS)
            pumpQueue()
        }

        @Deprecated("Deprecated in API 33, but the only read callback below it; minSdk is 26")
        @Suppress("DEPRECATION")
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            char: BluetoothGattCharacteristic,
            status: Int,
        ) {
            val value = char.value
            val op = synchronized(pendingFragments) {
                writeInFlight = false
                controlInFlight.also { controlInFlight = null }
            }
            (op as? ControlOp.Read)?.done?.invoke(if (status == BluetoothGatt.GATT_SUCCESS) value else null)
            pumpQueue()
        }
    }

    companion object {
        private const val TAG = "RelayBleClient"
        private const val PREFS = "relay_ble"
        private const val KEY_ADDRESS = "terminal_address"

        /** Last terminal this phone connected to; lets no-address starts reconnect without scanning. */
        fun savedAddress(context: Context): String? =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY_ADDRESS, null)

        private fun saveAddress(context: Context, address: String) {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putString(KEY_ADDRESS, address).apply()
        }

        // Fixed OpenApex BLE identifiers — see docs/OpenApex_SPEC.md §5.3.
        val SERVICE_UUID: UUID = UUID.fromString("c9c6d0a0-0001-4f0a-9c8e-2f6b1a2d3e4f")
        val CHAR_UUID: UUID = UUID.fromString("c9c6d0a0-0002-4f0a-9c8e-2f6b1a2d3e4f")
        /** Incoming call, phone -> terminal (CallPacket.kt / firmware call_packet.h). */
        val CALL_EVENT_UUID: UUID = UUID.fromString("c9c6d0a0-0003-4f0a-9c8e-2f6b1a2d3e4f")
        /** Accept, terminal -> phone, by notification. */
        val CALL_ACTION_UUID: UUID = UUID.fromString("c9c6d0a0-0004-4f0a-9c8e-2f6b1a2d3e4f")
        private val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        // Terminal settings, device info and avatar upload — docs/OpenApex_SPEC.md §5.6.
        val SETTINGS_UUID: UUID = UUID.fromString("c9c6d0a0-0005-4f0a-9c8e-2f6b1a2d3e4f")
        val DEVICE_INFO_UUID: UUID = UUID.fromString("c9c6d0a0-0006-4f0a-9c8e-2f6b1a2d3e4f")
        val AVATAR_UUID: UUID = UUID.fromString("c9c6d0a0-0007-4f0a-9c8e-2f6b1a2d3e4f")

        // Rider mesh identity and friends — docs/OpenApex_SPEC.md §5.7.
        val IDENTITY_UUID: UUID = UUID.fromString("c9c6d0a0-0008-4f0a-9c8e-2f6b1a2d3e4f")
        val FRIENDS_UUID: UUID = UUID.fromString("c9c6d0a0-0009-4f0a-9c8e-2f6b1a2d3e4f")

        /** Packet id (1) + fragment index (1) + fragment count (1) ahead of each chunk's payload. */
        private const val FRAG_HEADER_BYTES = 3

        /** Payload bytes per fragment. FRAG_HEADER_BYTES + this must fit MTU 23 - 3 = 20. Mirrors ble_link.c. */
        private const val FRAG_CHUNK_BYTES = 17

        private const val FRAG_WRITE_BYTES = FRAG_HEADER_BYTES + FRAG_CHUNK_BYTES
        private val FRAG_COUNT = (RAW_NOTIF_PACKET_SIZE + FRAG_CHUNK_BYTES - 1) / FRAG_CHUNK_BYTES
        private val CALL_FRAG_COUNT = (CALL_EVENT_PACKET_SIZE + FRAG_CHUNK_BYTES - 1) / FRAG_CHUNK_BYTES

        private const val FRAG_RETRY_DELAY_MS = 15L

        init {
            check(FRAG_WRITE_BYTES <= 20) { "fragment write must fit the default 23-byte ATT MTU" }
        }
    }
}
