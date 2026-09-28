#include "ancs_core.h"

#include <string.h>

// The attributes we ask for on every notification, in this order. The Data Source response comes
// back in request order, but the parser below is order-independent anyway -- a future iOS could
// reorder them and nothing here would care.
//
// AppIdentifier tells us whether this is Google Maps (the nav path) or anything else; Title is the
// maneuver line on the nav path and the caller name on the call path (SPEC §5.5); Subtitle and
// Message carry the distance/ETA text that parse_distance_metres() reads; Date is the phone's own
// timestamp for the notification, which is the only age signal ANCS gives us.
#define ANCS_REQUESTED_ATTR_COUNT 5u

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

// Copies a non-terminated wire string into a fixed buffer, truncating on a byte boundary and
// always terminating. ANCS strings are UTF-8; truncation can split a sequence, which the display
// fold in call_packet.c and the ASCII-only fonts already handle downstream.
static void copy_wire_str(char *dst, size_t cap, const uint8_t *src, size_t len) {
    if (cap == 0) {
        return;
    }
    size_t n = len;
    if (n > cap - 1u) {
        n = cap - 1u;
    }
    if (n > 0) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

size_t ancs_encode_get_attributes(uint8_t *out, size_t cap, uint32_t uid) {
    // [cmd][uid u32] then per attribute [id], with a u16 max-length for the three that take one.
    // The max-lengths are our own buffer sizes minus the terminator, so the phone truncates for us
    // rather than us dropping the tail of a long street name.
    const size_t need = 1u + 4u + 1u + (1u + 2u) + (1u + 2u) + (1u + 2u) + 1u;
    if (cap < need) {
        return 0;
    }
    size_t o = 0;
    out[o++] = (uint8_t)ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES;
    wr_u32(&out[o], uid);
    o += 4;
    out[o++] = (uint8_t)ANCS_ATTR_APP_IDENTIFIER;
    out[o++] = (uint8_t)ANCS_ATTR_TITLE;
    wr_u16(&out[o], (uint16_t)(ANCS_TITLE_LEN - 1));
    o += 2;
    out[o++] = (uint8_t)ANCS_ATTR_SUBTITLE;
    wr_u16(&out[o], (uint16_t)(ANCS_SUBTITLE_LEN - 1));
    o += 2;
    out[o++] = (uint8_t)ANCS_ATTR_MESSAGE;
    wr_u16(&out[o], (uint16_t)(ANCS_MESSAGE_LEN - 1));
    o += 2;
    out[o++] = (uint8_t)ANCS_ATTR_DATE;
    return o;
}

size_t ancs_encode_perform_action(uint8_t *out, size_t cap, uint32_t uid, ancs_action_id_t action) {
    if (cap < 6u) {
        return 0;
    }
    out[0] = (uint8_t)ANCS_CMD_PERFORM_NOTIFICATION_ACTION;
    wr_u32(&out[1], uid);
    out[5] = (uint8_t)action;
    return 6u;
}

typedef enum {
    PARSE_INCOMPLETE = 0,
    PARSE_COMPLETE,
    PARSE_MALFORMED,
} parse_result_t;

// Walks a (possibly partial) Get Notification Attributes response. Only reports COMPLETE once all
// the attributes we asked for have arrived, so a caller can feed it the same growing buffer after
// every Data Source notification.
static parse_result_t parse_attributes(const uint8_t *data, size_t len, ancs_notification_t *out,
                                       size_t *consumed) {
    if (len < 5u) {
        return PARSE_INCOMPLETE;
    }
    if (data[0] != (uint8_t)ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES) {
        return PARSE_MALFORMED;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->uid = rd_u32(&data[1]);
    }

    size_t off = 5u;
    unsigned seen = 0;
    while (seen < ANCS_REQUESTED_ATTR_COUNT) {
        if (len - off < 3u) {
            return PARSE_INCOMPLETE;
        }
        const uint8_t id = data[off];
        const uint16_t alen = rd_u16(&data[off + 1]);
        // A length larger than the whole reassembly buffer can never complete, so it is a
        // desync rather than a slow arrival -- fail now instead of stalling the stream forever.
        if ((size_t)alen + 3u > ANCS_DS_BUF_LEN) {
            return PARSE_MALFORMED;
        }
        if (len - off < 3u + (size_t)alen) {
            return PARSE_INCOMPLETE;
        }
        const uint8_t *val = &data[off + 3];
        if (out != NULL) {
            switch (id) {
            case ANCS_ATTR_APP_IDENTIFIER:
                copy_wire_str(out->app_id, sizeof(out->app_id), val, alen);
                break;
            case ANCS_ATTR_TITLE:
                copy_wire_str(out->title, sizeof(out->title), val, alen);
                break;
            case ANCS_ATTR_SUBTITLE:
                copy_wire_str(out->subtitle, sizeof(out->subtitle), val, alen);
                break;
            case ANCS_ATTR_MESSAGE:
                copy_wire_str(out->message, sizeof(out->message), val, alen);
                break;
            case ANCS_ATTR_DATE:
                copy_wire_str(out->date, sizeof(out->date), val, alen);
                break;
            default:
                // An attribute we did not ask for. Skip it without counting it, so a phone that
                // volunteers extras cannot make us stop one attribute short.
                off += 3u + (size_t)alen;
                continue;
            }
        }
        off += 3u + (size_t)alen;
        seen++;
    }
    if (consumed != NULL) {
        *consumed = off;
    }
    return PARSE_COMPLETE;
}

bool ancs_parse_attributes(const uint8_t *data, size_t len, ancs_notification_t *out) {
    return parse_attributes(data, len, out, NULL) == PARSE_COMPLETE;
}

void ancs_core_init(ancs_core_t *c, ancs_write_cp_fn write_cp, ancs_notification_fn on_notification,
                    void *ctx) {
    memset(c, 0, sizeof(*c));
    c->write_cp = write_cp;
    c->on_notification = on_notification;
    c->ctx = ctx;
}

void ancs_core_set_raw_tap(ancs_core_t *c, ancs_raw_fn fn) {
    c->on_raw = fn;
}

void ancs_core_reset(ancs_core_t *c) {
    memset(c->tracked, 0, sizeof(c->tracked));
    c->ds_len = 0;
    c->ds_overflow = false;
    c->request_active = false;
    c->request_uid = 0;
}

static void raw_tap(ancs_core_t *c, ancs_raw_kind_t kind, const uint8_t *data, size_t len) {
    if (c->on_raw != NULL) {
        c->on_raw(kind, data, len, c->ctx);
    }
}

static ancs_tracked_t *find_tracked(ancs_core_t *c, uint32_t uid) {
    for (unsigned i = 0; i < ANCS_MAX_TRACKED; i++) {
        if (c->tracked[i].used && c->tracked[i].uid == uid) {
            return &c->tracked[i];
        }
    }
    return NULL;
}

static ancs_tracked_t *claim_tracked(ancs_core_t *c, uint32_t uid) {
    ancs_tracked_t *t = find_tracked(c, uid);
    if (t != NULL) {
        return t;
    }
    for (unsigned i = 0; i < ANCS_MAX_TRACKED; i++) {
        if (!c->tracked[i].used) {
            memset(&c->tracked[i], 0, sizeof(c->tracked[i]));
            c->tracked[i].used = true;
            c->tracked[i].uid = uid;
            return &c->tracked[i];
        }
    }
    // Table full. Evicting the oldest would need a clock this layer deliberately does not have,
    // so evict slot 0: on a phone busy enough to fill sixteen slots the nav and call notifications
    // are the ones being re-announced constantly and will re-claim immediately.
    memset(&c->tracked[0], 0, sizeof(c->tracked[0]));
    c->tracked[0].used = true;
    c->tracked[0].uid = uid;
    return &c->tracked[0];
}

int ancs_core_category_of(const ancs_core_t *c, uint32_t uid) {
    for (unsigned i = 0; i < ANCS_MAX_TRACKED; i++) {
        if (c->tracked[i].used && c->tracked[i].uid == uid) {
            return (int)c->tracked[i].category;
        }
    }
    return -1;
}

static void request_attributes(ancs_core_t *c, uint32_t uid) {
    if (c->write_cp == NULL) {
        return;
    }
    uint8_t buf[ANCS_CP_BUF_LEN];
    const size_t n = ancs_encode_get_attributes(buf, sizeof(buf), uid);
    if (n == 0) {
        return;
    }
    // A new request invalidates whatever partial response is in flight: the phone answers one
    // Get Notification Attributes at a time, so leftover bytes belong to a response we abandoned.
    c->ds_len = 0;
    c->ds_overflow = false;
    c->request_active = true;
    c->request_uid = uid;
    raw_tap(c, ANCS_RAW_CONTROL_POINT, buf, n);
    c->write_cp(buf, n, c->ctx);
}

bool ancs_core_on_notification_source(ancs_core_t *c, const uint8_t *data, size_t len) {
    raw_tap(c, ANCS_RAW_NOTIFICATION_SOURCE, data, len);
    if (len < ANCS_NOTIFICATION_SOURCE_LEN) {
        return false;
    }
    c->stat_events++;

    const uint8_t event_id = data[0];
    const uint8_t event_flags = data[1];
    const uint8_t category = data[2];
    // data[3] is CategoryCount: how many notifications of this category are on the phone. We do
    // not use it -- the terminal shows one thing at a time -- but it is in the raw capture.
    const uint32_t uid = rd_u32(&data[4]);

    if (event_id == ANCS_EVENT_REMOVED) {
        ancs_tracked_t *t = find_tracked(c, uid);
        const uint8_t cat = (t != NULL) ? t->category : category;
        if (t != NULL) {
            t->used = false;
        }
        if (c->request_active && c->request_uid == uid) {
            // The phone withdrew the notification mid-fetch. Drop the partial response rather
            // than completing a notification that no longer exists.
            c->request_active = false;
            c->ds_len = 0;
        }
        if (c->on_notification != NULL) {
            ancs_notification_t n;
            memset(&n, 0, sizeof(n));
            n.uid = uid;
            n.category = cat;
            n.event_flags = event_flags;
            c->on_notification(&n, true, c->ctx);
        }
        return true;
    }

    if (event_id != ANCS_EVENT_ADDED && event_id != ANCS_EVENT_MODIFIED) {
        return false;
    }

    ancs_tracked_t *t = claim_tracked(c, uid);
    t->category = category;
    t->event_flags = event_flags;

    // Turn-by-turn on iOS arrives as MODIFIED on one long-lived notification, so a modify has to
    // re-fetch attributes: the maneuver text changing is the whole event. There is no way to ask
    // for "what changed", only for the attributes again.
    if (!c->request_active) {
        t->attrs_requested = true;
        request_attributes(c, uid);
    } else if (c->request_uid == uid) {
        // Already fetching this one; the in-flight response is the fresh text.
        t->attrs_requested = true;
    } else {
        // Another fetch is in flight. Mark it so the next completion picks this one up rather
        // than queueing Control Point writes the phone would reject.
        t->attrs_requested = false;
    }
    return true;
}

// After a response completes, start the next pending fetch if any notification is still waiting.
static void pump_pending(ancs_core_t *c) {
    if (c->request_active) {
        return;
    }
    for (unsigned i = 0; i < ANCS_MAX_TRACKED; i++) {
        if (c->tracked[i].used && !c->tracked[i].attrs_requested) {
            c->tracked[i].attrs_requested = true;
            request_attributes(c, c->tracked[i].uid);
            return;
        }
    }
}

bool ancs_core_on_data_source(ancs_core_t *c, const uint8_t *data, size_t len) {
    raw_tap(c, ANCS_RAW_DATA_SOURCE, data, len);
    if (len == 0) {
        return true;
    }
    if (!c->request_active) {
        // Bytes for a response we abandoned (the notification was removed, or a newer request
        // superseded it). Drop them: mixing them into the next response would corrupt it.
        return true;
    }
    if (c->ds_len + len > sizeof(c->ds_buf)) {
        // Longer than anything we asked for can be. The response is unusable; abandon it and let
        // the next Notification Source event re-request. Never truncate into a parse -- that is
        // how a half-read street name becomes a wrong maneuver.
        c->ds_overflow = true;
        c->stat_ds_overflow++;
        c->ds_len = 0;
        c->request_active = false;
        pump_pending(c);
        return false;
    }
    memcpy(&c->ds_buf[c->ds_len], data, len);
    c->ds_len += len;

    ancs_notification_t notif;
    size_t consumed = 0;
    const parse_result_t r = parse_attributes(c->ds_buf, c->ds_len, &notif, &consumed);
    if (r == PARSE_INCOMPLETE) {
        return true;
    }
    if (r == PARSE_MALFORMED) {
        c->stat_ds_malformed++;
        c->ds_len = 0;
        c->request_active = false;
        pump_pending(c);
        return false;
    }

    const ancs_tracked_t *t = find_tracked(c, notif.uid);
    notif.category = (t != NULL) ? t->category : (uint8_t)ANCS_CATEGORY_OTHER;
    notif.event_flags = (t != NULL) ? t->event_flags : 0u;

    c->stat_attrs_complete++;
    c->ds_len = 0;
    c->request_active = false;

    if (c->on_notification != NULL) {
        c->on_notification(&notif, false, c->ctx);
    }
    pump_pending(c);
    return true;
}

bool ancs_core_perform_action(ancs_core_t *c, uint32_t uid, ancs_action_id_t action) {
    if (c->write_cp == NULL) {
        return false;
    }
    uint8_t buf[ANCS_CP_BUF_LEN];
    const size_t n = ancs_encode_perform_action(buf, sizeof(buf), uid, action);
    if (n == 0) {
        return false;
    }
    raw_tap(c, ANCS_RAW_CONTROL_POINT, buf, n);
    c->write_cp(buf, n, c->ctx);
    return true;
}
