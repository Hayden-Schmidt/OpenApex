package org.openapex.androidauto

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
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
    private val pendingFragments = ArrayDeque<ByteArray>()
    private var writeInFlight = false
    private val mainHandler = Handler(Looper.getMainLooper())

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice) {
        // Multiple wake paths (RelayService's own scan, plus CDM's legacy onDeviceAppeared and
        // API 36 onDevicePresenceEvent callbacks, which can both fire for the same appearance)
        // can each call connect() for the same device within milliseconds. Without this guard,
        // every call tore down the in-flight/just-established GATT client and opened a new one,
        // so the connection never survived long enough for characteristic writes to succeed.
        if (gatt != null && connectedAddress == device.address) {
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
        Log.i(TAG, "connecting to ${device.address}")
        gatt?.close()
        connectedAddress = device.address
        // autoConnect=false: a DIRECT connection always performs a fresh ATT MTU exchange. With
        // autoConnect=true the stack reuses cached link state across a terminal reboot, and the
        // re-requested exchange comes back GATT_REQUEST_NOT_SUPPORTED, leaving the MTU unverifiable
        // (2026-09-24 afternoon ride: three of four sessions). Nothing is lost by dropping the
        // background auto-reconnect -- RelayService re-scans and reconnects itself on disconnect.
        gatt = device.connectGatt(context, false, callback)
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
        val frames = mutableListOf<ByteArray>()
        var offset = 0
        var fragIndex = 0
        while (offset < packet.size) {
            val chunkLen = minOf(FRAG_CHUNK_BYTES, packet.size - offset)
            val frame = ByteArray(FRAG_HEADER_BYTES + chunkLen)
            frame[0] = packetId.toByte()
            frame[1] = fragIndex.toByte()
            frame[2] = FRAG_COUNT.toByte()
            System.arraycopy(packet, offset, frame, FRAG_HEADER_BYTES, chunkLen)
            frames += frame
            offset += chunkLen
            fragIndex++
        }
        synchronized(pendingFragments) {
            // A newer publish (~3Hz) supersedes whatever of the previous packet hasn't gone out
            // yet; the reassembler on the terminal keys off packet_id so a partial old packet is
            // harmless, not corrupting.
            pendingFragments.clear()
            pendingFragments.addAll(frames)
        }
        Log.i(TAG, "write: packet=$packetId len=${packet.size} frags=$FRAG_COUNT queued")
        pumpQueue()
    }

    @SuppressLint("MissingPermission")
    @Suppress("DEPRECATION") // writeCharacteristic(char, type, value) needs API 33; minSdk is 26
    private fun pumpQueue() {
        val g = gatt ?: return
        val char = characteristic ?: return
        synchronized(pendingFragments) {
            if (writeInFlight) return
            val frame = pendingFragments.firstOrNull() ?: return
            char.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
            char.value = frame
            if (g.writeCharacteristic(char)) {
                writeInFlight = true
                pendingFragments.removeFirst()
            } else {
                // Controller-side write buffer is momentarily full; retry shortly instead of
                // dropping the fragment outright.
                mainHandler.postDelayed({ pumpQueue() }, FRAG_RETRY_DELAY_MS)
            }
        }
    }

    private val callback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            Log.i(TAG, "connection state change: status=$status newState=$newState")
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                // write() fragments to a fixed size that fits the BLE-spec default MTU regardless
                // of negotiation (see write() KDoc), so nothing here depends on an MTU exchange.
                g.discoverServices()
            } else {
                characteristic = null
                connectedAddress = null
                RelayStateHolder.noteBleDisconnected(status)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            characteristic = g.getService(SERVICE_UUID)?.getCharacteristic(CHAR_UUID)
            Log.i(TAG, "services discovered: status=$status characteristicFound=${characteristic != null}")
            if (characteristic != null) {
                RelayStateHolder.noteBleConnected()
            }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            char: BluetoothGattCharacteristic,
            status: Int,
        ) {
            synchronized(pendingFragments) { writeInFlight = false }
            pumpQueue()
        }
    }

    companion object {
        private const val TAG = "RelayBleClient"

        // Fixed OpenApex BLE identifiers — see docs/OpenApex_SPEC.md §5.3.
        val SERVICE_UUID: UUID = UUID.fromString("c9c6d0a0-0001-4f0a-9c8e-2f6b1a2d3e4f")
        val CHAR_UUID: UUID = UUID.fromString("c9c6d0a0-0002-4f0a-9c8e-2f6b1a2d3e4f")

        /** Packet id (1) + fragment index (1) + fragment count (1) ahead of each chunk's payload. */
        private const val FRAG_HEADER_BYTES = 3

        /** Payload bytes per fragment. FRAG_HEADER_BYTES + this must fit MTU 23 - 3 = 20. Mirrors ble_link.c. */
        private const val FRAG_CHUNK_BYTES = 17

        private const val FRAG_WRITE_BYTES = FRAG_HEADER_BYTES + FRAG_CHUNK_BYTES
        private val FRAG_COUNT = (RAW_NOTIF_PACKET_SIZE + FRAG_CHUNK_BYTES - 1) / FRAG_CHUNK_BYTES

        private const val FRAG_RETRY_DELAY_MS = 15L

        init {
            check(FRAG_WRITE_BYTES <= 20) { "fragment write must fit the default 23-byte ATT MTU" }
        }
    }
}
