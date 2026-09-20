package org.openapex.androidauto

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattServer
import android.bluetooth.BluetoothGattServerCallback
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelUuid
import java.util.UUID

/**
 * BLE peripheral: advertises one service with one NOTIFY characteristic carrying
 * [RawNotifPacket] bytes. The phone is peripheral/advertiser; the ESP32 terminal is the central
 * that connects and subscribes. Mirrors the OpenApex BLE identifiers from docs/OpenApex_SPEC.md
 * (same pair the ESP32 firmware decodes against).
 */
class RelayBleServer(private val context: Context) {

    private val bluetoothManager = context.getSystemService(BluetoothManager::class.java)
    private var gattServer: BluetoothGattServer? = null
    private var characteristic: BluetoothGattCharacteristic? = null
    private var running = false

    fun start() {
        if (!hasPermissions()) return
        if (running) return

        val adapter = bluetoothManager?.adapter ?: return
        val char = BluetoothGattCharacteristic(
            CHAR_UUID,
            BluetoothGattCharacteristic.PROPERTY_READ or BluetoothGattCharacteristic.PROPERTY_NOTIFY,
            BluetoothGattCharacteristic.PERMISSION_READ,
        )
        // Central-mode subscribers (the ESP32) discover and write this CCCD to enable
        // notifications; without it there is nothing for descriptor discovery to find.
        val cccd = android.bluetooth.BluetoothGattDescriptor(
            CCCD_UUID,
            android.bluetooth.BluetoothGattDescriptor.PERMISSION_READ or android.bluetooth.BluetoothGattDescriptor.PERMISSION_WRITE,
        )
        char.addDescriptor(cccd)
        val service = BluetoothGattService(SERVICE_UUID, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        service.addCharacteristic(char)

        val server = bluetoothManager.openGattServer(context, object : BluetoothGattServerCallback() {
            override fun onConnectionStateChange(device: android.bluetooth.BluetoothDevice, status: Int, newState: Int) {
                if (newState != BluetoothGatt.STATE_CONNECTED) {
                    RelayStateHolder.noteBleDisconnected()
                } else {
                    RelayStateHolder.noteBleConnected()
                }
            }

            @SuppressLint("MissingPermission")
            override fun onCharacteristicReadRequest(
                device: android.bluetooth.BluetoothDevice,
                requestId: Int,
                offset: Int,
                characteristic: BluetoothGattCharacteristic,
            ) {
                gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, RelayStateHolder.latestPacket())
            }

            @SuppressLint("MissingPermission")
            override fun onDescriptorWriteRequest(
                device: android.bluetooth.BluetoothDevice,
                requestId: Int,
                descriptor: android.bluetooth.BluetoothGattDescriptor,
                preparedWrite: Boolean,
                responseNeeded: Boolean,
                offset: Int,
                value: ByteArray,
            ) {
                if (responseNeeded) {
                    gattServer?.sendResponse(device, requestId, BluetoothGatt.GATT_SUCCESS, offset, value)
                }
            }
        })
        server.addService(service)
        characteristic = char
        gattServer = server
        running = true

        // No device name: a 128-bit service UUID (18 bytes with AD header) plus a device name
        // plus mandatory flags (3 bytes) overflows the 31-byte legacy advertising payload,
        // causing startAdvertising to fail silently (ADVERTISE_FAILED_DATA_TOO_LARGE). The ESP32
        // central only filters on the service UUID, so the name isn't needed.
        val advData = AdvertiseData.Builder()
            .addServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setConnectable(true)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
            .build()
        @Suppress("DEPRECATION")
        adapter.bluetoothLeAdvertiser.startAdvertising(settings, advData, advertiseCallback)
    }

    fun stop() {
        if (!running) return
        try {
            @Suppress("DEPRECATION")
            bluetoothManager?.adapter?.bluetoothLeAdvertiser?.stopAdvertising(advertiseCallback)
        } catch (_: Exception) {
        }
        gattServer?.clearServices()
        gattServer?.close()
        gattServer = null
        characteristic = null
        running = false
    }

    @SuppressLint("MissingPermission")
    fun notify(packet: ByteArray) {
        val server = gattServer ?: return
        val char = characteristic ?: return
        val devices = bluetoothManager?.getConnectedDevices(android.bluetooth.BluetoothProfile.GATT)
        val device = devices?.firstOrNull() ?: return
        char.value = packet
        server.notifyCharacteristicChanged(device, char, false)
    }

    private fun hasPermissions(): Boolean {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S && context.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) return false
        return true
    }

    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartFailure(errorCode: Int) {
            android.util.Log.e("RelayBleServer", "startAdvertising failed: errorCode=$errorCode")
        }
    }

    companion object {
        // Fixed OpenApex BLE identifiers — see docs/OpenApex_SPEC.md §5.3.
        val SERVICE_UUID: UUID = UUID.fromString("c9c6d0a0-0001-4f0a-9c8e-2f6b1a2d3e4f")
        val CHAR_UUID: UUID = UUID.fromString("c9c6d0a0-0002-4f0a-9c8e-2f6b1a2d3e4f")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    }
}
