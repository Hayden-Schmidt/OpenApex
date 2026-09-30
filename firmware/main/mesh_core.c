#include "mesh_core.h"

#include <math.h>
#include <string.h>

#include "ema.h"
#include "mesh_crypto.h"

static bool id_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, MESH_RIDER_ID_LEN) == 0; }

static bool look_eq(const mesh_look_t *a, const mesh_look_t *b) {
    return a->avatar == b->avatar && a->accent_rgb == b->accent_rgb && strcmp(a->name, b->name) == 0;
}

static bool fresh(uint32_t now_ms, uint32_t at_ms, uint32_t max_age_ms) { return (now_ms - at_ms) <= max_age_ms; }

void mesh_core_init(mesh_core_t *c, uint32_t session_seed) {
    memset(c, 0, sizeof(*c));
    c->enabled = true;
    c->session = session_seed;
    c->rng = session_seed ^ 0xA5A5A5A5u;
    c->always_discoverable = MESH_ALWAYS_DISCOVERABLE != 0;
    // The pseudonym's random half is drawn once per boot (04.1.3); a distinct mix from `session` so the
    // two are not the same number on air.
    const uint32_t r = session_seed * 2654435761u + 0x9E3779B9u;
    memcpy(c->prand, &r, sizeof(c->prand));
}

void mesh_core_set_enabled(mesh_core_t *c, bool enabled) {
    c->enabled = enabled;
    if (!enabled) c->prompt = MESH_PROMPT_NONE;
}

void mesh_core_set_identity(mesh_core_t *c, const mesh_identity_t *id) {
    const bool renamed = c->has_identity && id != NULL && strcmp(c->self.name, id->name) != 0;
    c->has_identity = id != NULL;
    if (id == NULL) return;
    c->self = *id;
    mesh_tag_key(c->self.key, c->self_tag_key);
    mesh_pseudonym_make(c->self_tag_key, c->prand, c->pseud);
    if (renamed) c->look_push_left = MESH_LOOK_PUSH_TRIES; // the name is part of the look
}

void mesh_core_set_look(mesh_core_t *c, const mesh_look_t *look) {
    if (!look_eq(&c->look, look)) c->look_push_left = MESH_LOOK_PUSH_TRIES;
    c->look = *look;
}

void mesh_core_set_own_pos(mesh_core_t *c, const mesh_pos_t *pos, uint32_t now_ms) {
    c->own_pos = *pos;
    c->own_pos_ms = now_ms;
    c->has_own_pos = true;
    if (c->fix_count == MESH_OWN_FIXES) {
        memmove(&c->fixes[0], &c->fixes[1], (MESH_OWN_FIXES - 1u) * sizeof(c->fixes[0]));
        c->fix_count--;
    }
    c->fixes[c->fix_count++] = (mesh_fix_t){pos->lat_e7, pos->lon_e7, now_ms};
}

// -- discovery -----------------------------------------------------------------------------------

void mesh_core_set_menu_open(mesh_core_t *c, bool open, uint32_t now_ms) {
    if (open) {
        c->menu_open = true;
        c->grace = false;
    } else if (c->menu_open) {
        c->menu_open = false;
        c->grace = true;
        c->grace_until_ms = now_ms + MESH_DISCOVERABLE_GRACE_MS;
    }
}

void mesh_core_set_always_discoverable(mesh_core_t *c, bool on) { c->always_discoverable = on; }

static bool discoverable(const mesh_core_t *c, uint32_t now_ms) {
    return c->always_discoverable || c->menu_open || (c->grace && (int32_t)(c->grace_until_ms - now_ms) > 0);
}

// -- friend list ---------------------------------------------------------------------------------

const mesh_friend_t *mesh_core_friend_find(const mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]) {
    for (size_t i = 0; i < c->friend_count; i++) {
        if (id_eq(c->friends[i].id, id)) return &c->friends[i];
    }
    return NULL;
}

