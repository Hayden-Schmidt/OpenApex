#include "mesh_core.h"

#include <string.h>

static bool id_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, MESH_RIDER_ID_LEN) == 0; }

static bool look_eq(const mesh_look_t *a, const mesh_look_t *b) {
    return a->avatar == b->avatar && a->accent_rgb == b->accent_rgb && strcmp(a->name, b->name) == 0 &&
           a->has_custom == b->has_custom && (!a->has_custom || memcmp(a->art, b->art, MESH_ART_BYTES) == 0);
}

void mesh_core_init(mesh_core_t *c, uint32_t session_seed) {
    memset(c, 0, sizeof(*c));
    c->enabled = true;
    c->session = session_seed;
}

void mesh_core_set_enabled(mesh_core_t *c, bool enabled) {
    c->enabled = enabled;
    if (!enabled) c->prompt = MESH_PROMPT_NONE;
}

void mesh_core_set_identity(mesh_core_t *c, const mesh_identity_t *id) {
    c->has_identity = id != NULL;
    if (id != NULL) c->self = *id;
}

void mesh_core_set_look(mesh_core_t *c, const mesh_look_t *look) { c->look = *look; }

void mesh_core_set_own_pos(mesh_core_t *c, const mesh_pos_t *pos, uint32_t now_ms) {
    c->own_pos = *pos;
    c->own_pos_ms = now_ms;
    c->has_own_pos = true;
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
    c->friends_generation++;
    return true;
}

bool mesh_core_friend_remove(mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]) {
    for (size_t i = 0; i < c->friend_count; i++) {
        if (!id_eq(c->friends[i].id, id)) continue;
        c->friends[i] = c->friends[--c->friend_count];
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
        memcpy(p->id, id, MESH_RIDER_ID_LEN);
    }
    p->heard_ms = now_ms;
    return p;
}

static bool suppressed(const mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]) {
    for (size_t i = 0; i < c->suppressed_count; i++) {
        if (id_eq(c->suppressed[i], id)) return true;
    }
    return false;
}

static void suppress(mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN]) {
    if (suppressed(c, id)) return;
    memcpy(c->suppressed[c->suppressed_next], id, MESH_RIDER_ID_LEN);
    c->suppressed_next = (c->suppressed_next + 1) % MESH_MAX_SUPPRESSED;
    if (c->suppressed_count < MESH_MAX_SUPPRESSED) c->suppressed_count++;
}

// -- sending -------------------------------------------------------------------------------------

static mesh_header_t next_header(mesh_core_t *c, uint8_t type) {
    mesh_header_t h;
    h.type = type;
    memcpy(h.sender, c->self.id, MESH_RIDER_ID_LEN);
    h.seq = ++c->seq;
    // The POS nonce is sender | session | seq: a new session before seq repeats keeps it unique.
    if (c->seq == 0) c->session = c->session * 1103515245u + 12345u;
    return h;
}

static void send_accept(mesh_core_t *c, const uint8_t target[MESH_RIDER_ID_LEN], mesh_send_fn send, void *ctx) {
    uint8_t buf[MESH_FRAME_MAX];
    const mesh_header_t h = next_header(c, MESH_TYPE_FRIEND_ACCEPT);
    const size_t n = mesh_encode_accept(&h, target, c->self.key, buf, sizeof(buf));
    if (n) send(buf, n, ctx);
}

static void send_ack(mesh_core_t *c, const uint8_t target[MESH_RIDER_ID_LEN], mesh_send_fn send, void *ctx) {
    uint8_t buf[MESH_FRAME_MAX];
    const mesh_header_t h = next_header(c, MESH_TYPE_FRIEND_ACK);
    const size_t n = mesh_encode_ack(&h, target, buf, sizeof(buf));
    if (n) send(buf, n, ctx);
}

static void complete(mesh_core_t *c, mesh_peer_t *p) {
    mesh_friend_t f;
    memcpy(f.id, p->id, MESH_RIDER_ID_LEN);
    memcpy(f.key, p->remote_key, MESH_KEY_LEN);
    f.look = p->look;
    if (!mesh_core_friend_upsert(c, &f)) return; // list full: the prompt lapses unanswered
    push_event(c, MESH_FRIEND_EVENT_ADDED, &f);
    p->local_accepted = p->remote_accepted = false;
    if (c->prompt != MESH_PROMPT_NONE && id_eq(c->prompt_id, p->id)) {
        c->prompt = MESH_PROMPT_DONE;
        c->prompt_done_at_ms = c->last_tick_ms;
    }
}

// -- receive -------------------------------------------------------------------------------------

