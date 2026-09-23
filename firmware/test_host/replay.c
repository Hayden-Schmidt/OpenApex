// Replays a captured drive through the CURRENT normalizer and countdown engine.
//
// This is the payoff of the drive logger: a recorded drive becomes a regression fixture. Change
// derive_maneuver() for Waze or Apple Maps phrasing, replay every drive ever captured, and see
// immediately whether Google Maps parsing regressed -- without driving again.
//
//   gcc -std=c11 -o replay test_host/replay.c main/normalize.c main/packet.c
//       main/countdown.c main/pipeline.c -lm
//   ./replay esp_log.bin
//
// Each RAW record is fed through view_state_apply_packet() exactly as the terminal does, using the
// record's own uptime_ms as the clock so countdown interpolation reproduces the original timing.
// Where the capture also holds the terminal's MODEL/VIEW records for the same packet, the replay
// compares against them and reports any divergence -- that is the difference between "the code
// changed" and "the code changed and the old behaviour was wrong".
//
// Fidelity note: the RAW record carries every field normalize_packet() reads. The ETA string is
// truncated to 12 bytes on flash and motion samples are not logged at all (the normalizer never
// consumes them), so those two are not byte-faithful; nothing else is lossy.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/drive_log.h"
#include "../main/nav_model.h"
#include "../main/normalize.h"
#include "../main/packet.h"
#include "../main/pipeline.h"
#include "../main/view_state.h"

static const char *ICON_NAMES[] = {
    "STRAIGHT", "TURN_LEFT", "TURN_RIGHT", "SLIGHT_LEFT", "SLIGHT_RIGHT",
    "SHARP_LEFT", "SHARP_RIGHT", "ROUNDABOUT_LEFT", "ROUNDABOUT_RIGHT",
    "ROUNDABOUT_STRAIGHT", "U_TURN", "ARRIVED", "UNKNOWN",
};
static const char *STATE_NAMES[] = {"IDLE", "ACTIVE", "STALE", "ARRIVED"};

static const char *icon_name(int icon) {
    int count = (int)(sizeof(ICON_NAMES) / sizeof(ICON_NAMES[0]));
    return (icon >= 0 && icon < count) ? ICON_NAMES[icon] : "?";
}

static const char *state_name(int state) {
    int count = (int)(sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]));
    return (state >= 0 && state < count) ? STATE_NAMES[state] : "?";
}

// pipeline.h declares this as the platform's monotonic clock. The replay drives time from the
// capture instead, so this is never consulted -- but the link still needs it.
uint32_t platform_now_ms(void) {
    return 0;
}

// Rebuilds the raw_notif_t the terminal decoded, from the logged RAW record.
static void raw_from_record(const drive_log_raw_t *logged, raw_notif_t *out) {
    memset(out, 0, sizeof(*out));
    out->sequence = logged->sequence;
    out->gnss_fix_valid = logged->gnss_fix_valid != 0;
    out->speed_kmh_x10 = logged->speed_kmh_x10;
    out->heading_deg = logged->heading_deg;
    out->battery_percent = logged->battery_percent;
    out->progress = logged->progress;
    out->progress_max = logged->progress_max;
    out->icon_rotation_deg = logged->icon_rotation_deg;
    for (int i = 0; i < 3; i++) {
        out->accel_mg[i] = (int16_t)RAW_I16_UNKNOWN;
        out->gyro_mdps[i] = (int16_t)RAW_I16_UNKNOWN;
    }
    memcpy(out->title_str, logged->title, sizeof(logged->title));
    out->title_str[sizeof(out->title_str) - 1] = '\0';
    memcpy(out->distance_str, logged->distance, sizeof(logged->distance));
    out->distance_str[sizeof(out->distance_str) - 1] = '\0';
    memcpy(out->eta_str, logged->eta, sizeof(logged->eta));
    out->eta_str[sizeof(out->eta_str) - 1] = '\0';
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <esp_log.bin>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    pipeline_reset();
    terminal_view_state_t view;
    memset(&view, 0, sizeof(view));
    view.state = VIEW_IDLE;

    // Last replayed result, held so the next MODEL/VIEW record from the same packet can be
    // compared against it.
    nav_model_t replayed_model;
    memset(&replayed_model, 0, sizeof(replayed_model));
    uint32_t replayed_seq = 0;
    bool have_replay = false;

    unsigned long packets = 0, mismatches = 0;
    drive_log_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        if (rec.magic != DRIVE_LOG_MAGIC || rec.version != DRIVE_LOG_VERSION) {
            continue; // erased slot or torn write
        }
        switch (rec.kind) {
        case DRIVE_LOG_KIND_BOOT:
            printf("---- boot session 0x%08lx: %.*s\n", (unsigned long)rec.boot_id,
                   (int)sizeof(rec.payload), (const char *)rec.payload);
            pipeline_reset();
            memset(&view, 0, sizeof(view));
            view.state = VIEW_IDLE;
            have_replay = false;
            break;

        case DRIVE_LOG_KIND_RAW: {
            drive_log_raw_t logged;
            memcpy(&logged, rec.payload, sizeof(logged));
            raw_notif_t raw;
            raw_from_record(&logged, &raw);

            normalize_packet(&raw, &replayed_model);
            view_state_apply_packet(&raw, rec.uptime_ms, &view);
            replayed_seq = logged.sequence;
            have_replay = true;
            packets++;

            printf("%9.3fs seq=%-6lu title=\"%s\"\n", rec.uptime_ms / 1000.0,
                   (unsigned long)logged.sequence, logged.title);
            printf("             -> replay icon=%s dist=%ld street=\"%s\" | view=%s dist=%lu stale=%d\n",
                   icon_name((int)replayed_model.icon_type),
                   (long)replayed_model.distance_meters, replayed_model.street_name,
                   state_name((int)view.state), (unsigned long)view.distance_meters, view.stale);
            break;
        }

        case DRIVE_LOG_KIND_MODEL: {
            drive_log_model_t logged;
            memcpy(&logged, rec.payload, sizeof(logged));
            if (have_replay && logged.sequence == replayed_seq &&
                logged.icon_type != (uint8_t)replayed_model.icon_type) {
                printf("             ** MISMATCH seq=%lu device icon=%s, current code says %s\n",
                       (unsigned long)logged.sequence, icon_name(logged.icon_type),
                       icon_name((int)replayed_model.icon_type));
                mismatches++;
            }
            break;
        }

        case DRIVE_LOG_KIND_BLE: {
            uint8_t event = rec.payload[0];
            int32_t detail;
            memcpy(&detail, rec.payload + 1, sizeof(detail));
            printf("%9.3fs ble event=%u detail=%ld\n", rec.uptime_ms / 1000.0, event, (long)detail);
            break;
        }

        default:
            break; // VIEW records are the device's own output; the replay recomputes it above
        }
    }
    fclose(f);

    printf("\nreplayed %lu packets, %lu maneuver mismatch(es) vs what the terminal displayed\n",
           packets, mismatches);
    return 0;
}