bool mesh_core_friend_upsert(mesh_core_t *c, const mesh_friend_t *f) {
    mesh_friend_t *slot = (mesh_friend_t *)mesh_core_friend_find(c, f->id);
    if (slot == NULL) {
        if (c->friend_count >= MESH_MAX_FRIENDS) return false;
        slot = &c->friends[c->friend_count++];
    }
    *slot = *f;
    mesh_tag_key(slot->key, c->tag_keys[slot - c->friends]);
    c->friends_generation++;
    return true;
}

bool mesh_core_friend_remove(mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]) {
    for (size_t i = 0; i < c->friend_count; i++) {
        if (!id_eq(c->friends[i].id, id)) continue;
        c->friend_count--;
        c->friends[i] = c->friends[c->friend_count];
        memcpy(c->tag_keys[i], c->tag_keys[c->friend_count], MESH_KEY_LEN);
        c->friends_generation++;
        return true;
    }
    return false;
}

void mesh_core_friend_clear(mesh_core_t *c) {
    c->friend_count = 0;
    c->friends_generation++;
}

static void push_event(mesh_core_t *c, uint8_t kind, const mesh_friend_t *f) {
    // Full: drop the oldest. The phone re-reads the whole list on every connect anyway.
    if (c->event_count == MESH_FRIEND_EVENTS) {
        c->event_head = (c->event_head + 1) % MESH_FRIEND_EVENTS;
        c->event_count--;
    }
    mesh_friend_event_t *e = &c->events[(c->event_head + c->event_count++) % MESH_FRIEND_EVENTS];
    e->kind = kind;
    e->record = *f;
}

bool mesh_core_take_friend_event(mesh_core_t *c, mesh_friend_event_t *out) {
    if (c->event_count == 0) return false;
    *out = c->events[c->event_head];
    c->event_head = (c->event_head + 1) % MESH_FRIEND_EVENTS;
    c->event_count--;
    return true;
}

// -- peers ---------------------------------------------------------------------------------------

static mesh_peer_t *peer_find(mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]) {
    for (size_t i = 0; i < MESH_MAX_PEERS; i++) {
        if (c->peers[i].used && id_eq(c->peers[i].id, id)) return &c->peers[i];
    }
    return NULL;
}

// Finds or makes the entry for `id`; when the table is full the longest-silent peer goes.
static mesh_peer_t *peer_touch(mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN], uint32_t now_ms) {
    mesh_peer_t *p = peer_find(c, id);
    if (p == NULL) {
        p = &c->peers[0];
        for (size_t i = 0; i < MESH_MAX_PEERS; i++) {
            if (!c->peers[i].used) {
                p = &c->peers[i];
                break;
            }
            if ((now_ms - c->peers[i].heard_ms) > (now_ms - p->heard_ms)) p = &c->peers[i];
        }
        memset(p, 0, sizeof(*p));
        p->used = true;
        p->friend_idx = MESH_NO_FRIEND;
        memcpy(p->id, id, MESH_RIDER_ID_LEN);
    }
    p->heard_ms = now_ms;
    return p;
}

// Which friend owns this pseudonym: at most MESH_MAX_FRIENDS AES blocks (04.1.3).
static uint8_t resolve(const mesh_core_t *c, const uint8_t pseudonym[MESH_RIDER_ID_LEN]) {
    for (size_t i = 0; i < c->friend_count; i++) {
        if (mesh_pseudonym_match(c->tag_keys[i], pseudonym)) return (uint8_t)i;
    }
    return MESH_NO_FRIEND;
}

// The cached friend link, without refreshing it (read-only callers).
static uint8_t friend_of(const mesh_core_t *c, const mesh_peer_t *p) {
    return p->resolved && p->resolved_gen == c->friends_generation ? p->friend_idx : resolve(c, p->id);
}

// Refreshes the cached link when the friend list has changed, and owes a look push the first time a
// friend is seen in range (both sides do this, so nobody has to ask, 04.1.5).
static uint8_t peer_friend(mesh_core_t *c, mesh_peer_t *p) {
    if (!p->resolved || p->resolved_gen != c->friends_generation) {
        p->friend_idx = resolve(c, p->id);
        p->resolved_gen = c->friends_generation;
        p->resolved = true;
        if (p->friend_idx == MESH_NO_FRIEND) p->announced = false;
    }
    if (p->friend_idx != MESH_NO_FRIEND && !p->announced) {
        p->announced = true;
        c->look_push_left = MESH_LOOK_PUSH_TRIES;
    }
    return p->friend_idx;
}

