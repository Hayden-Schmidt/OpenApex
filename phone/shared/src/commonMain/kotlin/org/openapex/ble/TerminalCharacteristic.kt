package org.openapex.ble

/**
 * The terminal's GATT characteristics (spec §5.3), named rather than carried as UUIDs so the
 * shared layer never holds a platform type. Android maps these to `java.util.UUID` and iOS to
 * `CBUUID`; only that mapping differs.
 */
enum class TerminalCharacteristic {
    /** Nav packets, phone -> terminal, fragmented, without response. */
    NAV,

    /** Incoming call, phone -> terminal (CallPacket.kt / firmware call_packet.h). */
    CALL_EVENT,

    /** Accept/reject, terminal -> phone, by notification. */
    CALL_ACTION,

    SETTINGS,
    DEVICE_INFO,
    AVATAR,
    IDENTITY,
    FRIENDS,
}
