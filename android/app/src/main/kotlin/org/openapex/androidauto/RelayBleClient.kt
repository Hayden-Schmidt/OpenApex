package org.openapex.androidauto

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothProfile
import android.content.Context
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

    @SuppressLint("MissingPermission")
    fun connect(device: BluetoothDevice) {
        Log.i(TAG, "connecting to ${device.address}")
        gatt?.close()
        gatt = device.connectGatt(context, true, callback)
    }

    @SuppressLint("MissingPermission")
    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        characteristic = null
    }

    @SuppressLint("MissingPermission")
    @Suppress("DEPRECATION") // writeCharacteristic(char, type, value) needs API 33; minSdk is 26
    fun write(packet: ByteArray) {
        val g = gatt ?: return
        val char = characteristic ?: return
        char.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        char.value = packet
        g.writeCharacteristic(char)
    }

    private val callback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            Log.i(TAG, "connection state change: status=$status newState=$newState")
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                g.discoverServices()
            } else {
                characteristic = null
                RelayStateHolder.noteBleDisconnected()
            }
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
    }
}