// -- sending -------------------------------------------------------------------------------------

static mesh_header_t next_header(mesh_core_t *c, uint8_t type) {
    mesh_header_t h;
    h.type = type;
    memcpy(h.sender, c->pseud, MESH_RIDER_ID_LEN);
    h.seq = ++c->seq;
    // The nonce is sender | session | seq: a new session before seq repeats keeps it unique.
    if (c->seq == 0) c->session = c->session * 1103515245u + 12345u;
    return h;
}

static void own_look(const mesh_core_t *c, mesh_look_t *out) {
    *out = c->look;
    memcpy(out->name, c->self.name, sizeof(out->name));
}

static void send_target(mesh_core_t *c, uint8_t type, const uint8_t target[MESH_RIDER_ID_LEN], mesh_send_fn send,
                        void *ctx) {
    uint8_t buf[MESH_FRAME_MAX];
    const mesh_header_t h = next_header(c, type);
    const size_t n = mesh_encode_target(&h, target, buf, sizeof(buf));
    if (n) send(buf, n, ctx);
}

// FRIEND_ACCEPT and FRIEND_KEYS carry the same body: our permanent id and key, to one target.
static void send_keys(mesh_core_t *c, uint8_t type, const uint8_t target[MESH_RIDER_ID_LEN], mesh_send_fn send,
                      void *ctx) {
    uint8_t buf[MESH_FRAME_MAX];
    const mesh_header_t h = next_header(c, type);
    const size_t n = mesh_encode_keys(&h, target, c->self.id, c->self.key, buf, sizeof(buf));
    if (n) send(buf, n, ctx);
}

static uint32_t jitter(mesh_core_t *c) {
    c->rng = c->rng * 1664525u + 1013904223u;
    return (c->rng >> 16) % MESH_RETRY_JITTER_MS;
}

static bool due(uint32_t now_ms, uint32_t at_ms) { return (int32_t)(now_ms - at_ms) >= 0; }

// The rider acted (menu pick, hold ring): the caller sends the first frame now, retries follow.
static void set_hs(mesh_core_t *c, mesh_peer_t *p, mesh_hs_t hs, uint32_t base_retry_ms, uint32_t now_ms) {
    p->hs = hs;
    p->tries = 1;
    p->next_try_ms = now_ms + base_retry_ms + jitter(c);
}

// A frame from the peer triggered this step: the first frame goes out from the tick, after the
// peer's own advert window (MESH_REPLY_DELAY_MS).
static void defer_hs(mesh_peer_t *p, mesh_hs_t hs, uint32_t now_ms) {
    p->hs = hs;
    p->tries = 0;
    p->next_try_ms = now_ms + MESH_REPLY_DELAY_MS;
}

static void defer_ack(mesh_peer_t *p, uint32_t now_ms) {
    p->ack_due = true;
    p->ack_at_ms = now_ms + MESH_REPLY_DELAY_MS;
}

static void close_prompt(mesh_core_t *c) {
    mesh_peer_t *p = peer_find(c, c->prompt_id);
    if (p != NULL && (p->hs == MESH_HS_REQ_SENT || p->hs == MESH_HS_REQ_RECV)) p->hs = MESH_HS_NONE;
    c->prompt = MESH_PROMPT_NONE;
}

// A handshake step ended without a friendship: drop the waiting page if it was for this peer.
static void lapse(mesh_core_t *c, mesh_peer_t *p) {
    p->hs = MESH_HS_NONE;
    if (c->prompt == MESH_PROMPT_WAITING && id_eq(c->prompt_id, p->id)) c->prompt = MESH_PROMPT_NONE;
}

