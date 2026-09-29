# Rider Mesh — Private Friending, Group Rides, Nav Share, Relay — Design Plan

Status: **planned, not started. Revision 2 (2026-09-29)**, after review against the code. This
extends docs/OpenApex_SPEC.md §5.7 rather than replacing it: the transports, the 14-byte header
shape, AES-CCM position privacy and the phone-side sync are kept except where this doc says
otherwise.

**Naming.** The work is split into four *stages* (Stage 1-4 below). They are deliberately not
called "phases": SPEC §11, the iOS work (Phase A/B/C) and `mesh_proto.h`'s "Phase 2" comment all
already use that word for other things. When the docs restructure (below) lands, this doc becomes a
numbered blueprint and the stages become that blueprint's phases (`N.1`-`N.4`).

## Before any of this starts

1. **Docs and repo restructure lands first, as its own step.** Queued; see "Queued: docs and repo
   restructure" at the end. No rider-mesh code until it is merged.
2. **A second terminal.** A second, screenless C3 is available during development; it is the board
   that verified the existing §5.7 friending. It covers the radio side of Stages 1-3. The screenless
   side has no UI, so its half of each flow is driven and checked over serial (a debug console
   command per U-row action, e.g. "open friend menu", "request friend", "start ride"). Every stage is
   also host-tested (`firmware/test_host/mesh_test.c`).
3. **C3 heap measurement** with the phone link, the Coded-PHY mesh advert/scan and the ANCS client
   all live (open item in `iOS_Handoff.md` §4). Every stage adds RAM; measure the baseline first.

## Scope at a glance

| Stage | What | Radio | Boards |
|---|---|---|---|
| 1 | Menu-only friending, private discovery, rotating pseudonyms, encrypted key exchange, corrected relative positioning | 1-hop | C3 + S3 |
| 2 | Group rides: open / friends / request policies, ride positions on the compass, ride screen | 1-hop | C3 + S3 |
| 3 | Leader nav share, delivered where the leader was | 1-hop | C3 + S3 |
| 4 | **Deferred.** Multi-hop relay, ESP-NOW boards only | multi-hop | S3 relays; C3 is a non-relaying leaf |

Out of scope for all stages: phone UI (comes after the architecture is mapped), non-touch boards
(not supported at this stage; revisit only if ever needed), simultaneous membership of two rides.

---

## Terminal UI — owner input goes here

Terminal flows are still moving with the hardware and will eventually be configurable from the
phone. This doc fixes **only** the protocol and state machines; every surface below is a hook the
owner designs (Figma) and fills in when it is needed. Implementation of a surface waits for its row.

| # | Surface | Stage | What the firmware provides | Flow (owner) | Design link (owner) |
|---|---|---|---|---|---|
| U1 | Open the friend menu | 1 | Entering the menu makes us discoverable (see "Discovery") | TBD | TBD |
| U2 | Friend menu list | 1 | Discoverable riders in range: pseudonym + look | TBD | TBD |
| U3 | Send a request | 1 | `mesh_core_request(pseudonym)` | TBD | TBD |
| U4 | Incoming request prompt | 1 | Replaces today's automatic prompt; same prompt gate | TBD (hold ring today) | TBD |
| U5 | "Now friends" / request lapsed | 1 | Handshake outcome events | TBD | TBD |
| U6 | Start a ride, choose policy | 2 | `mesh_core_ride_start(policy)` | TBD | TBD |
| U7 | Rides nearby / join | 2 | Ride adverts heard in range | TBD | TBD |
| U8 | Approve a join (request policy) | 2 | Incoming join request, first approver wins | TBD | TBD |
| U9 | On-ride screen | 2 | Selected by `SETTINGS_TAG_RIDE_SCREEN` | TBD | TBD |
| U10 | Leave / end ride | 2 | Leave (any member), end (starter) | TBD | TBD |
| U11 | Start / stop nav share | 3 | Starter only, while navigating | TBD | TBD |
| U12 | "Alongside" marker treatment | 1 | `close` flag per marker (see "Relative positioning") | TBD | TBD |