void mesh_core_rx(mesh_core_t *c, const uint8_t *frame, size_t len, uint32_t now_ms, mesh_send_fn send,
                  void *ctx) {
    mesh_header_t h;
    if (!c->enabled || !c->has_identity || !mesh_decode_header(frame, len, &h)) return;
    if (id_eq(h.sender, c->self.id)) return; // our own rider on another bike, or an echo

    switch (h.type) {
    case MESH_TYPE_HELLO: {
        mesh_look_t look;
        uint8_t caps;
        if (!mesh_decode_hello(frame, len, &look, &caps)) return;
        mesh_peer_t *p = peer_touch(c, h.sender, now_ms);
        p->look = look;
        mesh_friend_t *f = (mesh_friend_t *)mesh_core_friend_find(c, h.sender);
        if (f != NULL && !look_eq(&f->look, &look)) {
            f->look = look;
            c->friends_generation++;
            push_event(c, MESH_FRIEND_EVENT_UPDATED, f);
        }
        break;
    }
    case MESH_TYPE_FRIEND_ACCEPT: {
        uint8_t target[MESH_RIDER_ID_LEN], key[MESH_KEY_LEN];
        if (!mesh_decode_accept(frame, len, target, key) || !id_eq(target, c->self.id)) return;
        mesh_peer_t *p = peer_find(c, h.sender);
        if (p == NULL) return; // not heard a HELLO yet: stay silent, their retries will land
        p->heard_ms = now_ms;
        send_ack(c, h.sender, send, ctx);
        mesh_friend_t *f = (mesh_friend_t *)mesh_core_friend_find(c, h.sender);
        if (f != NULL) {
            // Already friends here but not there (their list was lost): our earlier consent
            // stands, so answer once and let their side complete.
            if (memcmp(f->key, key, MESH_KEY_LEN) != 0) {
                memcpy(f->key, key, MESH_KEY_LEN);
                c->friends_generation++;
                push_event(c, MESH_FRIEND_EVENT_UPDATED, f);
            }
            if (!p->replied) {
                p->replied = true;
                send_accept(c, h.sender, send, ctx);
            }
            return;
        }
        memcpy(p->remote_key, key, MESH_KEY_LEN);
        p->remote_accepted = true;
        if (p->local_accepted) {
            // Their accept may have been the only one that got through: send ours once more.
            send_accept(c, h.sender, send, ctx);
            complete(c, p);
        }
        break;
    }
    case MESH_TYPE_FRIEND_ACK: {
        uint8_t target[MESH_RIDER_ID_LEN];
        if (!mesh_decode_ack(frame, len, target) || !id_eq(target, c->self.id)) return;
        mesh_peer_t *p = peer_find(c, h.sender);
        if (p != NULL) p->acked = true;
        break;
    }
    case MESH_TYPE_POS: {
        const mesh_friend_t *f = mesh_core_friend_find(c, h.sender);
        mesh_pos_t pos;
        if (f == NULL || !mesh_decode_pos(frame, len, f->key, &pos)) return;
        mesh_peer_t *p = peer_touch(c, h.sender, now_ms);
        if (!p->look.name[0] && !p->look.accent_rgb) p->look = f->look; // POS before any HELLO
        p->pos = pos;
        p->pos_ms = now_ms;
        p->has_pos = true;
        break;
    }
    default:
        break; // nav share (Phase 2) and anything newer
    }
}

// -- tick ----------------------------------------------------------------------------------------

static bool fresh(uint32_t now_ms, uint32_t at_ms, uint32_t max_age_ms) { return (now_ms - at_ms) <= max_age_ms; }

bool mesh_core_prompt_gate(bool navigating, uint32_t turn_distance_m, uint16_t speed_kmh_x10) {
    if (!navigating) return true;
    if (turn_distance_m > MESH_PROMPT_MIN_TURN_M) return true;
    return speed_kmh_x10 != 0xFFFF && speed_kmh_x10 < MESH_PROMPT_MAX_SPEED_KMH_X10;
}

static void close_prompt(mesh_core_t *c) {
    suppress(c, c->prompt_id);
    c->prompt = MESH_PROMPT_NONE;
}

static void run_prompt(mesh_core_t *c, uint32_t now_ms, uint32_t dt_ms, bool allowed) {
    c->prompt_visible = allowed;
    if (c->prompt == MESH_PROMPT_DONE) {
        if (now_ms - c->prompt_done_at_ms >= MESH_PROMPT_DONE_MS) c->prompt = MESH_PROMPT_NONE;
        return;
    }
    if (c->prompt != MESH_PROMPT_NONE) {
        mesh_peer_t *p = peer_find(c, c->prompt_id);
        if (p == NULL || !fresh(now_ms, p->heard_ms, MESH_PEER_TIMEOUT_MS)) {
            close_prompt(c); // rode apart
            return;
        }
        c->prompt_look = p->look;
        if (allowed) c->prompt_shown_ms += dt_ms;
        if (c->prompt_shown_ms >= MESH_PROMPT_TIMEOUT_MS) {
            p->local_accepted = false;
            close_prompt(c);
        }
        return;
    }
    if (!allowed || c->friend_count >= MESH_MAX_FRIENDS) return;
    for (size_t i = 0; i < MESH_MAX_PEERS; i++) {
        mesh_peer_t *p = &c->peers[i];
        if (!p->used || !fresh(now_ms, p->heard_ms, MESH_PEER_TIMEOUT_MS)) continue;
        if (mesh_core_friend_find(c, p->id) != NULL || suppressed(c, p->id)) continue;
        c->prompt = MESH_PROMPT_OFFER;
        memcpy(c->prompt_id, p->id, MESH_RIDER_ID_LEN);
        c->prompt_look = p->look;
        c->prompt_shown_ms = 0;
        return;
    }
}