// The other rider's identity and key arrived and we had consented: store them. Idempotent, so a
// repeated frame or both sides completing at once is harmless. False when the list is full.
static bool complete(mesh_core_t *c, mesh_peer_t *p, const uint8_t rider_id[MESH_RIDER_ID_LEN],
                     const uint8_t key[MESH_KEY_LEN]) {
    mesh_friend_t *have = (mesh_friend_t *)mesh_core_friend_find(c, rider_id);
    if (have != NULL) {
        if (memcmp(have->key, key, MESH_KEY_LEN) != 0) {
            memcpy(have->key, key, MESH_KEY_LEN);
            mesh_tag_key(have->key, c->tag_keys[have - c->friends]);
            c->friends_generation++;
            push_event(c, MESH_FRIEND_EVENT_UPDATED, have);
        }
    } else {
        mesh_friend_t f;
        memset(&f, 0, sizeof(f));
        memcpy(f.id, rider_id, MESH_RIDER_ID_LEN);
        memcpy(f.key, key, MESH_KEY_LEN);
        if (p->has_look) f.look = p->look;
        if (!mesh_core_friend_upsert(c, &f)) return false;
        push_event(c, MESH_FRIEND_EVENT_ADDED, &f);
    }
    if (c->prompt != MESH_PROMPT_NONE && id_eq(c->prompt_id, p->id)) {
        c->prompt = MESH_PROMPT_DONE;
        c->prompt_done_at_ms = c->last_tick_ms;
    }
    return true;
}

// -- receive -------------------------------------------------------------------------------------

// Own position at `t_ms`, interpolated between the two fixes that bracket it; clamped to the
// oldest or newest fix outside the history.
static bool own_at(const mesh_core_t *c, uint32_t t_ms, double *lat, double *lon) {
    const size_t n = c->fix_count;
    if (n == 0) return false;
    const mesh_fix_t *f = c->fixes;
    if ((int32_t)(t_ms - f[0].t_ms) <= 0) {
        *lat = f[0].lat_e7;
        *lon = f[0].lon_e7;
        return true;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if ((int32_t)(t_ms - f[i + 1].t_ms) > 0) continue;
        const uint32_t span = f[i + 1].t_ms - f[i].t_ms;
        const double frac = span ? (double)(t_ms - f[i].t_ms) / (double)span : 1.0;
        *lat = f[i].lat_e7 + ((double)f[i + 1].lat_e7 - f[i].lat_e7) * frac;
        *lon = f[i].lon_e7 + ((double)f[i + 1].lon_e7 - f[i].lon_e7) * frac;
        return true;
    }
    *lat = f[n - 1].lat_e7;
    *lon = f[n - 1].lon_e7;
    return true;
}

// Compare like-aged fixes (04.1.6): our position when their fix was taken, not our latest. The
// smoothed vector is the offset the compass draws.
static void update_offset(mesh_core_t *c, mesh_peer_t *p, const mesh_pos_t *pos, uint16_t age_ms, uint32_t now_ms) {
    if (c->fix_count == 0 || !fresh(now_ms, c->fixes[c->fix_count - 1].t_ms, MESH_OWN_POS_MAX_AGE_MS)) return;
    if (age_ms > MESH_OWN_POS_MAX_AGE_MS) return;
    double lat, lon;
    if (!own_at(c, now_ms - age_ms, &lat, &lon)) return;
    float east, north;
    mesh_offset((int32_t)lat, (int32_t)lon, pos->lat_e7, pos->lon_e7, &east, &north);
    if (!p->has_off || !fresh(now_ms, p->off_ms, MESH_OFFSET_MAX_AGE_MS)) {
        p->off_east_m = east;
        p->off_north_m = north;
    } else {
        float dt_s = (float)(now_ms - p->off_ms) / 1000.0f;
        if (dt_s > 5.0f) dt_s = 5.0f;
        const float alpha = ema_alpha(dt_s, MESH_OFFSET_TAU_S);
        p->off_east_m = ema_blend(p->off_east_m, east, alpha);
        p->off_north_m = ema_blend(p->off_north_m, north, alpha);
    }
    p->has_off = true;
    p->off_ms = now_ms;
}

