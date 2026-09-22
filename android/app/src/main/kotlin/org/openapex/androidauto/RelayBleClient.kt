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
        char.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        char.value = packet
        val ok = g.writeCharacteristic(char)
        Log.i(TAG, "write: len=${packet.size} queued=$ok")
    }

    private val callback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            Log.i(TAG, "connection state change: status=$status newState=$newState")
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                // Default ATT MTU is 23 bytes (20-byte payload after the 3-byte ATT header), far
                // smaller than the 144-byte RawNotifPacket. Without negotiating a larger MTU
                // first, writeCharacteristic() silently truncates every packet down to whatever
                // the current MTU allows, which the firmware then rejects as malformed rather
                // than decoding a partial packet.
                g.requestMtu(MTU_BYTES)
            } else {
                characteristic = null
                connectedAddress = null
                RelayStateHolder.noteBleDisconnected()
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            Log.i(TAG, "mtu changed: mtu=$mtu status=$status")
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

        // RawNotifPacket is 144 bytes; +3 for the ATT opcode/handle header, rounded up.
        private const val MTU_BYTES = 153
    }
}
