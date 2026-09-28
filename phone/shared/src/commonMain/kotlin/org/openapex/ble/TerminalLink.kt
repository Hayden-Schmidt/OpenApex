package org.openapex.ble

import org.openapex.protocol.AvatarArt
import org.openapex.protocol.DeviceInfo
import org.openapex.protocol.TerminalSettings

/**
 * An opaque handle to one terminal, as the platform names it.
 *
 * Android uses the Bluetooth MAC, which is stable and public. iOS never exposes a MAC and gives a
 * `CBPeripheral.identifier` instead -- a UUID scoped to that install, so it changes if the app is
 * reinstalled and differs between two phones looking at the same terminal. Nothing above this
 * layer may parse, compare across platforms, or display it.
 *
 * Android's value is the MAC string it already stored, so there is nothing to migrate.
 */
@JvmInline
value class DeviceId(val value: String)

/**
 * Everything the app does to a terminal. The implementation is `RelayBleClient` on Android and
 * `IosTerminalLink` on iOS; between them they are the only classes allowed to know about
 * BluetoothGatt or CoreBluetooth.
 *
 * Callback-shaped, matching the queue underneath it. Connection state is not returned here: it is
 * observed through the app's existing state holder, because a link can drop without anyone having
 * asked it for anything.
 */
interface TerminalLink {
    /**
     * Connects to [id], possibly after bonding first. [background] asks the platform to park the
     * request until the terminal next advertises rather than connecting now, which is how a
     * powered-off terminal is picked back up without scanning.
     */
    fun connect(id: DeviceId, background: Boolean = false)

    fun disconnect()

    /** Sends a nav packet, fragmented, without response. Dropped when not connected. */
    fun write(packet: ByteArray)

    /** Sends a CALL_EVENT packet, ahead of any queued nav fragments. */
    fun writeCall(packet: ByteArray)

    fun readSettings(done: (TerminalSettings?) -> Unit)
    fun writeSettings(settings: TerminalSettings, done: (Boolean) -> Unit = {})
    fun readDeviceInfo(done: (DeviceInfo?) -> Unit)
    fun readAvatar(done: (AvatarArt?) -> Unit)
    fun uploadAvatar(art: AvatarArt, done: (Boolean) -> Unit = {})
    fun clearAvatar(done: (Boolean) -> Unit = {})

    /** Pushes the phone's friend list again, e.g. after the rider removes one in the app. */
    fun pushFriends()
}