static void on_request(mesh_core_t *c, mesh_peer_t *p, uint32_t now_ms) {
    if (peer_friend(c, p) != MESH_NO_FRIEND) {
        // Already friends here but not there (their list was lost): our earlier consent stands, so
        // answer and let their side complete.
        if (p->hs == MESH_HS_NONE) defer_hs(p, MESH_HS_ACCEPT_SENT, now_ms);
        return;
    }
    switch (p->hs) {
    case MESH_HS_REQ_SENT:
        // Both riders picked each other at the same moment: each request is the other's consent.
        defer_hs(p, MESH_HS_ACCEPT_SENT, now_ms);
        break;
    case MESH_HS_NONE:
        if (c->prompt != MESH_PROMPT_NONE || c->friend_count >= MESH_MAX_FRIENDS) return; // they retry
        p->hs = MESH_HS_REQ_RECV;
        c->prompt = MESH_PROMPT_OFFER;
        memcpy(c->prompt_id, p->id, MESH_RIDER_ID_LEN);
        c->prompt_look = p->look;
        c->prompt_shown_ms = 0;
        break;
    default:
        break; // already answering this rider
    }
}

void mesh_core_rx(mesh_core_t *c, const uint8_t *frame, size_t len, uint32_t now_ms, mesh_send_fn send,
                  void *ctx) {
    // Nothing is sent from here: every reply waits for the tick (MESH_REPLY_DELAY_MS). The
    // parameters stay so a later frame type can answer inline if it ever needs to.
    (void)send;
    (void)ctx;
    mesh_header_t h;
    if (!c->enabled || !c->has_identity || !mesh_decode_header(frame, len, &h)) return;
    if (id_eq(h.sender, c->pseud)) return; // an echo of our own frame

    switch (h.type) {
    case MESH_TYPE_HELLO: {
        mesh_look_t look;
        bool has_look;
        uint8_t caps;
        if (!mesh_decode_hello(frame, len, &look, &has_look, &caps)) return;
        mesh_peer_t *p = peer_touch(c, h.sender, now_ms);
        if (peer_friend(c, p) == MESH_NO_FRIEND) {
            // A stranger's public look, for the menu. A friend's look comes only from LOOK.
            p->has_look = has_look;
            if (has_look) p->look = look;
        }
        break;
    }
    case MESH_TYPE_FRIEND_REQUEST: {
        uint8_t target[MESH_RIDER_ID_LEN];
        if (!mesh_decode_target(frame, len, target) || !id_eq(target, c->pseud)) return;
        mesh_peer_t *p = peer_find(c, h.sender);
        if (p == NULL) return; // not heard a HELLO yet: stay silent, their retries will land
        p->heard_ms = now_ms;
        on_request(c, p, now_ms);
        break;
    }
    case MESH_TYPE_FRIEND_ACCEPT: {
        uint8_t target[MESH_RIDER_ID_LEN], rider_id[MESH_RIDER_ID_LEN], key[MESH_KEY_LEN];
        if (!mesh_decode_keys(frame, len, target, rider_id, key) || !id_eq(target, c->pseud)) return;
        mesh_peer_t *p = peer_find(c, h.sender);
        // Only a rider we asked (or who is answering us) may hand us a key.
        if (p == NULL || (p->hs != MESH_HS_REQ_SENT && p->hs != MESH_HS_ACCEPT_SENT && p->hs != MESH_HS_KEYS_SENT)) {
            return;
        }
        p->heard_ms = now_ms;
        if (!complete(c, p, rider_id, key)) {
            lapse(c, p); // list full
            return;
        }
        // Their repeated ACCEPT means our KEYS has not landed: answer again, after their window.
        defer_hs(p, MESH_HS_KEYS_SENT, now_ms);
        break;
    }
    case MESH_TYPE_FRIEND_KEYS: {
        uint8_t target[MESH_RIDER_ID_LEN], rider_id[MESH_RIDER_ID_LEN], key[MESH_KEY_LEN];
        if (!mesh_decode_keys(frame, len, target, rider_id, key) || !id_eq(target, c->pseud)) return;
        mesh_peer_t *p = peer_find(c, h.sender);
        if (p == NULL) return;
        if (p->hs != MESH_HS_ACCEPT_SENT && p->hs != MESH_HS_KEYS_SENT) {
            // Already done here and our ACK was lost: they keep resending KEYS until it lands. Only
            // a friend we already hold, with the same key, gets an answer; nothing is changed.
            const mesh_friend_t *f = mesh_core_friend_find(c, rider_id);
            if (p->hs == MESH_HS_NONE && f != NULL && memcmp(f->key, key, MESH_KEY_LEN) == 0) {
                p->heard_ms = now_ms;
                defer_ack(p, now_ms);
            }
            return;
        }
        p->heard_ms = now_ms;
        if (!complete(c, p, rider_id, key)) {
            lapse(c, p);
            return;
        }
        defer_ack(p, now_ms);
        if (p->hs == MESH_HS_ACCEPT_SENT) p->hs = MESH_HS_NONE; // KEYS_SENT keeps retrying until their ACK
        break;
    }
    case MESH_TYPE_FRIEND_ACK: {
        uint8_t target[MESH_RIDER_ID_LEN];
        if (!mesh_decode_target(frame, len, target) || !id_eq(target, c->pseud)) return;
        mesh_peer_t *p = peer_find(c, h.sender);
        if (p != NULL && p->hs == MESH_HS_KEYS_SENT) p->hs = MESH_HS_NONE;
        break;
    }
    case MESH_TYPE_POS: {
        const uint8_t idx = resolve(c, h.sender);
        mesh_pos_t pos;
        uint16_t age_ms;
        uint8_t ttl;
        if (idx == MESH_NO_FRIEND || !mesh_decode_pos(frame, len, c->friends[idx].key, &pos, &age_ms, &ttl)) return;
        mesh_peer_t *p = peer_touch(c, h.sender, now_ms);
        peer_friend(c, p);
        if (!p->has_look) {
            p->look = c->friends[idx].look;
            p->has_look = true;
        }
        update_offset(c, p, &pos, age_ms, now_ms);
        break;
    }
    case MESH_TYPE_LOOK: {
        const uint8_t idx = resolve(c, h.sender);
        mesh_look_t look;
        if (idx == MESH_NO_FRIEND || !mesh_decode_look(frame, len, c->friends[idx].key, &look)) return;
        mesh_peer_t *p = peer_touch(c, h.sender, now_ms);
        peer_friend(c, p);
        p->look = look;
        p->has_look = true;
        if (!look_eq(&c->friends[idx].look, &look)) {
            c->friends[idx].look = look;
            c->friends_generation++;
            push_event(c, MESH_FRIEND_EVENT_UPDATED, &c->friends[idx]);
        }
        break;
    }
    default:
        break; // nav share (Phase 2) and anything newer
    }
}

