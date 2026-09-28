package org.openapex.app

import org.openapex.ble.ControlOp
import org.openapex.ble.ControlQueue
import org.openapex.ble.TerminalCharacteristic
import org.openapex.mesh.MeshTransport
import org.openapex.mesh.RiderMeshSync
import org.openapex.protocol.*
import org.openapex.store.RiderMeshStore
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


    // gatt.writeCharacteristic() queues into a shallow controller-side buffer even for
    // WRITE_TYPE_NO_RESPONSE; firing all 9 fragments synchronously overflowed it and
    // writeCharacteristic() returned false from fragment 1 onward on every single packet
    // (2026-09-24: "packet=N fragment 1/9 queue failed" on every publish). Fragments are now
    // paced one at a time, kicked off again from onCharacteristicWrite once the controller has
    // drained the previous one.
    //
    // Call-event fragments share the same queue (one GATT operation at a time, whatever the
    // target) but jump ahead of nav fragments: a ring must not wait behind a nav packet.
    // What to send next and in what order lives in commonMain (FragmentQueue): it is the half
    // iOS needs unchanged, and the half worth testing without a phone. This class owns only the
    // GATT calls and the one-operation-in-flight rule. FragmentQueue is not thread-safe, which is
    // why every access below is inside queueLock.
    private val frames = FragmentQueue()

    // Guards frames, controlOps and writeInFlight together: they are one queue with one in-flight
    // slot and are never consistent apart. An explicit object rather than the FragmentQueue
    // itself, because a local named frames -- writeAll() had a parameter by that name -- silently
    // shadowed it and locked the wrong thing.
    //
    // Held across the control completions as well, not just the queue mutation. Neither queue is
    // thread-safe and GATT callbacks arrive on a binder thread while the pump runs on the main
    // looper. A completion callback that queues more work re-enters this lock on its own thread,
    // which synchronized allows.
    private val queueLock = Any()
    private var writeInFlight = false
    private val mainHandler = Handler(Looper.getMainLooper())

    // Settings / device-info / avatar operations (spec §5.6). Android allows one GATT operation in
    // flight, so these share writeInFlight with the nav fragments and go ahead of them in
    // pumpQueue(): they are rare and user-driven, while a delayed nav packet is superseded anyway.
    private val controlOps = ControlQueue()

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
        val packetId = synchronized(queueLock) { frames.offerNav(packet) }
        Log.i(TAG, "write: packet=$packetId len=${packet.size} frags=$NAV_FRAG_COUNT queued")
        pumpQueue()
    }

    /**
     * Sends a CALL_EVENT packet (CallPacket.kt) to the terminal's call characteristic, fragmented
     * the same way as nav packets and queued ahead of them. Dropped while not connected: the
     * ring refresh (CallRelay) re-sends once the link is back.
     */
    fun writeCall(packet: ByteArray) {
        val packetId = synchronized(queueLock) { frames.offerCall(packet) }
        Log.i(TAG, "writeCall: packet=$packetId frags=$CALL_FRAG_COUNT queued")
        pumpQueue()
    }

    /** Reads the terminal's settings; [done] gets null if not connected or unreadable. */
    fun readSettings(done: (TerminalSettings?) -> Unit) =
        read(TerminalCharacteristic.SETTINGS) { done(it?.let { b -> SettingsTlv.decode(b) }) }

    /** Reads the device-info characteristic, which says the board and avatar size. */
    fun readDeviceInfo(done: (DeviceInfo?) -> Unit) =
        read(TerminalCharacteristic.DEVICE_INFO) { done(it?.let(DeviceInfo::parse)) }

    /** Writes every setting, split into MTU-sized partial writes. [done] is false if any failed. */
    fun writeSettings(settings: TerminalSettings, done: (Boolean) -> Unit = {}) =
        writeAll(TerminalCharacteristic.SETTINGS, SettingsTlv.encodeWrites(settings), done)

    /**
     * Stores custom avatar art ([AvatarArt]) on the terminal. Show it by writing settings with
     * avatar = [TerminalSettings.AVATAR_CUSTOM]. One 80-byte write, which Android sends as an ATT
     * long write, so the terminal gets all of it or none.
     */
    fun uploadAvatar(art: AvatarArt, done: (Boolean) -> Unit = {}) =
        writeAll(TerminalCharacteristic.AVATAR, listOf(art.bytes), done)

    /** Reads the stored custom avatar; [done] gets null if none is stored or the read failed. */
    fun readAvatar(done: (AvatarArt?) -> Unit) =
        read(TerminalCharacteristic.AVATAR) { done(it?.let(AvatarArt::fromBytes)) }

    fun clearAvatar(done: (Boolean) -> Unit = {}) =
        writeAll(TerminalCharacteristic.AVATAR, listOf(ByteArray(0)), done)

    // Ordering, batch-failure collapsing and completion live in ControlQueue (commonMain). What
    // is left here is the not-connected short circuit, which is the one thing the queue cannot
    // know: it has no link to ask.
    private fun writeAll(char: TerminalCharacteristic, frames: List<ByteArray>, done: (Boolean) -> Unit) {
        if (characteristic == null) {
            done(false)
            return
        }
        synchronized(queueLock) { controlOps.writeAll(char, frames, done) }
        pumpQueue()
    }

    private fun read(char: TerminalCharacteristic, done: (ByteArray?) -> Unit) {
        if (characteristic == null) {
            done(null)
            return
        }
        synchronized(queueLock) { controlOps.read(char, done) }
        pumpQueue()
    }

    private fun subscribe(char: TerminalCharacteristic) {
        synchronized(queueLock) { controlOps.subscribe(char) }
        pumpQueue()
    }

    // Rider mesh sync (spec 5.7) lives in commonMain: the cursor walk over the terminal's friend
    // list, the merge and the push-back are the same on both platforms, and they are the half
    // worth testing without a phone. This class supplies the GATT half as a MeshTransport, and
    // the store adapter below so the sync goes through RiderStore's lock rather than the
    // un-synchronised core.
    private val mesh = RiderMeshSync(
        store = object : RiderMeshStore {
            override fun identity() = RiderStore.identity(context)
            override fun friends() = RiderStore.friends(context)
            override fun setFriends(list: List<Friend>) = RiderStore.setFriends(context, list)
        },
        link = object : MeshTransport {
            override fun writeAll(char: TerminalCharacteristic, frames: List<ByteArray>, done: (Boolean) -> Unit) =
                this@RelayBleClient.writeAll(char, frames, done)

            override fun read(char: TerminalCharacteristic, done: (ByteArray?) -> Unit) =
                this@RelayBleClient.read(char, done)

            override fun subscribe(char: TerminalCharacteristic) = this@RelayBleClient.subscribe(char)

            override fun log(message: String) { Log.i(TAG, message) }
        },
    )

    private fun uuidOf(char: TerminalCharacteristic): UUID = when (char) {
        TerminalCharacteristic.NAV -> CHAR_UUID
        TerminalCharacteristic.CALL_EVENT -> CALL_EVENT_UUID
        TerminalCharacteristic.CALL_ACTION -> CALL_ACTION_UUID
        TerminalCharacteristic.SETTINGS -> SETTINGS_UUID
        TerminalCharacteristic.DEVICE_INFO -> DEVICE_INFO_UUID
        TerminalCharacteristic.AVATAR -> AVATAR_UUID
        TerminalCharacteristic.IDENTITY -> IDENTITY_UUID
        TerminalCharacteristic.FRIENDS -> FRIENDS_UUID
    }

    /** Pushes the phone's friend list again, e.g. after the rider removes one in the app. */
    fun pushFriends() = mesh.pushFriends()

    private fun onFriendNotify(char: BluetoothGattCharacteristic, value: ByteArray?) {
        if (char.uuid == FRIENDS_UUID && value != null) mesh.onFriendNotify(value)
    }

    // Starts the next control op, if any. Returns true when one was pending, so nav waits.
    @SuppressLint("MissingPermission")
    @Suppress("DEPRECATION") // read/writeCharacteristic(char, ...) overloads need API 33; minSdk is 26
    private fun pumpControl(g: BluetoothGatt): Boolean {
        val op = controlOps.peek() ?: return false
        val char = g.getService(SERVICE_UUID)?.getCharacteristic(uuidOf(op.char))
        if (char == null) {
            // Firmware older than spec §5.6. Posted rather than called here: failHead() runs the
            // caller's callback, and that must not happen while holding queueLock.
            mainHandler.post { synchronized(queueLock) { controlOps.failHead() } }
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
            controlOps.markStarted()
        } else {
            mainHandler.postDelayed({ pumpQueue() }, FRAG_RETRY_DELAY_MS)
        }
        return true
    }

    @SuppressLint("MissingPermission")
    private fun pumpQueue() {
        val g = gatt ?: return
        synchronized(queueLock) {
            if (writeInFlight) return
            if (pumpControl(g)) return
            val frame = frames.peek() ?: return
            val char = (if (frame.call) callEventChar else characteristic) ?: run {
                // Old firmware without the call characteristic: drop call frames, keep nav going.
                if (frame.call) {
                    frames.removeFirst()
                    mainHandler.post { pumpQueue() }
                }
                return
            }
            if (writeNoResponse(g, char, frame.bytes)) {
                writeInFlight = true
                frames.removeFirst()
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
                synchronized(queueLock) {
                    writeInFlight = false
                    controlOps.failAll()
                }
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
                synchronized(queueLock) { writeInFlight = subscribing }
                RelayStateHolder.noteBleConnected()
                mesh.onConnected()
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            Log.i(TAG, "descriptor write ${descriptor.characteristic.uuid} status=$status")
            synchronized(queueLock) {
                writeInFlight = false
                // A CCCD write the queue did not issue (the call-action subscribe on connect)
                // lands here too; complete() ignores it when nothing is in flight.
                controlOps.complete(status == BluetoothGatt.GATT_SUCCESS)
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
            synchronized(queueLock) {
                writeInFlight = false
                controlOps.complete(status == BluetoothGatt.GATT_SUCCESS)
            }
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
            synchronized(queueLock) {
                writeInFlight = false
                controlOps.complete(status == BluetoothGatt.GATT_SUCCESS, value)
            }
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

        // Fragment sizes and counts are in commonMain (Fragmentation.kt), with the check that
        // they fit the default ATT MTU.
        private const val FRAG_RETRY_DELAY_MS = 15L
    }
}