Designs live under `design reference/` (today's friend prompt is `design reference/7. Add Freind
Screen`); the restructure fixes the final folder name.

---

## Stage 1 — Private friending

### 1.1 Friending is menu-only

The automatic prompt goes. Today `run_prompt` (`mesh_core.c:265`) offers the *first* unsuppressed
stranger in `peers[]` on its own, which in a pack of strangers is effectively random. After this
stage a friendship only starts from the menu (U1-U3):

- Rider A opens the menu, sees B, selects B: A sends `FRIEND_REQUEST` to B's pseudonym.
- B's terminal raises the incoming-request prompt (U4) for A, through the existing prompt gate
  (`mesh_core_prompt_gate`: slow, far from the turn, or not navigating).
- B consents: the key exchange below runs; both terminals store each other.

Consent is still mutual: A's selection is A's consent, B's accept is B's. The suppression list
(`suppressed[]`, `MESH_MAX_SUPPRESSED`) only existed so the automatic prompt did not nag; it is
removed. A request that is not answered lapses silently (`MESH_PROMPT_TIMEOUT_MS`, on screen), as
now. If A and B select each other at the same moment, each sees an incoming request from a rider it
has itself requested; that counts as consent on both sides and the lower pseudonym acts as the
requester.

### 1.2 Discovery: discoverable only while looking

**The problem.** Today's HELLO carries name, accent, avatar and custom art in clear every 2 s.
Rotating the id alone does not help: that combination is a stable fingerprint, so a roadside logger
tracks the look instead of the id.

**Decision (owner confirmed, OQ1):** by default a terminal broadcasts its
look in clear **only while its rider has the friend menu open**, plus a short grace after closing
(`MESH_DISCOVERABLE_GRACE_MS`, 2 min) so the other rider has time to open theirs. The rest of the
time HELLO carries no look at all, just the rotating pseudonym.

- Friending needs both riders to open the menu. They are standing next to each other agreeing to
  friend, so that costs nothing.
- Friends never need the public look: they get it encrypted (`LOOK`, below).
- An opt-in **"always discoverable"** toggle keeps the look in HELLO at all times, for riders who
  want passive features ("riders you saw this week"). Default off. It is a stored setting
  (`SETTINGS_TAG_DISCOVERABLE = 0x0B`, the next free TLV tag after Stage 2's 0x0A; same six-place
  checklist as §2.5). No ESP settings UI and no phone UI yet, so until the phone UI lands it is set
  only from the serial console or a debug build. While it is on, the pseudonym still rotates (§1.3),
  but the public look makes rotation moot for that rider; the toggle's description must say so.

**Rejected: a shared rolling key published in GitHub releases.** This is an open-source project,
so the key would be public. It stops nobody who wants to sniff, and it breaks discovery on any
terminal whose phone has not fetched the current key — out in the no-coverage places the mesh is
meant for.

### 1.3 Rotating pseudonym, resolvable by friends

The header's `sender[8]` becomes a pseudonym built like a BLE resolvable private address:

    sender = prand (4) | hash (4),   hash = AES-128(tag_key, prand | zero pad)[0..3]
    tag_key = AES-128(rider_key, "OA-TAGKEY" | zero pad)          (derived once per friend)

- Every frame, not just HELLO, is self-resolving. A receiver resolves a new pseudonym once by trying
  the `tag_key` of each of up to 16 friends: at most 16 AES blocks, using the portable
  `mesh_aes128_encrypt` the host tests already cover. The result is cached on the peer entry.
- A match is provisional until a frame from that sender decrypts under the friend's key. A 32-bit
  hash makes a false match about 16 / 2^32 per new stranger, and a false match can only produce a
  failed decrypt.
- `tag_key` is derived rather than `rider_key` used directly, so the key that drives AES-CCM is
  never also used as a raw block cipher on attacker-chosen input.
- **Rotation:** on boot and every `MESH_PSEUDONYM_ROTATE_MS` (15 min), re-rolling `session`
  with it so POS nonces stay unique. Frozen while discoverable, during a handshake and while on a
  ride. Rotating mid-ride would create a duplicate stranger marker for up to 10 s, and a ride is
  linkable by its `ride_id` anyway.
- The permanent `rider_id` never appears on air in clear again. It travels only inside the
  encrypted key exchange and stays what friend records, NVS and phone sync are keyed by.
- **Known limitation, unchanged from §5.7:** a removed friend keeps your `rider_key`, so it can
  still resolve and decrypt you until you reset your identity on the phone. Revocation is not
  in scope.

### 1.4 Key exchange: ephemeral X25519

§5.7 sends the permanent `rider_key` **in clear** inside `FRIEND_ACCEPT`. Anyone logging near a
friending event gets that key and can decrypt that rider's position for as long as the key lives.
Stage 1 closes this:

1. A → B `FRIEND_REQUEST`: B's pseudonym, A's ephemeral X25519 public key.
2. B consents → B → A `FRIEND_ACCEPT`: A's pseudonym, B's ephemeral public key, and
   `CCM_K{B.rider_id, B.rider_key}`. K = BLAKE2b(shared secret | both public keys), truncated to 16 bytes.
3. A completes the friendship on receipt (A consented at step 1) → A → B `FRIEND_KEYS`:
   `CCM_K{A.rider_id, A.rider_key}`.
4. B completes the friendship on receipt → B → A `FRIEND_ACK` stops A's retries.

Each step retries on the existing `MESH_ACCEPT_RETRY_MS`/`MESH_ACCEPT_TRIES` shape until the next
step arrives. Ephemeral keys are per handshake and discarded afterwards.

This stops passive capture, which is the realistic threat. It does not stop an active
man-in-the-middle who is in range and live at the moment of the handshake. That person is standing
next to both riders, and the menu shows exactly who is being friended. Accepted.

**Library:** Monocypher (single-file portable C, CC0/BSD-2), vendored, for X25519 and BLAKE2b.
Its EdDSA is only needed by the deferred hardening item H1. It is portable C like
`mesh_crypto.c`, so host tests run the exact
bytes the terminal runs. Record it in SPEC §14. Measure flash (C3 is at 64.9%) and the time of one
X25519 on the C3 before committing. The estimate is tens of ms, for an event that happens once per
friendship.

### 1.5 LOOK frame: look updates to friends only

A friend's look used to come from their public HELLO (`MESH_FRIEND_EVENT_UPDATED`). It now comes
from `LOOK`: our look under AES-CCM with our own `rider_key`, so only friends can read it. It is
sent on change, on the first HELLO a friend resolves, and every 30 s. About 120 bytes.

### 1.6 Relative positioning: compare like-aged fixes, then smooth

**What is wrong today.** `mesh_core_friends_in_range` compares a friend's fix, up to 10 s old, with
our *latest* fix. The error that matters is not how fast the two riders move relative to each
other. It is **how fast we both move × the difference in age between the two fixes**. At 100 km/h a
1 s age gap puts a rider who is alongside about 28 m behind on the compass. The age gap is up to
about 1.5 s: POS holds our fix for up to 1 s before sending, BLE takes up to 300 ms plus the queue,
and our own fix is up to 1 s old.

**Fix:**

- POS gains `age_ms` in clear: how old the sender's fix is when it transmits, from its own
  monotonic clock. Android publishes on the location callback, so the phone-side age is close to
  zero. iOS is to be confirmed in the iOS work's Phase C.
- Each terminal keeps its last 6 own fixes (`lat`, `lon`, receive time). That is a fixed array,
  about 6 s at 1 Hz.
- On receipt, find our own position at `now - age_ms` by linear interpolation between the two
  neighbouring fixes. Compute the **offset** (metres east/north) from us-then to them-then and store
  it on the peer. Bearing and distance on the compass are that offset. Do not add it back onto our
  current position and recompute.
- The remaining error is (their motion − our motion) × age: riders moving together cancel. This is
  the 1 Hz-is-fine intuition, and it only holds once the ages are aligned.
- **GPS jitter.** Two phones a few metres apart each wander ±3-5 m, and not in sync (different
  antennas and multipath). At 10 m apart that swings the bearing ±20° or more. Smooth the stored
  offset with an EMA (time constant about 3 s), and set a `close` flag when the distance is below
  `max(10 m, acc_self + acc_peer)`. The GUI can then draw "alongside" instead of a bearing that
  jumps around (U12).
- Render only offsets under 5 s old, down from 10 s. The buffer does not store heading and there is
  no dynamic sizing.

### 1.7 Wire format v2

`MESH_PROTO_VERSION = 2`. v1 frames are dropped by version, as §5.7 already specifies.

| Type | Name | Body |
|---|---|---|
| 0x01 | HELLO | `caps` (1) \| `flags` (1: bit0 look present, bit1 ride advert present) \| [look, only while discoverable] \| [ride advert, Stage 2] |
| 0x02 | FRIEND_REQUEST | target pseudonym (8) \| eph pub (32) |
| 0x03 | FRIEND_ACCEPT | target pseudonym (8) \| eph pub (32) \| CCM_K{rider_id 8, rider_key 16} + tag 8 |
| 0x04 | POS | `age_ms` (2, clear) \| `ttl` (1, clear, always 0 until Stage 4) \| CCM under own `rider_key`{`mesh_pos_t`} |
| 0x05 | FRIEND_KEYS | target pseudonym (8) \| CCM_K{rider_id 8, rider_key 16} + tag 8 |
| 0x06 | FRIEND_ACK | target pseudonym (8) |
| 0x07 | LOOK | CCM under own `rider_key`{look} |

`age_ms` and `ttl` sit in clear, outside the AAD, because a Stage 4 relay must edit them without
holding the key. `ttl` ships now, always 0, so Stage 4 needs no wire change. A forged `age_ms` can
only move a marker, within the 5 s cutoff.

POS is sent only while a friend is in range *who is not on our current ride*. Ride members already
get `RIDE_POS` (Stage 2). This keeps the C3 inside its airtime budget (below).

### 1.8 Touchpoints

- `mesh_proto.h/.c`: version, the new types, explicit-field encode/decode for each.
  `mesh_header_t.sender` is documented as a pseudonym.
- `mesh_crypto`: `tag_key` derivation, pseudonym build/resolve. Monocypher is vendored beside it.
- `mesh_core.c`: remove the automatic offer and `suppressed[]`. Add a request/handshake state per
  peer, a resolved-friend link per peer (keyed internally by permanent id), discoverable state,
  rotation, the own-fix history and a per-peer offset.
- `mesh.c`: NVS keys unchanged, since friends stay keyed by permanent id.
- Shared Kotlin `RiderMesh`: the 122-byte friend record is unchanged. Nothing phone-facing changes
  in Stage 1.
- `mesh_test.c` and the golden fixtures: every frame, the handshake end to end, pseudonym
  resolution (including a false match), rotation freeze rules, and the interpolation and EMA.

---

## Stage 2 — Group rides (1-hop)

### 2.1 Membership model

- **One ride slot per terminal**, not a list. Starting or joining replaces the current ride.
- **Membership = holding `ride_key`**: `RIDE_POS` decrypts, you are in. Identity plays no part.
- **Authority = the ride key plus a soft starter check (no signatures).** Only members can
  produce `RIDE_END` and `NAV_SHARE`, because both are encrypted under `ride_key`, so outsiders
  cannot forge them. Members accept either frame only when its sender is the starter's pseudonym,
  which the grant carries and which stays frozen for the ride (§1.3). That check stops mistakes and
  unmodified firmware. It does **not** stop a member running modified firmware, who can spoof the
  starter's pseudonym. **Accepted limitation:** any member of a ride can, with modified firmware,
  end it or send fake turns. For a group-ride terminal this is judged out of scope. Signatures that
  close it are recorded in full as deferred hardening item **H1**.
- **Joining needs only one member in range**, not the starter: every member advertises the ride and
  can grant.

**Freshness rules** (cheap, kept without signatures):

1. **Replay is stopped by the sequence number.** A copied `RIDE_END` or `NAV_SHARE` still decrypts,
   but only as the exact same frame. A member keeps the starter's last accepted `seq` for these
   frames and rejects any with `seq <=` that. Resends of the current nav frame carry the same `seq`,
   count as liveness only, and are not re-queued. The `seq` is in the header, and the header is CCM
   associated data, so it cannot be altered without failing the tag.
2. **Cross-ride replay fails automatically**, because every ride has a fresh `ride_key`.
3. **Unencrypted `age_ms` is bounded.** For a given `seq`, a member uses the `age_ms` from the first
   copy it receives. Values above 3 s mark the frame stale for the countdown (liveness still
   counts). Tampering can therefore shift a distance by at most a few seconds of travel, never
   change the instruction.

### 2.2 Policies

| Policy | A member in range grants when… |
|---|---|
| Open | always |
| Friends | the requester's pseudonym resolves to one of *that member's* friends (§1.3) |
| Request | that member's rider approves (U8). First grant the requester receives wins; later duplicates are dropped |

Leader-approve is removed. It was the only reason handshake frames needed flooding.

The policy is chosen per ride at ride start on the terminal (U6); it is not a stored setting. A
phone-side default comes with the phone UI.

**Shipping order (owner confirmed, OQ3):** Stage 2 ships **open** and **friends**. **Request**
follows once U8 (the approval screen) is designed. The `policy` byte reserves request's value from
the start, so adding it later changes no wire format; a terminal that does not yet implement it
ignores join requests to a request-policy ride.

Every grant is delivered through the same ephemeral X25519 exchange as friending. The ride key never
goes on air in clear, under any policy.

### 2.3 Frames

| Type | Name | Body | Cadence |
|---|---|---|---|
| (HELLO) | ride advert block | `ride_id` (8) \| `policy` (1) | In every HELLO while on a ride. No separate `RIDE_START` frame: this saves 0.5 frames/s of C3 airtime |
| 0x10 | RIDE_JOIN_REQUEST | target member pseudonym (8) \| eph pub (32) | Retried like friending |
| 0x11 | RIDE_JOIN_GRANT | target pseudonym (8) \| eph pub (32) \| CCM_K{`ride_id` 8, `ride_key` 16, starter pseudonym 8, `policy` 1, `key_epoch` 1} + tag | On admission |
| 0x12 | RIDE_POS | `age_ms` (2) \| `ttl` (1) \| CCM under `ride_key`{`mesh_pos_t`} | 1 Hz |
| 0x13 | RIDE_END | CCM under `ride_key`{`ride_id` 8, `key_epoch` 1} + tag | Once by the starter. Each member that authenticates it (sender = starter's pseudonym) forwards it once, then leaves |
| 0x14 | NAV_SHARE | Stage 3 | |
| 0x15 | RIDE_LOOK | CCM under `ride_key`{look} | On join, on change, every 30 s (as `LOOK`, §1.5) |

`ride_id` appears in clear only in the ride advert. The UI needs it to group "these riders are one
ride". It links those pseudonyms together for the length of the ride, which is accepted. The
one-shot `RIDE_END` forward is the only thing a C3 ever re-sends for someone else: it is
authenticated under `ride_key`, it happens once per ride, and it is not position relay.

**Liveness without relay.** With 1-hop only, a member who drops back out of range must not lose the
ride. Membership therefore ends only on an authenticated `RIDE_END`, on leaving (U10), or when the
held key expires; silence only makes a member dormant (below). The old 10 s fallback assumed relay.

**Dormant, not dropped (owner confirmed, OQ4).** Silence does not end membership; it pauses it.
This replaces the single silence timeout proposed earlier.

- After the dormant timeout (default 5 min) with no authentic ride frame, the terminal goes
  **dormant**: it stops the ride advert and `RIDE_POS`, and unfreezes its pseudonym. It keeps
  `ride_key`, `ride_id` and the starter's public key.
- A dormant terminal that hears any authentic frame for that `ride_id` (a ride advert, or a
  `RIDE_POS` that decrypts) **resumes silently**: no handshake and no rider action. This works
  because there is no rekeying on leave, so the key is still valid.
- The held key is discarded on an authenticated `RIDE_END`, on leaving (U10), on reboot, or after
  `MESH_RIDE_HOLD_MS` (12 h, compile-time constant), whichever comes first.
- **The dormant timeout is a stored setting, configurable from the phone app:**
  `SETTINGS_TAG_RIDE_DORMANT = 0x0C`, len 2, `uint16` seconds, default 300, clamped to 60-3600 on
  decode. Same checklist as §2.5. The phone UI for it arrives with the phone UI; until then it stays
  at its default.
- Net effect: a rider who stops for fuel and catches up an hour later is back on the ride
  automatically. A rider who missed `RIDE_END` stops advertising within 5 min.
- Side effect, accepted: a dormant rider who comes back into range rejoins automatically, even if
  they split off on purpose. They leave from the menu (U10).

**No rekeying on leave** (unchanged). A leaver keeps `ride_key` until the ride ends.

### 2.4 Roster and markers

There is no separate roster in Stage 2. With no relay, every member you can see is a 1-hop peer, so
"ride member" is a flag on the `peers[]` entry, set when that peer's `RIDE_POS` decrypts. The marker
gate in `mesh_core_friends_in_range` becomes *friend OR ride member*. Visible markers stay capped
by `MESH_MAX_PEERS` (8) and `VIEW_FRIENDS_MAX` (8, `view_state.h:20`). Raise both together, and only
after the heap measurement: `terminal_view_state_t` is copied every frame. A counted roster (the old
"24") only means something with relay, so it moves to Stage 4.

**Ride looks (owner confirmed, OQ5):** every ride member sees every other member's look,
friend or not. A stranger member has no public look (§1.2), so each member sends `RIDE_LOOK`
(0x15): its look encrypted under `ride_key`, readable by the ride only. The look is stored on the
peer entry. Until a member's first `RIDE_LOOK` arrives, that member draws as a dot. In Stage 4 the
same frame is relayed, so looks reach members across the whole mesh, not only 1-hop.

### 2.5 On-ride screen

The existing pages are reused; this is screen selection only.

- **Nav share live:** the nav page (`VIEW_ACTIVE`), as with any navigation.
- **No nav share:** the compass page with ride markers (default), or the idle screen with markers
  around the edge. Chosen by a new settings tag: `SETTINGS_TAG_RIDE_SCREEN = 0x0A`, len 1,
  `0 = compass` (default), `1 = idle ring`. It is local rendering only and never goes on the wire.

The tag rides the existing settings characteristic, so the phone *can* write it (`SettingsTlv.kt`
needs the field). No phone settings UI exists yet (`MainActivity` handles permissions only); the
toggle arrives with the phone UI. Until then it stays at its default.

Adding a tag touches six places in `settings.h`/`settings.c`: enum, `terminal_settings_t` field,
`kDefaults`, `kByteTagMax`, the decode `case`, and the encode `bytes[][2]` table. Also update
`SETTINGS_TLV_MAX_LEN`. That is a hand-written `#define` and truncates silently if missed. With
0x0A alone it becomes `1 + (2+3) + 9*(2+1)`. With all three new tags (0x0A ride screen, 0x0B
discoverable, both 1 byte; 0x0C ride dormant, 2 bytes) recompute it from `settings.h` at the time
(`+ (2+1)` per 1-byte tag, `+ (2+2)` for 0x0C). The 2-byte tag does not fit the byte-tag tables, so
0x0C needs its own encode/decode path. The Kotlin `SettingsTlv` and its golden fixture move with
it.

When the ride starts, the page switches the way starting navigation switches it today. When the
ride ends, normal behaviour resumes. The exact flow is U9.

---

## Stage 3 — Leader nav share (1-hop)

Off by default. Only the starter can turn it on, from their own navigation (U11). With no relay it
reaches riders in direct radio range of the leader. That range is still a real lag distance: the
Coded PHY reaches hundreds of metres.

### 3.1 Payload: one frame, no fragmentation

`NAV_SHARE` = `age_ms` (2) | `ttl` (1) | CCM under `ride_key`{leader `lat_e7`, `lon_e7`, nav
fields} + tag. Accepted only from the starter's pseudonym (§2.1).

- The leader's position is **inside** the ciphertext, never in clear.
- **Nav fields sent:** `icon_type`, `distance_meters`, `remaining_meters`, `street_name`
  (length-prefixed), `eta` (length-prefixed), `traffic[]` + count, `trip_progress_permille`. That is
  at most about 150 bytes, with an explicit field-by-field encoding, never a memcpy of `nav_model_t`.
- **Not sent**, because they are the leader's own telemetry and would show the leader's speed and
  battery on the follower's screen: `speed_kmh_x10`, `heading_deg`, `battery_percent`,
  `gnss_fix_valid`, `notification_age_ms` (replaced by `age_ms`), `sequence` (replaced by the header
  `seq`).
- Worst case 14 + 3 + 8 + 158 + 8 ≈ 191 bytes, against a limit of 247 (the BLE transport's 251
  minus the 4-byte AD header). It fits with about 56 bytes spare, so there is no truncation.
  Typical frames are about 60-90 bytes. `frag_index`/`frag_count` and reassembly are dropped.
  The encoder still clamps `street_name` at a UTF-8 boundary as a guard. H1 would add 64 bytes
  (worst case about 255) and bring truncation back.

### 3.2 Cadence: resend, queue on maneuver change

- The leader re-sends the current state every 2 s as well as on change. The same frame, same
  `seq`, is re-sent; a new `seq` is used only when its content changes. A follower counts the share live while
  frames arrive, and not live after 5 s without one. The cutoff is tied to the resend period.
- A follower queues an update only when the **maneuver** changes (icon or street). Between
  maneuvers, the follower's own countdown runs the distance, as it does for phone nav. Plain distance
  ticks do not fill the queue.
- A follower processes each new `seq` once, not on every resend, and rejects any `seq` at or
  below the last accepted one (§2.1, freshness rules).

### 3.3 Delivered where the leader was

This is kept from revision 1. Each queued maneuver carries the leader's position when it was
captured, aged with `age_ms`. The head of the queue is promoted once the follower's own position
comes within the trigger distance of that point (`mesh_geo`). The queue is consumed strictly in
order, and its depth is bounded. `remaining_meters`, `trip_progress_permille`, `eta` and `traffic`
are the leader's values, passed through as approximations.

### 3.4 Arbitration: gate the phone's nav at the input

There is no second pipeline and no fallback slot. While the ride's nav share is live, the nav half of
the pipeline input comes from `NAV_SHARE`, and phone-sourced nav is dropped before `countdown_accept`
(`pipeline.c:219`). Telemetry (speed, heading, battery, fix) still comes from our own phone. The
pipeline's countdown input already takes speed from the model (`pipeline.c:209`), so the follower
counts down at its own speed. This follows the two-producer split `packet_merge` already makes
between nav and telemetry.

When the share stops (ride end, or 5 s stale), the gate opens. The follower's maps app has been
navigating the whole time and posts again within seconds, so its own nav returns on the next
notification. A brief stale flash is possible and accepted. Switching source resets the countdown
anchor, exactly as a new maneuver does.

---

## Stage 4 — Multi-hop relay (deferred)

Not built now: there is no S3 in hand, and the C3 cannot afford it. Recorded so nothing has to be
rediscovered.

- **ESP-NOW boards relay; the C3 never does.** The C3 stays a full mesh member over BLE (it hears
  and is heard), but it does not forward. The C3's BLE transport shows each frame for
  `FRAME_WINDOW_MS` (300 ms) from a 4-deep outbox that drops the oldest frame. That is about 3.3
  frames/s in total, and Stages 1-3 already use most of it.
- **Controlled flooding:** `ttl` (already on the wire, default 3), `age_ms` += local dwell time
  (measured with one monotonic clock per hop), and a `(sender, seq)` dedupe cache **sized by time**,
  not by entry count. A 16-entry cache turns over in under a second at relay rates.
- **Relay covers position frames, `RIDE_LOOK` and nav share only.** HELLO, handshakes and grants
  stay 1-hop.
- **Four gaps to close for multi-hop friend positions**, because today's code assumes every friend
  is a direct peer:
  1. POS is only sent when a friend is a 1-hop peer (`mesh_core.c:333-341`). A lone rider with a
     friend 3 hops away never sends.
  2. A relayed frame's sender must resolve without its HELLO. Closed by §1.3: every frame is
     self-resolving.
  3. `friends_in_range` walks only `peers[]` (8, 1-hop). Remote positions need their own table.
  4. The look of a remote friend comes from the friend record, which is already persisted.
     Remote non-friend ride members get theirs from relayed `RIDE_LOOK` (OQ5), stored in the
     remote table.
- A counted ride roster, cap about 24, belongs here.
- Numeric defaults (TTL, cache window, roster cap) go to a GitHub tuning issue after multi-bike
  field data exists.

---

## Deferred hardening

Not planned for any stage. Recorded in full so it can be picked up without redesign.

### H1 — Ride authority signatures (`NAV_SHARE`, `RIDE_END`)

**Gap it closes.** `ride_key` is shared by every member, so it proves *membership*, not *who*. The
soft starter check (§2.1) compares the header's sender pseudonym, which a member running modified
firmware can set to anything. Such a member can therefore end the ride for everyone or send the
pack fake turn instructions. Outsiders cannot: they lack `ride_key`.

**When to pick it up.** Any of:

- Open rides with strangers become common, such as public group-ride events.
- Nav share gains safety weight, such as it being the only nav a follower has.
- Relay (Stage 4) ships. A forged frame then reaches the whole mesh, not only 1-hop.
- A reported incident, or a security review that asks for it.

**How a signature works, and why it stops forgery.**

- At ride start the starter's terminal makes an Ed25519 key pair. The **private key** never leaves
  that terminal. The **public key** goes to members.
- Signing takes the private key and the exact bytes of a message and produces 64 bytes. Verifying
  takes the public key, the message and the signature, and says yes only if that private key signed
  exactly those bytes. The public key can check but cannot sign, and the private key cannot be
  derived from it.
- So a member can copy a signature, but it is only valid on the exact bytes it was made for.
  Editing "right" to "left", or moving the signature onto another frame, fails verification. The
  only thing left is replaying the identical frame, which the `seq` rule already rejects (§2.1).

**Design.**

1. **Key pair per ride.** Made at ride start; discarded at ride end. A starter reboot loses it, so
   that ride can then no longer be ended or nav-shared; members fall back to dormancy (§2.3).
2. **Bind the ride id to the public key:** `ride_id = BLAKE2b(starter_pub)[0..7]`. Any member can
   grant, so without this a malicious member could put *their own* public key in a grant, and the
   joiner would then trust fake turns signed by them. With it, the joiner checks
   `BLAKE2b(starter_pub from grant) == ride_id` from the ride advert it chose, and rejects the grant
   on mismatch. Faking a key with the same 8-byte hash is about 2^64 work. This also retires the
   soft starter-pseudonym check.
3. **The grant carries `starter_pub` (32)** in place of the starter pseudonym (8). The grant grows by
   24 bytes; it is sent once per join.
4. **What is signed:** the whole header (magic, version, type, sender, `seq`), `ride_id`, and the
   whole body (ciphertext and tag). **Not signed:** `ttl` and `age_ms`, because relays edit them;
   the §2.1 bound on `age_ms` covers tampering with it.
5. **When it runs.** The leader signs once per new `seq` (maneuver or content change), not per
   resend. A follower verifies once per new `seq`. Resends with a known `seq` are liveness only.
6. **Size.** +64 bytes on `NAV_SHARE` and `RIDE_END`. `NAV_SHARE` worst case becomes about 255
   bytes against 247, so the encoder truncates `street_name` at a UTF-8 boundary (typical frames
   stay under 150 bytes). Alternative: carry the signature in a separate frame keyed by `seq`;
   rejected as more loss-prone on the C3's drop-oldest outbox.
7. **Wire change.** `MESH_PROTO_VERSION` bumps to 3. v2 terminals drop v3 frames by version, as
   §5.7 already specifies, so a mixed fleet cannot share a ride; all riders update together.

**Prerequisites and gates.**

- Monocypher is already vendored for X25519; H1 only adds its `crypto_eddsa_*` functions (flash
  cost to be measured).
- Benchmark Ed25519 sign and verify on the C3. The gate: verify under 50 ms, and the working
  buffers fit after the C3 heap measurement. Verification runs in the mesh task, not the GUI task,
  so a few tens of ms only delay a turn's appearance imperceptibly.
- Host tests: sign/verify round trip; tampered byte rejected; replayed `seq` rejected; grant whose
  public key does not hash to `ride_id` rejected; changing the header `type` breaks verification.

**Residual after H1.** Active interference by someone standing next to a joiner during the X25519
exchange (accepted, as for friending). And the under-2 s window where a joiner with no stored `seq`
can be shown a replayed older instruction until the leader's next resend supersedes it.

---

## C3 airtime budget (Stages 1-3)

About 3.3 frames/s total on BLE-only boards. Per terminal:

| Frame | Rate | When |
|---|---|---|
| HELLO | 0.5/s | Always (radio on) |
| POS | 1/s | A friend in range who is not on our ride |
| RIDE_POS | 1/s | On a ride |
| LOOK | ~0.03/s | Always, friends only benefit |
| RIDE_LOOK | ~0.03/s | On a ride |
| NAV_SHARE | 0.5/s | Leader, while sharing |
| Handshake frames | bursts | Rare |

A typical ride member sends about 1.5-2.5 frames/s. The worst case is a sharing leader with a
non-member friend nearby, at about 3 frames/s, which is inside the budget but close. If
measurement disagrees, the first lever is `FRAME_WINDOW_MS` (300 ms → 200 ms, two adverts per frame
instead of three).

## Explicitly not pursued

- **BLE RSSI trilateration** mixed with GPS: worse than the GPS each terminal already has.
  Revisit only if GPS itself proves unreliable (tunnels, urban canyons).
- **Release-published global mesh key**: see §1.2.
- **Leader-approve rides**: see §2.2.

---

## Decisions log

Revision 1 (Q1-Q12) is superseded where it conflicts with the list below.

- Relay deferred to Stage 4, ESP-NOW-only. The C3 is a non-relaying member.
- `ttl`/`age_ms` sit in clear. The leader's position in `NAV_SHARE` is encrypted.
- Friending is menu-only. The automatic prompt and the suppression list are removed.
- Discovery is private by default: the look is public only while the menu is open, plus an opt-in
  "always discoverable" setting, default off (OQ1).
- Stage 2 ships open + friends policies; request follows once U8 is designed (OQ3).
- Ride silence makes a member dormant (default 5 min, phone-configurable via settings tag 0x0C),
  with silent resume; the key is held up to 12 h, until reboot, `RIDE_END` or leaving (OQ4).
- Ride members see every member's look via `RIDE_LOOK` under `ride_key`, relayed in Stage 4 (OQ5).
- Hardware testing uses the second, screenless C3, driven over serial.
- The pseudonym is a BLE-RPA-style value; every frame is self-resolving. Tag and hash are built on
  AES, with no HMAC/SHA.
- Key delivery (friend keys and ride keys) uses ephemeral X25519. Nothing secret goes on air in clear.
- No signatures (OQ2). `ride_key` gates membership and, with a soft starter-pseudonym check,
  `RIDE_END` and `NAV_SHARE`. A member with modified firmware can forge both; accepted, with
  signatures recorded as deferred hardening H1.
- Policies are open / friends / request (first 1-hop approver). Leader-approve is removed.
- The policy is chosen per ride on the terminal. Phone UI and phone defaults come later.
- The ride advert is carried in HELLO; there is no separate `RIDE_START`.
- There is no roster in Stages 1-3: ride membership is a flag on the peer entry.
- Nav share is a single frame (no truncation needed), re-sent periodically, queued on maneuver
  change, and replay-protected by `seq`. It excludes the leader's own telemetry fields.
- Arbitration drops phone nav at the pipeline input while the share is live.
- Relative positioning uses age-aligned offsets with EMA smoothing and a `close` flag.
- There is no phone UI and no non-touch board support in any stage. Terminal UI is owned by the
  project owner (the U-table).

## Open questions

None. OQ1-OQ5 are answered (Decisions log).

---

## Owner TODOs (cannot be resolved by Claude)

- [x] Second terminal: a screenless C3 is available during development.
- [ ] Terminal flows and designs for U1-U12: fill in the "Flow" and "Design link" columns.
- [ ] Confirm the Monocypher licence choice (CC0 or BSD-2) for SPEC §14.

## Queued: docs and repo restructure (lands before any further development)

A standalone step, done and merged before rider-mesh or other feature work resumes. It is planned as
a pack of single-session tasks in `docs/restructure/`: start at `docs/restructure/00_CONTEXT.md`.
When it lands, this doc becomes `docs/blueprints/04_rider_mesh.md` and Stages 1-4 become phases
04.1-04.4.