void mesh_core_tick(mesh_core_t *c, uint32_t now_ms, bool prompt_allowed, mesh_send_fn send, void *ctx) {
    const uint32_t dt = c->ticked ? now_ms - c->last_tick_ms : 0;
    c->last_tick_ms = now_ms;
    c->ticked = true;
    if (!c->enabled || !c->has_identity) return;

    for (size_t i = 0; i < MESH_MAX_PEERS; i++) {
        mesh_peer_t *p = &c->peers[i];
        if (p->used && !fresh(now_ms, p->heard_ms, MESH_PEER_TIMEOUT_MS) &&
            !(c->prompt != MESH_PROMPT_NONE && id_eq(p->id, c->prompt_id))) {
            p->used = false;
        }
    }
    run_prompt(c, now_ms, dt, prompt_allowed);

    uint8_t buf[MESH_FRAME_MAX];
    if (now_ms - c->last_hello_ms >= MESH_HELLO_PERIOD_MS || c->last_hello_ms == 0) {
        c->last_hello_ms = now_ms ? now_ms : 1;
        mesh_look_t look = c->look;
        memcpy(look.name, c->self.name, sizeof(look.name));
        const mesh_header_t h = next_header(c, MESH_TYPE_HELLO);
        const size_t n = mesh_encode_hello(&h, &look, 0, buf, sizeof(buf));
        if (n) send(buf, n, ctx);
    }

    for (size_t i = 0; i < MESH_MAX_PEERS; i++) {
        mesh_peer_t *p = &c->peers[i];
        if (!p->used || !p->local_accepted || p->acked || p->tries >= MESH_ACCEPT_TRIES) continue;
        if (now_ms - p->last_try_ms < MESH_ACCEPT_RETRY_MS) continue;
        p->tries++;
        p->last_try_ms = now_ms;
        send_accept(c, p->id, send, ctx);
    }

    // Position only while a friend is actually listening, and only a fix we'd trust ourselves.
    if (!c->has_own_pos || !fresh(now_ms, c->own_pos_ms, MESH_OWN_POS_MAX_AGE_MS)) return;
    if (now_ms - c->last_pos_ms < MESH_POS_PERIOD_MS) return;
    bool friend_near = false;
    for (size_t i = 0; i < MESH_MAX_PEERS && !friend_near; i++) {
        const mesh_peer_t *p = &c->peers[i];
        friend_near = p->used && mesh_core_friend_find(c, p->id) != NULL;
    }
    if (!friend_near) return;
    c->last_pos_ms = now_ms;
    const mesh_header_t h = next_header(c, MESH_TYPE_POS);
    const size_t n = mesh_encode_pos(&h, c->self.key, c->session, &c->own_pos, buf, sizeof(buf));
    if (n) send(buf, n, ctx);
}

void mesh_core_accept(mesh_core_t *c, const uint8_t id[MESH_RIDER_ID_LEN], uint32_t now_ms, mesh_send_fn send,
                      void *ctx) {
    if (c->prompt != MESH_PROMPT_OFFER || !id_eq(id, c->prompt_id)) return;
    mesh_peer_t *p = peer_find(c, id);
    if (p == NULL) return;
    p->local_accepted = true;
    p->acked = false;
    p->tries = 1;
    p->last_try_ms = now_ms;
    c->prompt = MESH_PROMPT_WAITING;
    send_accept(c, id, send, ctx);
    if (p->remote_accepted) complete(c, p);
}

mesh_prompt_state_t mesh_core_prompt(const mesh_core_t *c, uint8_t id[MESH_RIDER_ID_LEN], mesh_look_t *look) {
    if (c->prompt == MESH_PROMPT_NONE || !c->prompt_visible) return MESH_PROMPT_NONE;
    if (id) memcpy(id, c->prompt_id, MESH_RIDER_ID_LEN);
    if (look) *look = c->prompt_look;
    return c->prompt;
}

size_t mesh_core_friends_in_range(const mesh_core_t *c, uint32_t now_ms, mesh_friend_fix_t *out, size_t cap) {
    if (!c->enabled || !c->has_own_pos || !fresh(now_ms, c->own_pos_ms, MESH_OWN_POS_MAX_AGE_MS)) return 0;
    size_t n = 0;
    for (size_t i = 0; i < MESH_MAX_PEERS && n < cap; i++) {
        const mesh_peer_t *p = &c->peers[i];
        if (!p->used || !p->has_pos || !fresh(now_ms, p->pos_ms, MESH_PEER_TIMEOUT_MS)) continue;
        const mesh_friend_t *f = mesh_core_friend_find(c, p->id);
        if (f == NULL) continue;
        out[n].f = f;
        mesh_geo(c->own_pos.lat_e7, c->own_pos.lon_e7, p->pos.lat_e7, p->pos.lon_e7, &out[n].bearing_deg,
                 &out[n].dist_m);
        n++;
    }
    return n;
}
