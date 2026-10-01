# Terminal settings, device info and avatar

**Kind:** Reference — describes the code as built.  
**Moved from:** OpenApex_SPEC.md §5.6 (restructure, 2026-09-29)  
**Last verified against code:** not yet

## Terminal settings, device info and avatar

*Was SPEC §5.6.*

Three further characteristics on the same service let the phone configure the terminal UI. All
settings and the uploaded avatar persist on the terminal (NVS namespace `settings`, keys `tlv` and
`avatar`) and are loaded before the splash, so the rider's configuration shows on every boot with
no phone in range. The default ATT MTU is 23, so settings writes stay at 20 bytes or less.

| UUID | Properties | Purpose |
|---|---|---|
| `c9c6d0a0-0005-4f0a-9c8e-2f6b1a2d3e4f` | READ, WRITE (both encrypted) | Settings TLV |
| `c9c6d0a0-0006-4f0a-9c8e-2f6b1a2d3e4f` | READ | Device info |
| `c9c6d0a0-0007-4f0a-9c8e-2f6b1a2d3e4f` | READ, WRITE (both encrypted) | Custom avatar |
| `c9c6d0a0-0008-4f0a-9c8e-2f6b1a2d3e4f` | READ, WRITE (both encrypted) | Rider identity ([rider_mesh_v1](rider_mesh_v1.md)) |
| `c9c6d0a0-0009-4f0a-9c8e-2f6b1a2d3e4f` | READ, WRITE, NOTIFY (encrypted) | Friend list ([rider_mesh_v1](rider_mesh_v1.md)) |

**Settings TLV.** `[schema=0x01]` followed by records of `[tag u8][len u8][value]`.

| Tag | Len | Field | Values | Default |
|---|---|---|---|---|
| 0x01 | 3 | Accent colour | RGB888, big-endian | `00 FF E8` |
| 0x02 | 1 | Avatar | 0 default character (the dragon, preset 8), 1-9 preset character, 0xFF custom upload | 0 |
| 0x03 | 1 | ETA format | 0 arrival time, 1 time left | 1 |
| 0x04 | 1 | Clock | 0 24-hour, 1 12-hour (`H:MM`, no am/pm) | 0 |
| 0x05 | 1 | Units | 0 metric, 1 imperial (distances) | 0 |
| 0x06 | 1 | Default dial outer ring | 0 compass, 1 trip | 0 |
| 0x07 | 1 | Friends on the compass | 0 hidden, 1 shown | 1 |
| 0x08 | 1 | Friend marker style | 0 their character, 1 a dot | 0 |
| 0x09 | 1 | Rider mesh radio | 0 silent, 1 on | 1 |
| 0x0A | 1 | Odometer page once riding | 1: over 5 km/h with no navigation the odometer page comes up; after 3 min stopped it goes back to idle | 1 |

- A read returns every field. Golden fixture for the defaults (33 bytes):
  `01 01 03 00 FF E8 02 01 00 03 01 01 04 01 00 05 01 00 06 01 00 07 01 01 08 01 00 09 01 01 0A 01 01`.
  Tags 0x07-0x0A push the read past the 20-byte default MTU, so a full read needs a negotiated
  MTU or an ATT read-blob. A phone that sees only the first 20 bytes still parses the records it
  did get, which is the reason this is a record stream and not a fixed struct.
- A write may carry any subset of records; untouched fields keep their value. Split a full config
  across several writes to stay within 20 bytes.
- Unknown tags are skipped (forward compatibility). A wrong schema byte, truncated record, wrong
  length or out-of-range value rejects the whole write with an ATT error, and nothing is applied.
- The terminal applies a write live and saves it to flash after 1 s without further changes.

**Device info.** `[info_ver=1][board_id][gfx_tier][settings_schema][caps][avatar_px u16 LE][fw version, ASCII]`.
`board_id` is 1 for C3 and 2 for S3. `caps` bit 0 means custom avatar upload is supported,
bit 1 that the navigation characteristic also accepts a whole packet in one write, and bit 2
that the terminal reads ANCS and CTS off the phone itself ([ios_ancs](ios_ancs.md)), so an iOS app need not --
and cannot -- relay notifications or the clock.
`avatar_px` is the edge an avatar is drawn at (96 on C3, 192 on S3), for previews.

**Preset avatars.** The nine presets (crab, owl, dino, octopus, chick, slime, bear, dragon,
robot, in tag order) are full-colour pixel art built into the firmware from
`design/characters` (`<name>.png` up to 32x32, the small versions in `small.png`). Each
keeps a palette of at most 10 colours. Colours tied to the character's main colour follow the accent;
the rest (pupils, eye whites, beaks, feet) are fixed. The outline is a themed colour at 20% brightness so it shows on the black screen. The main colour is the accent's hue at the
character's own saturation and brightness (as a fraction of full, multiplied by the accent's). Each
other themed colour is the main colour turned by a hue offset, with saturation and brightness scaled
by a ratio, so a pink slime and a blue slime shade the same way. The 32x32 is drawn at a
whole-number scale.

**Custom avatar art.** An uploaded avatar is colour-agnostic: 2 bits per pixel, drawn in the accent.
Levels are 0 transparent (background, and holes such as eyes), 1 about 1/3 accent (outline,
shadow), 2 about 2/3 (body) and 3 full accent (highlight). Pixels are row-major, four per byte,
first pixel in the high bits. An avatar is 80 bytes: the 16x16 image (64 B) then an 8x8 version of
the same character (16 B) for small surfaces. The 16x16 is drawn at a whole-number scale.

**Custom avatar.** A read returns the stored 80 bytes, or nothing when none is stored. A write of
exactly 80 bytes replaces it and an empty write deletes it; any other length is rejected. The
value is longer than one packet, so the phone sends it as an ATT long write (prepare/execute,
Android's default write type does this); the terminal only sees the whole value, so an interrupted
upload changes nothing. Selecting 0xFF with nothing stored shows the default character.
