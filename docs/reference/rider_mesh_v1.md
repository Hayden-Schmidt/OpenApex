# Rider mesh (v1, as built)

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md §5.7 (restructure, 2026-09-29)  
**Last verified against code:** not yet

> Planned successor: [blueprint 04](../blueprints/04_rider_mesh.md) (wire v2). This doc
> stays accurate to the code until 04.1 ships.

## Rider mesh

*Was SPEC §5.7.*

Terminals within radio range of each other show their riders as markers on the compass. The
phone is not in the path: two terminals talk directly, so the mesh keeps working where there is
no cell coverage, which is where a group ride most wants it.

**Transports.** BLE extended advertising on the Coded PHY is the baseline every board speaks, so
any two terminals can meet. Boards with the RAM for Wi-Fi (the S3, not the C3) additionally run
ESP-NOW for more range and bandwidth. A frame goes out on every transport the board has; the
receive side de-duplicates, so a board running both is not heard twice
(`firmware/main/mesh_transport.h`).

**Frame header.** Every frame is 14 bytes of header then a type-specific body:

    'O' 'A' | proto_ver=1 | type | sender rider_id (8) | seq u16 LE

A wrong magic or version is dropped and an unknown type is ignored, so a newer terminal's frames
pass harmlessly through an older one. `MESH_FRAME_MAX` is 250 bytes (the ESP-NOW v1 payload
limit). Multi-byte fields are little-endian except colours, which are RGB big-endian as in the
settings TLV.

| Type | Name | Body |
|---|---|---|
| 0x01 | HELLO | The sender's look (avatar, accent, name, optional 80-byte custom art) + caps |
| 0x02 | FRIEND_ACCEPT | Target rider_id (8) + the sender's rider key (16) |
| 0x03 | FRIEND_ACK | Target rider_id (8) |
| 0x04 | POS | Encrypted position (see below) |
| 0x10-0x1F | reserved | Nav share, Phase 2: encrypted like POS, fragmented |

HELLO goes out every 2 s in clear: it is what makes a stranger visible enough to be friended,
and it carries nothing a passer-by could not see by looking at the bike. Position never does.

**Friending is a mutual physical act.** Both riders hold the ring on the friend prompt. Holding
sends FRIEND_ACCEPT (retried up to 5 times at 1 s, stopped by the peer's FRIEND_ACK), and the
friendship completes on a terminal only once it has both held locally and received the other's
accept. So one rider cannot add another unilaterally, and a rider who declines simply never
holds. A peer that has not been heard from for 10 s drops out of range.

The prompt may cover the nav page only when it cannot hide a turn the rider needs: under
20 km/h, or the next maneuver more than 50 m away, or not navigating at all
(`mesh_core.h`). An unanswered prompt lapses after 20 s on screen.

**Position privacy.** POS frames are AES-128-CCM (RFC 3610, 13-byte nonce, 8-byte tag) under the
*sender's own* key, which only riders it has friended hold. A non-friend hears the ciphertext and
learns nothing; a forged frame fails the tag and is discarded rather than decoded into a
plausible position (`mesh_crypto.h`). The nonce is the sender id, a random per-boot `session`
value re-rolled when `seq` wraps, and `seq` itself. The plaintext is lat/lon at 1e-7 degrees,
speed and heading at 0.1 units with 0xFFFF for unknown, and horizontal accuracy in metres. POS
goes out once a second, and a fix of ours older than 5 s is not shared at all rather than shared
stale.

**Phone-facing values.** Two further characteristics on the same service, both bonded-only in
each direction because they carry the rider key:

- Identity `c9c6d0a0-0008-...`, READ/WRITE: `[ver=1][rider_id 8][rider_key 16][name_len u8]
  [name <= 12 bytes UTF-8]`. The phone owns the identity and the key; the terminal stores them.
- Friends `c9c6d0a0-0009-...`, READ/WRITE/NOTIFY. A write is an op byte then its argument:
  `01` upsert (+ a 122-byte record), `02` remove (+ rider_id 8), `03` clear, `04` set the read
  cursor (+ index u8). A read returns the record at the cursor, or zero bytes past the end,
  which is how the phone knows the list has ended. A friendship completed on the terminal is
  notified as an upsert, so the two lists converge without the phone polling.

A friend record is a fixed 122 bytes: `rider_id 8 | rider_key 16 | avatar u8 | accent RGB 3 |
flags u8 | name_len u8 | name 12 (zero-padded) | art 80 (zeros unless flags bit 0)`.

**Limits.** 8 peers in range, 16 friends, names 12 bytes of UTF-8. Tags 0x07-0x09 of the
settings TLV control whether friends are drawn on the compass, whether they are drawn as their
character or a dot, and whether the mesh radio runs at all.

**Terminal-side structure.** `mesh_core.c` is the whole protocol with no radio, no clock and no
NVS -- frames and time go in, a send callback goes out -- so it runs unchanged in host tests.
`mesh.c` is the only thing that knows about ESP-IDF: it owns the single instance behind a mutex,
drives the transports, and persists identity and friends to NVS.
