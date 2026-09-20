package org.openapex.androidauto

/**
 * Process-wide bridge between the listener service, GNSS/BLE (RelayService), and the BLE read
 * path. Sources emit [RelayStateEvent]s; [RelayService] is the single observer that repacks and
 * publishes. The latest packed packet is stored here so GATT read requests can serve it without
 * reaching into the service.
 */
sealed class RelayStateEvent {
    data class NavUpdated(val nav: RawNavNotification?) : RelayStateEvent()
    object ListenerConnected : RelayStateEvent()
    object ListenerDisconnected : RelayStateEvent()
    object BleConnected : RelayStateEvent()
    object BleDisconnected : RelayStateEvent()
}

object RelayStateHolder {
    private var observer: ((RelayStateEvent) -> Unit)? = null
    private var packet: ByteArray = ByteArray(RAW_NOTIF_PACKET_SIZE)

    fun attach(fn: (RelayStateEvent) -> Unit) {
        observer = fn
    }

    fun detach() {
        observer = null
    }

    fun updateNav(nav: RawNavNotification?) = emit(RelayStateEvent.NavUpdated(nav))
    fun noteListenerConnected() = emit(RelayStateEvent.ListenerConnected)
    fun noteListenerDisconnected() = emit(RelayStateEvent.ListenerDisconnected)
    fun noteBleConnected() = emit(RelayStateEvent.BleConnected)
    fun noteBleDisconnected() = emit(RelayStateEvent.BleDisconnected)

    fun latestPacket(): ByteArray = packet

    fun setLatestPacket(bytes: ByteArray) {
        packet = bytes
    }

    private fun emit(event: RelayStateEvent) {
        observer?.invoke(event)
    }
}
