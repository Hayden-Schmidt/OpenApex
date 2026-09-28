#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Apple Notification Center Service protocol core (docs/OpenApex_SPEC.md §5.2, §5.5).
//
// On iOS there is no notification-listener API a phone app could use, so the terminal itself is
// the GATT client of the iPhone's ANCS and reads notifications straight off the phone. This file
// is the protocol half of that: Notification Source events in, Control Point requests out, Data
// Source bytes reassembled into attributes. No NimBLE, no clock, no NVS -- the caller passes bytes
// in, time in, and a write callback out -- so the whole thing runs in host tests exactly like
// mesh_core.c. ancs.c owns the only instance on the terminal and wires it to the real GATT client.
//
// Wire reference: Apple's "Apple Notification Center Service (ANCS) Specification". Multi-byte
// fields are little-endian. Strings are UTF-8 and are NOT null-terminated on the wire; this layer
// terminates them.

// ANCS UUIDs, canonical (big-endian) text form. ancs.c byte-reverses these for NimBLE.
#define ANCS_SERVICE_UUID_STR "7905F431-B5CE-4E99-A40F-4B1E122D00D0"
#define ANCS_CHR_NOTIFICATION_SOURCE_UUID_STR "9FBF120D-6301-42D9-8C58-25E699A21DBD"
#define ANCS_CHR_CONTROL_POINT_UUID_STR "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9"
#define ANCS_CHR_DATA_SOURCE_UUID_STR "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB"

#define ANCS_NOTIFICATION_SOURCE_LEN 8u

typedef enum {
    ANCS_EVENT_ADDED = 0,
    ANCS_EVENT_MODIFIED = 1,
    ANCS_EVENT_REMOVED = 2,
} ancs_event_id_t;

// EventFlags bits. PositiveAction/NegativeAction say whether Perform Notification Action is
// meaningful for this notification -- for an incoming call, positive is answer (SPEC §5.5).
#define ANCS_EVENT_FLAG_SILENT 0x01u
#define ANCS_EVENT_FLAG_IMPORTANT 0x02u
#define ANCS_EVENT_FLAG_PRE_EXISTING 0x04u
#define ANCS_EVENT_FLAG_POSITIVE_ACTION 0x08u
#define ANCS_EVENT_FLAG_NEGATIVE_ACTION 0x10u

typedef enum {
    ANCS_CATEGORY_OTHER = 0,
    ANCS_CATEGORY_INCOMING_CALL = 1,
    ANCS_CATEGORY_MISSED_CALL = 2,
    ANCS_CATEGORY_VOICEMAIL = 3,
    ANCS_CATEGORY_SOCIAL = 4,
    ANCS_CATEGORY_SCHEDULE = 5,
    ANCS_CATEGORY_EMAIL = 6,
    ANCS_CATEGORY_NEWS = 7,
    ANCS_CATEGORY_HEALTH_AND_FITNESS = 8,
    ANCS_CATEGORY_BUSINESS_AND_FINANCE = 9,
    ANCS_CATEGORY_LOCATION = 10, // Google Maps posts turn-by-turn here on iOS
    ANCS_CATEGORY_ENTERTAINMENT = 11,
} ancs_category_id_t;

typedef enum {
    ANCS_CMD_GET_NOTIFICATION_ATTRIBUTES = 0,
    ANCS_CMD_GET_APP_ATTRIBUTES = 1,
    ANCS_CMD_PERFORM_NOTIFICATION_ACTION = 2,
} ancs_command_id_t;

typedef enum {
    ANCS_ATTR_APP_IDENTIFIER = 0,
    ANCS_ATTR_TITLE = 1,    // takes a u16 max-length in the request
    ANCS_ATTR_SUBTITLE = 2, // takes a u16 max-length
    ANCS_ATTR_MESSAGE = 3,  // takes a u16 max-length
    ANCS_ATTR_MESSAGE_SIZE = 4,
    ANCS_ATTR_DATE = 5,
    ANCS_ATTR_POSITIVE_ACTION_LABEL = 6,
    ANCS_ATTR_NEGATIVE_ACTION_LABEL = 7,
} ancs_notification_attr_id_t;

typedef enum {
    ANCS_ACTION_POSITIVE = 0, // answers an incoming call
    ANCS_ACTION_NEGATIVE = 1,
} ancs_action_id_t;

// Attribute buffer sizes. Title carries the maneuver text on the nav path and the caller name on
// the call path; Message carries the distance/ETA line. Sized against what normalize.c and
// call_packet.h already accept downstream, so nothing truncates twice.
#define ANCS_APP_ID_LEN 48
#define ANCS_TITLE_LEN 64
#define ANCS_SUBTITLE_LEN 64
#define ANCS_MESSAGE_LEN 128
#define ANCS_DATE_LEN 16 // "yyyyMMdd'T'HHmmSS" is 15 chars plus terminator

