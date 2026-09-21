package org.openapex.androidauto

/**
 * Process-wide bridge between the listener service and GNSS/BLE (RelayService). Sources emit
 * [RelayStateEvent]s; [RelayService] is the single observer that repacks and publishes.
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

    private fun emit(event: RelayStateEvent) {
        observer?.invoke(event)
    }
}