// -- menu actions --------------------------------------------------------------------------------

size_t mesh_core_nearby(const mesh_core_t *c, uint32_t now_ms, const mesh_peer_t **out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i < MESH_MAX_PEERS && n < cap; i++) {
        const mesh_peer_t *p = &c->peers[i];
        if (!p->used || !p->has_look || !fresh(now_ms, p->heard_ms, MESH_PEER_TIMEOUT_MS)) continue;
        if (friend_of(c, p) != MESH_NO_FRIEND) continue;
        out[n++] = p;
    }
    return n;
}

bool mesh_core_request(mesh_core_t *c, const uint8_t pseudonym[MESH_RIDER_ID_LEN], uint32_t now_ms,
                       mesh_send_fn send, void *ctx) {
    if (!c->enabled || !c->has_identity || c->prompt != MESH_PROMPT_NONE || c->friend_count >= MESH_MAX_FRIENDS) {
        return false;
    }
    mesh_peer_t *p = peer_find(c, pseudonym);
    if (p == NULL || !fresh(now_ms, p->heard_ms, MESH_PEER_TIMEOUT_MS) || p->hs != MESH_HS_NONE ||
        peer_friend(c, p) != MESH_NO_FRIEND) {
        return false;
    }
    set_hs(c, p, MESH_HS_REQ_SENT, MESH_REQUEST_RETRY_MS, now_ms);
    c->prompt = MESH_PROMPT_WAITING;
    memcpy(c->prompt_id, p->id, MESH_RIDER_ID_LEN);
    c->prompt_look = p->look;
    c->prompt_shown_ms = 0;
    send_target(c, MESH_TYPE_FRIEND_REQUEST, p->id, send, ctx);
    return true;
}