// One notification's fetched attributes. Absent attributes are empty strings, never fabricated.
typedef struct {
    uint32_t uid;
    uint8_t category;    // ancs_category_id_t
    uint8_t event_flags; // ANCS_EVENT_FLAG_*
    char app_id[ANCS_APP_ID_LEN];
    char title[ANCS_TITLE_LEN];
    char subtitle[ANCS_SUBTITLE_LEN];
    char message[ANCS_MESSAGE_LEN];
    char date[ANCS_DATE_LEN];
} ancs_notification_t;

// Live notifications the phone has told us about. Small: we only ever care about the current
// navigation notification and a ringing call, but a phone with a busy lock screen will announce
// plenty of others before we filter them.
#define ANCS_MAX_TRACKED 16u

// Data Source reassembly buffer. One Get Notification Attributes response is
// 5 bytes of header plus, per attribute, 3 bytes plus its value. Sized for every attribute we ask
// for at full length, with slack.
#define ANCS_DS_BUF_LEN 448u

// Largest Control Point request we build: command + uid + five attribute ids + three u16 lengths.
#define ANCS_CP_BUF_LEN 32u

typedef void (*ancs_write_cp_fn)(const uint8_t *data, size_t len, void *ctx);

// Fired when a notification's attributes have all arrived. Also fired for a REMOVED event, with
// `removed` true and only uid/category populated -- the phone does not re-send attributes for a
// notification it has just withdrawn.
typedef void (*ancs_notification_fn)(const ancs_notification_t *notif, bool removed, void *ctx);

// Raw-byte tap for the day-one capture (drive_log). Called before any parsing, so a stream we
// fail to understand is still recorded verbatim. NULL to disable.
typedef enum {
    ANCS_RAW_NOTIFICATION_SOURCE = 0,
    ANCS_RAW_DATA_SOURCE = 1,
    ANCS_RAW_CONTROL_POINT = 2, // outbound
} ancs_raw_kind_t;

typedef void (*ancs_raw_fn)(ancs_raw_kind_t kind, const uint8_t *data, size_t len, void *ctx);

typedef struct {
    bool used;
    uint32_t uid;
    uint8_t category;
    uint8_t event_flags;
    bool attrs_requested;
} ancs_tracked_t;

typedef struct {
    ancs_write_cp_fn write_cp;
    ancs_notification_fn on_notification;
    ancs_raw_fn on_raw;
    void *ctx;

    ancs_tracked_t tracked[ANCS_MAX_TRACKED];

    // Data Source reassembly. A response is split across notifications at arbitrary boundaries,
    // including mid-length-field, so nothing may assume a frame starts an attribute.
    uint8_t ds_buf[ANCS_DS_BUF_LEN];
    size_t ds_len;
    bool ds_overflow; // dropped bytes: the current response is unusable, resync on the next header

    // The request in flight. ANCS answers one Get Notification Attributes at a time, so requests
    // queue behind this rather than racing on the Control Point.
    bool request_active;
    uint32_t request_uid;

    uint32_t stat_events;
    uint32_t stat_attrs_complete;
    uint32_t stat_ds_overflow;
    uint32_t stat_ds_malformed;
} ancs_core_t;

void ancs_core_init(ancs_core_t *c, ancs_write_cp_fn write_cp, ancs_notification_fn on_notification,
                    void *ctx);

// Raw capture tap, for the ANCS drive-log tier. Safe to call before or after init of the link.
void ancs_core_set_raw_tap(ancs_core_t *c, ancs_raw_fn fn);

// Drops all tracked state. Call on disconnect: notification UIDs are per-connection.
void ancs_core_reset(ancs_core_t *c);

// Feed one Notification Source notification (8 bytes). Returns false if malformed.
bool ancs_core_on_notification_source(ancs_core_t *c, const uint8_t *data, size_t len);

// Feed one Data Source notification (any length, any split point). Returns false if the stream
// went malformed, in which case it is dropped and the next response resyncs.
bool ancs_core_on_data_source(ancs_core_t *c, const uint8_t *data, size_t len);

// Ask the phone to answer (POSITIVE) or decline (NEGATIVE) the notification. Used for the call
// Accept hold: on iOS there is no CALL_ACTION characteristic, this is the answer path (SPEC §5.5).
bool ancs_core_perform_action(ancs_core_t *c, uint32_t uid, ancs_action_id_t action);

// Category of a tracked uid, or -1 if we do not know it.
int ancs_core_category_of(const ancs_core_t *c, uint32_t uid);

// ---------------------------------------------------------------------------------------------
// Pure encoders/decoders, exposed for the host tests.
// ---------------------------------------------------------------------------------------------

// Builds a Get Notification Attributes request for the attributes we always want. Returns the
// length written, or 0 if the buffer is too small.
size_t ancs_encode_get_attributes(uint8_t *out, size_t cap, uint32_t uid);

// Builds a Perform Notification Action request. Returns length written, or 0.
size_t ancs_encode_perform_action(uint8_t *out, size_t cap, uint32_t uid, ancs_action_id_t action);

// Parses a complete Get Notification Attributes response body into `out`. Returns false if the
// buffer is not a complete, well-formed response.
bool ancs_parse_attributes(const uint8_t *data, size_t len, ancs_notification_t *out);

#ifdef __cplusplus
}
#endif
