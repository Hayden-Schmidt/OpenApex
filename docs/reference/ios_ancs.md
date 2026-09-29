# iOS / ANCS path

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md §5.2 (restructure, 2026-09-29)  
**Last verified against code:** not yet

## iOS/ANCS path

*Was SPEC §5.2.*

iOS has no notification listener, no call observation and no permanent background service, so an
iOS companion app cannot be the nav source the way the Android relay is. The terminal reads the
notifications off the iPhone itself, over Apple's ANCS. That is the reason normalization lives in
C on the terminal ([system_overview](system_overview.md)): the same normalizer serves both platforms, or it would be written
twice and drift.

**Roles.** The terminal stays the *peripheral* exactly as in [ble_link](ble_link.md) -- the phone connects to it.
On that one connection the terminal is both the GATT **server** for its own service and a GATT
**client** of the iPhone's Apple services. Central/peripheral and client/server are orthogonal,
and ANCS is only offered to an accessory acting as the client, so this is required rather than a
choice. NimBLE gates every `ble_gattc_*` symbol behind `CONFIG_BT_NIMBLE_ROLE_CENTRAL`, which is
therefore enabled even though the terminal never initiates a connection.

**Getting iOS to offer ANCS.** The terminal puts an ANCS *service solicitation*
(`7905F431-B5CE-4E99-A40F-4B1E122D00D0`) in its advert, and initiates pairing on connect: every
ANCS characteristic requires an encrypted link, so discovering before the bond exists only earns
an ATT error. The 31-byte legacy advert cannot hold flags, our 128-bit service UUID, the
solicitation and the name at once, so the primary advert keeps flags plus our service UUID (which
is what Android's CompanionDeviceManager filters on) and the solicitation and name move to the
scan response.

iOS publishes and unpublishes ANCS at will, so the terminal subscribes to GATT Service Changed
(`0x1801` / `0x2A05`) and rediscovers rather than treating one discovery pass as final.

**What ANCS carries, and what it does not.** Per notification: `AppIdentifier`, `Title`,
`Subtitle`, `Message`, `Date` -- text only. There is no maneuver glyph, no progress, no traffic,
no speed, heading or position. So on iOS the maneuver comes from text matching alone (a
roundabout's exit direction, which on Android the glyph supplies, is unknown), and progress and
traffic render as unknown rather than as fabricated zeros ([nav_pipeline](nav_pipeline.md)). Google Maps posts turn-by-turn
under `CategoryIDLocation`; Apple Maps posts no notifications at all and cannot be a nav source.
Incoming calls arrive as `CategoryIDIncomingCall` and are answered with the notification's
positive action ([ble_link](ble_link.md)) rather than the Android CALL_ACTION characteristic.

**Clock.** The C3 has no RTC and on iOS there is no packet to carry a time whenever the companion
app is asleep, which iOS will make it be. The terminal therefore also reads and subscribes to the
Current Time Service (`0x1805` / `0x2A2B`). CTS carries no zone, so what it yields is local time
treated as an epoch, which is all the clock face needs.

**Telemetry.** ANCS and CTS both work with no app running, which is what the iOS design rests on.
Everything else -- GNSS, speed, heading, motion, phone battery -- still needs the companion app
writing the packet characteristic, so the iOS app is not optional, it is the telemetry half. When
iOS kills it the terminal goes telemetry-blind while nav and the clock keep working; that gap is
surfaced to the rider rather than papered over.

**Packet v6 and the merge.** Nav and telemetry arrive in one packet on Android and in two on iOS,
so the terminal needs to tell a telemetry-only write apart from a route that has ended -- they
look identical on the wire. v6 changes no offsets: it adds two flag bits beside `gnss_fix_valid`
in byte 1, `bit1 nav_fields_absent` and `bit2 source_is_ios`. v5 is still decoded and is read as
carrying both halves, which is what the Android relay in fact sends; the version check was strict
equality until now and the compatibility window is deliberately one version wide.

`packet_merge.c` holds the two halves independently and overwrites only the half a packet speaks
for, so an ANCS nav update cannot blank the app's speed and a telemetry write cannot end the
route. Freshness is per half, not per field: there are exactly two producers and neither varies
its fields independently. Each half reverts to its sentinels once its producer goes quiet -- nav
after 10 s, matching the countdown staleness rule that already governs the nav page, and telemetry
after 5 s, so an iOS app killed in the background blanks the dial rather than freezing a plausible
last speed on it ([nav_pipeline](nav_pipeline.md)). On Android, where every packet carries both halves, the merge is a
passthrough and the wire behaviour is unchanged.

Phone-side settings, firmware-update and device-info contracts are planned in
[Phone_App_Architecture.md](Phone_App_Architecture.md) [hardware](hardware.md) and must land here before implementation.