void mesh_core_accept(mesh_core_t *c, const uint8_t pseudonym[MESH_RIDER_ID_LEN], uint32_t now_ms,
                      mesh_send_fn send, void *ctx) {
    if (c->prompt != MESH_PROMPT_OFFER || !id_eq(pseudonym, c->prompt_id)) return;
    mesh_peer_t *p = peer_find(c, pseudonym);
    if (p == NULL || p->hs != MESH_HS_REQ_RECV) return;
    set_hs(c, p, MESH_HS_ACCEPT_SENT, MESH_ACCEPT_RETRY_MS, now_ms);
    c->prompt = MESH_PROMPT_WAITING;
    send_keys(c, MESH_TYPE_FRIEND_ACCEPT, p->id, send, ctx);
}

// -- tick ----------------------------------------------------------------------------------------

bool mesh_core_prompt_gate(bool navigating, uint32_t turn_distance_m, uint16_t speed_kmh_x10) {
    if (!navigating) return true;
    if (turn_distance_m > MESH_PROMPT_MIN_TURN_M) return true;
    return speed_kmh_x10 != 0xFFFF && speed_kmh_x10 < MESH_PROMPT_MAX_SPEED_KMH_X10;
}

static void run_prompt(mesh_core_t *c, uint32_t now_ms, uint32_t dt_ms, bool allowed) {
    c->prompt_visible = allowed;
    if (c->prompt == MESH_PROMPT_DONE) {
        if (now_ms - c->prompt_done_at_ms >= MESH_PROMPT_DONE_MS) c->prompt = MESH_PROMPT_NONE;
        return;
    }
    if (c->prompt == MESH_PROMPT_NONE) return;
    mesh_peer_t *p = peer_find(c, c->prompt_id);
    if (p == NULL || !fresh(now_ms, p->heard_ms, MESH_PEER_TIMEOUT_MS)) {
        close_prompt(c); // rode apart
        return;
    }
    c->prompt_look = p->look;
    if (allowed) c->prompt_shown_ms += dt_ms;
    if (c->prompt_shown_ms >= MESH_PROMPT_TIMEOUT_MS) close_prompt(c); // unanswered: lapses silently
}

