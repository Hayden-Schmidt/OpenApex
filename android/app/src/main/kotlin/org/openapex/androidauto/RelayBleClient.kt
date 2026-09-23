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
    private var negotiatedMtu = ATT_MTU_DEFAULT
    private var mtuRequested = false
    private var bondReceiver: BroadcastReceiver? = null

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
        gatt = device.connectGatt(context, true, callback)
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

    @SuppressLint("MissingPermission")
    @Suppress("DEPRECATION") // writeCharacteristic(char, type, value) needs API 33; minSdk is 26
    fun write(packet: ByteArray) {
        val g = gatt ?: run { Log.w(TAG, "write: no gatt, dropping"); return }
        val char = characteristic ?: run { Log.w(TAG, "write: no characteristic, dropping"); return }
        // Never hand a packet to a link that cannot carry it whole.
        //
        // WRITE_TYPE_NO_RESPONSE gives no delivery feedback, and the stack does not reject an
        // oversized value -- it truncates it to (MTU - 3) and reports success. The terminal then
        // receives a 20-byte fragment and discards it. On 2026-09-24 that happened for three
        // entire sessions, 120 packets, with `queued=true` logged every time and a blank display
        // on the bike. A silent success is the worst possible signal, so check explicitly.
        if (negotiatedMtu < packet.size + ATT_HEADER_BYTES) {
            Log.e(
                TAG,
                "write: MTU $negotiatedMtu too small for ${packet.size}-byte packet; " +
                    "dropping and re-requesting MTU",
            )
            RelayRecorder.lifecycle("bleMtuTooSmall", "mtu=$negotiatedMtu need=${packet.size + ATT_HEADER_BYTES}")
            requestMtuOnce(g)
            return
        }
        char.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        char.value = packet
        val ok = g.writeCharacteristic(char)
        Log.i(TAG, "write: len=${packet.size} mtu=$negotiatedMtu queued=$ok")
    }

    /**
     * Requests the MTU at most once per connection attempt.
     *
     * requestMtu() while an exchange is already outstanding fails, and a second exchange on a
     * link that already has one is rejected by many peripherals, so this guards rather than
     * retrying blindly. Cleared on every connection state change.
     */
    @SuppressLint("MissingPermission")
    private fun requestMtuOnce(g: BluetoothGatt) {
        if (mtuRequested) return
        mtuRequested = true
        val ok = g.requestMtu(MTU_BYTES)
        Log.i(TAG, "requestMtu($MTU_BYTES) -> $ok")
        if (!ok) mtuRequested = false
    }

    private val callback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            Log.i(TAG, "connection state change: status=$status newState=$newState")
            // Every state change invalidates what we knew about the link. Assume the BLE default
            // (23) until an exchange tells us otherwise: assuming the last connection's MTU is
            // exactly how truncated writes got sent believing they were fine.
            negotiatedMtu = ATT_MTU_DEFAULT
            mtuRequested = false
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                // Default ATT MTU is 23 bytes (20-byte payload after the 3-byte ATT header), far
                // smaller than the 146-byte RawNotifPacket. Without negotiating a larger MTU
                // first, writeCharacteristic() silently truncates every packet down to whatever
                // the current MTU allows, which the firmware then rejects as malformed rather
                // than decoding a partial packet.
                requestMtuOnce(g)
            } else {
                characteristic = null
                connectedAddress = null
                RelayStateHolder.noteBleDisconnected()
            }
        }

        @SuppressLint("MissingPermission")
        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            // Trust the reported value, not the requested one. A failed exchange still fires this
            // callback, and on a re-connect to a bonded peer Android may report a cached MTU the
            // peripheral does not actually have -- which is the failure this whole path exists to
            // catch. write() re-checks before every packet for that reason.
            negotiatedMtu = if (status == BluetoothGatt.GATT_SUCCESS) mtu else ATT_MTU_DEFAULT
            Log.i(TAG, "mtu changed: mtu=$mtu status=$status -> using $negotiatedMtu")
            RelayRecorder.lifecycle("bleMtuChanged", "mtu=$mtu status=$status")
            if (negotiatedMtu < RAW_NOTIF_PACKET_SIZE + ATT_HEADER_BYTES) {
                Log.e(TAG, "mtu $negotiatedMtu cannot carry a $RAW_NOTIF_PACKET_SIZE-byte packet")
            }
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            characteristic = g.getService(SERVICE_UUID)?.getCharacteristic(CHAR_UUID)
            Log.i(TAG, "services discovered: status=$status characteristicFound=${characteristic != null}")
            if (characteristic != null) {
                RelayStateHolder.noteBleConnected()
            }
        }
    }

    companion object {
        private const val TAG = "RelayBleClient"

        // Fixed OpenApex BLE identifiers — see docs/OpenApex_SPEC.md §5.3.
        val SERVICE_UUID: UUID = UUID.fromString("c9c6d0a0-0001-4f0a-9c8e-2f6b1a2d3e4f")
        val CHAR_UUID: UUID = UUID.fromString("c9c6d0a0-0002-4f0a-9c8e-2f6b1a2d3e4f")

        /** ATT opcode (1) + attribute handle (2) ahead of the value in a write PDU. */
        const val ATT_HEADER_BYTES = 3

        /** The BLE-spec default ATT MTU, i.e. a 20-byte payload. Assumed until an exchange says otherwise. */
        const val ATT_MTU_DEFAULT = 23

        // RawNotifPacket is 146 bytes; + the 3-byte ATT header, with headroom.
        private const val MTU_BYTES = RAW_NOTIF_PACKET_SIZE + ATT_HEADER_BYTES + 16
    }
}
