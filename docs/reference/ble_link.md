# BLE link (phone ↔ terminal)

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md §5 intro, §5.1, §5.3, §5.5, §13 item 4 (restructure, 2026-09-29)  
**Last verified against code:** not yet

## Overview

*Was SPEC §5.*

The final architecture supports both platform paths while keeping the terminal firmware source-
agnostic.

## Android path

*Was SPEC §5.1.*

The C3 terminal is the BLE peripheral: it advertises a custom GATT service containing a navigation
characteristic and accepts one bonded central. The Android helper is the BLE central/GATT client —
it associates with the terminal via `CompanionDeviceManager` (CDM) observer mode, connects, and
writes each normalized navigation packet into the characteristic (see [decisions](decisions.md) ADR-5 for the rationale).

This matches the role split used by essentially every consumer BLE accessory (fitness bands,
earbuds, Gadgetbridge/InfiniTime-class open trackers): the small, single-purpose, battery-constrained
device advertises and lets the phone do discovery, bonding, and reconnection management. It also
gives the "bike on -> terminal boots -> phone reconnects automatically" UX without opening the app:
CDM observer mode wakes a `CompanionDeviceService` from a killed process when the terminal's
advertisement is seen again, which starts the relay and connects.

It also leaves room for a later ESP32-to-ESP32 link (e.g. a handlebar terminal and a helmet or TPMS
sensor unit): with the terminal already acting as peripheral/GATT-server, a second ESP32 can attach
as another BLE central against the same or a second service, with no role renegotiation on the
terminal side.

## Project BLE identifiers

*Was SPEC §5.3.*

The Android implementation currently defines:

- Service UUID: `c9c6d0a0-0001-4f0a-9c8e-2f6b1a2d3e4f`
- Navigation characteristic UUID: `c9c6d0a0-0002-4f0a-9c8e-2f6b1a2d3e4f`

The navigation characteristic is `WRITE` / `WRITE_NO_RESPONSE`, encrypted (`BLE_GATT_CHR_F_WRITE_ENC`),
not `NOTIFY` — the phone (central) writes, the terminal (peripheral/GATT server) receives. No write
acknowledgement is required because a stale packet is always superseded by the next update rather
than retried.

The terminal must reject unsupported protocol versions and malformed lengths. BLE callbacks must
not block on rendering. Parsed packets are copied into a bounded queue or immutable state buffer.

Incoming calls use two more characteristics in the same service (layout in
`firmware/main/call_packet.h`, mirrored by `CallPacket.kt`):

- Call event `c9c6d0a0-0003-4f0a-9c8e-2f6b1a2d3e4f`, phone → terminal. Same flags and fragment
  framing as the navigation characteristic, with its own reassembly. It carries RING/CLEAR, a
  `call_id` and the caller string.
- Call action `c9c6d0a0-0004-4f0a-9c8e-2f6b1a2d3e4f`, terminal → phone. `NOTIFY`, with an
  encrypted CCCD. It carries ACCEPT plus the `call_id`.

**Fragmentation, and the single write that replaces it.** Every packet is normally split into
20-byte writes (3-byte header + 17 payload) that fit the 23-byte default ATT MTU on any stack.
That is a workaround, not a design: on the 2026-09-24 captures an Android stack reported
`GATT_REQUEST_NOT_SUPPORTED` for the MTU exchange all session and truncated every write to 20
bytes regardless of the MTU it had agreed, so nothing may depend on that exchange for
correctness. iOS negotiates 185 and honours it, where the split costs 12 extra writes per packet
to route around a bug that is not there.

So the terminal also accepts the whole 210-byte packet in a single write, advertised as `caps`
bit 1. The two framings are told apart by length rather than a header version field: a fragment
write is at most 20 bytes, so a 210-byte write cannot be one, and the packet's own version byte
already covers format changes. Nothing is negotiated and a writer may switch per packet -- which
matters, because the MTU is only known after an exchange that on Android lies about itself.

## Incoming calls

*Was SPEC §5.5.*

When the phone rings, the terminal shows the caller name and number
(`design/screens/05-calling/OpenApex Call Screens.svg`, `call_screen.cpp`). Accept is a 2 s hold
anywhere on the page, traced by a theme-coloured ring; letting go early retraces it. Once answered
the page shows the call duration until the call ends. There is deliberately no reject: the rider
lets it ring out.

CALL_EVENT (64 B, `call_packet.h`) carries type RING / ACTIVE / CLEAR, the call id, the caller name
(empty when the dialer only knows the number: shown as "Unknown Caller") and the number.

**Android (implemented).**
- **Source.** The existing NotificationListener picks up any `CATEGORY_CALL` notification that is
  incoming (`CallRelay.kt`).
  - On API 31+, `EXTRA_CALL_TYPE` identifies incoming calls.
  - Below API 31, the notification must have an answer action.
  - The caller is `EXTRA_TITLE`, exactly as the dialer shows it; a title that is itself a number
    is sent as the number with an empty name. No call-log or phone-state permission is needed, and
    VoIP apps that post CallStyle work too.
  - The number comes from the caller `Person` (`tel:` uri), the notification text, or -- for a
    saved contact, whose `Person` carries a contacts uri -- a contacts lookup behind the optional
    `READ_CONTACTS` permission. Without it a contact shows as "Saved contact" in the bubble.
- **Answered.** The same notification reposted as ongoing sends ACTIVE (answered on the terminal
  or on the phone); its removal sends CLEAR. An ongoing call never seen ringing (outgoing) is
  ignored. The duration is timed on the terminal from the first ACTIVE.
- **Refresh.** RING or ACTIVE is re-sent every 5 s while the call lasts. The terminal drops a ring not
  refreshed within 12 s, or immediately on link loss (`call_state.c`), so a lost CLEAR cannot
  strand the page.
- **Accept.** The tap goes through `gui_app_set_call_accept_handler` to `call_accept_queue`, then
  `countdown_task` sends the CALL_ACTION notify. The GUI never touches BLE ([system_overview](system_overview.md)). On the phone,
  `CallRelay.onAccept` fires the notification's own answer `PendingIntent`.
  - On API 34+ it is sent with background-activity-start opt-in (`ALLOW_ALWAYS` on 36, otherwise
    `ALLOWED`).
  - The fallback is `TelecomManager.acceptRingingCall()`, with the optional
    `ANSWER_PHONE_CALLS` permission (carrier calls only).
- **Routing.** A ringing or answered call outranks every page, including an active maneuver. The nav state keeps
  updating underneath and returns when the call clears.

**iOS (TODO, not implemented).** A third-party iOS app cannot observe or answer cellular calls, so
there is no companion-app work. The whole path is terminal-side ANCS, on the same client as [ios_ancs](ios_ancs.md):
- Notification Source reports `CategoryIDIncomingCall` (1) *added*. The terminal then sends Get
  Notification Attributes for `Title`, which is the caller name or number.
- `call_state` is fed with the ANCS UID as the call id.
- Accept writes Perform Notification Action (`ActionIDPositive`) for that UID to the Control
  Point.
- The notification being *removed* clears the call. Ringing out works with no extra logic.
- Needs bonding with the iPhone, ANCS service solicitation in the advertisement, and the terminal
  acting as a GATT client alongside its peripheral role.
- `CallScreen` and `call_state` are source-agnostic and are reused unchanged.

## Open decisions

*Was SPEC §13 item 4.*

4. Finalize packet version 1 byte layout, sentinels, and whether source identifiers fit in the first
   packet or use a second characteristic.