void mesh_core_tick(mesh_core_t *c, uint32_t now_ms, bool prompt_allowed, mesh_send_fn send, void *ctx) {
    const uint32_t dt = c->ticked ? now_ms - c->last_tick_ms : 0;
    c->last_tick_ms = now_ms;
    c->ticked = true;
    if (!c->enabled || !c->has_identity) return;

    bool friend_near = false;
    for (size_t i = 0; i < MESH_MAX_PEERS; i++) {
        mesh_peer_t *p = &c->peers[i];
        if (!p->used) continue;
        if (!fresh(now_ms, p->heard_ms, MESH_PEER_TIMEOUT_MS) &&
            !(c->prompt != MESH_PROMPT_NONE && id_eq(p->id, c->prompt_id))) {
            p->used = false;
            continue;
        }
        if (peer_friend(c, p) != MESH_NO_FRIEND && fresh(now_ms, p->heard_ms, MESH_PEER_TIMEOUT_MS)) friend_near = true;
    }
    run_prompt(c, now_ms, dt, prompt_allowed);

    uint8_t buf[MESH_FRAME_MAX];
    mesh_look_t look;
    if (!c->hello_started || due(now_ms, c->next_hello_ms)) {
        c->hello_started = true;
        c->next_hello_ms = now_ms + MESH_HELLO_PERIOD_MS - MESH_RETRY_JITTER_MS / 2 + jitter(c);
        own_look(c, &look);
        const mesh_header_t h = next_header(c, MESH_TYPE_HELLO);
        const size_t n = mesh_encode_hello(&h, discoverable(c, now_ms) ? &look : NULL, 0, buf, sizeof(buf));
        if (n) send(buf, n, ctx);
    }

    // Handshake frames: deferred replies and retries (frames are adverts and can be lost).
    for (size_t i = 0; i < MESH_MAX_PEERS; i++) {
        mesh_peer_t *p = &c->peers[i];
        if (!p->used) continue;
        if (p->ack_due && due(now_ms, p->ack_at_ms)) {
            p->ack_due = false;
            send_target(c, MESH_TYPE_FRIEND_ACK, p->id, send, ctx);
        }
        switch (p->hs) {
        case MESH_HS_REQ_SENT:
            if (c->prompt != MESH_PROMPT_WAITING || !id_eq(c->prompt_id, p->id)) {
                p->hs = MESH_HS_NONE; // the page closed: the request lapsed
            } else if (due(now_ms, p->next_try_ms)) {
                p->next_try_ms = now_ms + MESH_REQUEST_RETRY_MS + jitter(c);
                send_target(c, MESH_TYPE_FRIEND_REQUEST, p->id, send, ctx);
            }
            break;
        case MESH_HS_ACCEPT_SENT:
        case MESH_HS_KEYS_SENT:
            if (!due(now_ms, p->next_try_ms)) break;
            if (p->tries >= MESH_ACCEPT_TRIES) {
                lapse(c, p);
                break;
            }
            p->tries++;
            p->next_try_ms = now_ms + MESH_ACCEPT_RETRY_MS + jitter(c);
            send_keys(c, p->hs == MESH_HS_ACCEPT_SENT ? MESH_TYPE_FRIEND_ACCEPT : MESH_TYPE_FRIEND_KEYS, p->id, send,
                      ctx);
            break;
        default:
            break;
        }
    }

    // Our look to friends in range, once per appearance or change (04.1.5).
    if (c->look_push_left > 0 && friend_near && (c->last_look_ms == 0 || now_ms - c->last_look_ms >= MESH_LOOK_PUSH_MS)) {
        c->last_look_ms = now_ms ? now_ms : 1;
        c->look_push_left--;
        own_look(c, &look);
        const mesh_header_t h = next_header(c, MESH_TYPE_LOOK);
        const size_t n = mesh_encode_look(&h, c->self.key, c->session, &look, buf, sizeof(buf));
        if (n) send(buf, n, ctx);
    }

    // Position only while a friend is actually listening, and only a fix we'd trust ourselves.
    if (!c->has_own_pos || !fresh(now_ms, c->own_pos_ms, MESH_OWN_POS_MAX_AGE_MS)) return;
    if (!due(now_ms, c->next_pos_ms) || !friend_near) return;
    c->next_pos_ms = now_ms + MESH_POS_PERIOD_MS - MESH_RETRY_JITTER_MS / 2 + jitter(c);
    const mesh_header_t h = next_header(c, MESH_TYPE_POS);
    const size_t n = mesh_encode_pos(&h, c->self.key, c->session, &c->own_pos, (uint16_t)(now_ms - c->own_pos_ms), 0,
                                     buf, sizeof(buf));
    if (n) send(buf, n, ctx);
}

mesh_prompt_state_t mesh_core_prompt(const mesh_core_t *c, uint8_t id[MESH_RIDER_ID_LEN], mesh_look_t *look) {
    if (c->prompt == MESH_PROMPT_NONE || !c->prompt_visible) return MESH_PROMPT_NONE;
    if (id) memcpy(id, c->prompt_id, MESH_RIDER_ID_LEN);
    if (look) *look = c->prompt_look;
    return c->prompt;
}

size_t mesh_core_friends_in_range(const mesh_core_t *c, uint32_t now_ms, mesh_friend_fix_t *out, size_t cap) {
    if (!c->enabled) return 0;
    size_t n = 0;
    for (size_t i = 0; i < MESH_MAX_PEERS && n < cap; i++) {
        const mesh_peer_t *p = &c->peers[i];
        if (!p->used || !p->has_off || !fresh(now_ms, p->off_ms, MESH_OFFSET_MAX_AGE_MS)) continue;
        const uint8_t idx = friend_of(c, p);
        if (idx == MESH_NO_FRIEND) continue;
        out[n].f = &c->friends[idx];
        double b = atan2((double)p->off_east_m, (double)p->off_north_m) * 180.0 / 3.14159265358979323846;
        if (b < 0) b += 360.0;
        out[n].bearing_deg = (float)b;
        out[n].dist_m = hypotf(p->off_east_m, p->off_north_m);
        n++;
    }
    return n;
}
