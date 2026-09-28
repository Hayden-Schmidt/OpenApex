#include "ancs_bridge.h"
#include "apple_client.h"
#include "avatar_store.h"
#include "ble_link.h"
#include "call_state.h"
#include "countdown.h"
#include "drive_log.h"
#include "display_driver.h"
#include "touch_driver.h"
#include "gui_app.hpp"
#include "heading_store.h"
#include "mesh.h"
#include "odometer_store.h"
#include "packet.h"
#include "pipeline.h"
#include "settings.h"
#include "settings_store.h"
#include "view_state.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "openapex";

#define RAW_PACKET_QUEUE_LEN 8
#define CALL_EVENT_QUEUE_LEN 4
#define CALL_ACCEPT_QUEUE_LEN 2

// Concurrency model
// ------------------
// raw_packet_queue: single-producer (ble_handler_task) / single-consumer (countdown_task) bounded
//   FreeRTOS queue of decoded raw_notif_t. ble_handler_task must never block on a full queue for
//   longer than a short bounded wait — BLE callbacks must not stall on rendering or countdown work.
// view_mutex + shared_view: single-writer (countdown_task only) / multi-reader (gui_task, and
//   later diagnostics/system_manager_task) shared terminal_view_state_t. countdown_task is the
//   only task allowed to write shared_view, always under view_mutex. Any other task — gui_task
//   included — MUST read it only through view_state_snapshot() below, never by touching
//   shared_view directly. countdown.c's own internal state (current/filtered_speed_kmh/
//   has_baseline) is private to countdown_task's call path and must not be called from any other
//   task.
// call_event_queue: NimBLE host task -> countdown_task, same bounded-wait rule as raw_packet_queue.
// call_accept_queue: gui_task -> countdown_task. The Accept tap lands here rather than calling BLE
//   from the GUI (�346); countdown_task sends it on.
static QueueHandle_t raw_packet_queue;
static QueueHandle_t call_event_queue;
static QueueHandle_t call_accept_queue;
static SemaphoreHandle_t view_mutex;
static terminal_view_state_t shared_view;

// Copies the latest view state under view_mutex. This is the only sanctioned read path for
// shared_view outside of countdown_task; gui_task and any future reader task must call this
// instead of touching shared_view directly.
static void view_state_snapshot(terminal_view_state_t *out) {
    if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        *out = shared_view;
        xSemaphoreGive(view_mutex);
    }
}

uint32_t platform_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Day-one capture tap: every ANCS byte, both directions, before anything parses it. What iOS
// actually sends is the open question the first real iPhone answers, so a stream we fail to
// understand still has to survive verbatim (tools/decode_ancs.py reads these back).
static void log_ancs_raw(ancs_raw_kind_t kind, const uint8_t *data, size_t len, void *ctx) {
    (void)ctx;
    drive_log_ancs((uint8_t)kind, data, (uint16_t)len);
    // Also to the serial monitor, so a bench session answers "what did iOS just send" live
    // rather than only after pulling the flash partition. ANCS is a few frames per maneuver,
    // not the 3 Hz of the Android packet path, so this is not a firehose.
    static const char *kKind[] = {"NS", "DS", "CP->"};
    char hex[3 * 32 + 4];
    size_t shown = len > 32 ? 32 : len;
    size_t o = 0;
    for (size_t i = 0; i < shown; i++) {
        o += (size_t)snprintf(&hex[o], sizeof(hex) - o, "%02x ", data[i]);
    }
    if (shown < len) {
        snprintf(&hex[o], sizeof(hex) - o, "...");
    }
    ESP_LOGI(TAG, "ancs %s %uB: %s", kind < 3 ? kKind[kind] : "?", (unsigned)len, hex);
}

// gui_app's Accept seam. Runs on gui_task; never blocks it.
static void on_call_accept(uint16_t call_id) {
    xQueueSend(call_accept_queue, &call_id, 0);
}

// Pushes the pipeline-side settings (ETA format, clock) whenever the phone changes them. Called on
// countdown_task, the only task that runs the pipeline.
static void apply_pipeline_settings(void) {
    static uint32_t applied_generation;
    const uint32_t gen = settings_generation();
    if (gen == applied_generation) return;
    applied_generation = gen;
    const terminal_settings_t *s = settings_get();
    pipeline_set_eta_format((pipeline_eta_format_t)s->eta_format);
    pipeline_set_clock_12h(s->clock_12h != 0);
}

// Rider mesh prompt and friend markers into the view. Under view_mutex.
static void fill_mesh_view(terminal_view_state_t *v) {
    v->friend_prompt = mesh_get_prompt(v->friend_prompt_id, &v->friend_peer, &v->friend_self);
    v->friend_count = (uint8_t)mesh_get_friends_in_range(v->friends, VIEW_FRIENDS_MAX);
}

// gui_app's friend-accept seam. Runs on gui_task; mesh_request_accept only queues.
static void on_friend_accept(const uint8_t id[MESH_RIDER_ID_LEN]) {
    mesh_request_accept(id);
}

// Our fix for the rider mesh, from a v5 packet that carries one.
static void feed_mesh_pos(const raw_notif_t *raw) {
    if (raw->lat_e7 == RAW_I32_UNKNOWN || raw->lon_e7 == RAW_I32_UNKNOWN) return;
    const mesh_pos_t pos = {
        .lat_e7 = raw->lat_e7,
        .lon_e7 = raw->lon_e7,
        .speed_kmh_x10 = raw->speed_kmh_x10,
        .heading_deg_x10 = raw->heading_deg == RAW_U16_UNKNOWN ? 0xFFFFu : (uint16_t)(raw->heading_deg * 10u),
        .acc_m = raw->h_accuracy_m > 255u ? 255u : (uint8_t)raw->h_accuracy_m,
    };
    mesh_set_own_pos(&pos);
}

static void countdown_task(void *argument) {
    (void)argument;
    raw_notif_t raw;
    for (;;) {
        apply_pipeline_settings();
        while (xQueueReceive(raw_packet_queue, &raw, 0) == pdTRUE) {
            // Static: the view state is over 1 KB with the mesh fields, too much for this stack.
            static terminal_view_state_t next;
            memset(&next, 0, sizeof(next));
            view_state_apply_packet(&raw, platform_now_ms(), &next);
            feed_mesh_pos(&raw);
            next.phone_connected = ble_link_is_connected();
            call_state_fill(&next, platform_now_ms());
            fill_mesh_view(&next);
            if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                shared_view = next;
                xSemaphoreGive(view_mutex);
            }
        }

        uint32_t now = platform_now_ms();
        call_event_t call;
        while (xQueueReceive(call_event_queue, &call, 0) == pdTRUE) {
            call_state_apply(&call, now);
        }
        call_state_tick(now, ble_link_is_connected());
        uint16_t accept_id;
        while (xQueueReceive(call_accept_queue, &accept_id, 0) == pdTRUE) {
            // On iOS the call came from ANCS and is answered by writing the notification's
            // POSITIVE action back to the phone; there is no CALL_ACTION characteristic in that
            // direction. ancs_bridge claims the tap when the call is one of its own.
            if (!ancs_bridge_accept_call(accept_id)) {
                ble_link_send_call_accept(accept_id);
            }
        }
        // ANCS ring refresh and any pending Apple-services rediscovery.
        ancs_bridge_tick(now);
        // While a phone is connected but the Apple services have not come up, say so every 10s
        // with the full handle table. On iOS that gap is the single most likely place for the
        // whole path to be stuck (no bond, or iOS has not published ANCS yet), and it is
        // invisible from the screen.
        {
            static uint32_t next_dump_ms;
            // Only while there is still a chance of ANCS turning up. On an Android phone there
            // never is -- it publishes neither ANCS nor CTS -- so once apple_client has given up
            // retrying, repeating the same empty table every 10 s for the rest of the ride buries
            // everything else in the log.
            if (ble_link_is_connected() && !apple_client_ancs_ready() && apple_client_may_retry()) {
                if (next_dump_ms == 0 || (int32_t)(now - next_dump_ms) >= 0) {
                    next_dump_ms = now + 10000u;
                    apple_client_dump();
                }
            } else {
                next_dump_ms = 0;
            }
        }
        // Self-throttled to ~30s; cheap to call on every 100ms tick (see heading_store.h).
        heading_store_maybe_save(now);
        odometer_store_maybe_save(now);
        settings_store_maybe_save(now);
        if (xSemaphoreTake(view_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            view_state_tick(now, &shared_view);
            shared_view.phone_connected = ble_link_is_connected();
            call_state_fill(&shared_view, now);
            // A call covers the prompt, so its on-screen timeout must not run underneath.
            mesh_set_prompt_allowed(!shared_view.call_ringing && !shared_view.call_active &&
                                    mesh_core_prompt_gate(shared_view.state == VIEW_ACTIVE,
                                                          shared_view.distance_meters,
                                                          shared_view.speed_kmh_x10));
            fill_mesh_view(&shared_view);
            // A link that is delivering writes we cannot decode is NOT the same as no link and
            // NOT the same as "not navigating", but the terminal used to render all three
            // identically. On 2026-09-24 three whole sessions sat on the idle screen while the
            // phone believed it was relaying: every 146-byte packet was truncated to 20 bytes by
            // an unnegotiated ATT MTU and discarded here, so the view never advanced past boot.
            // The rider had no way to tell a quiet road from a broken link.
            //
            // Marking the view stale greys the dial (dial_screen.cpp), which at least says
            // "what you are looking at is not live". A dedicated fault screen naming the cause
            // would be better and is follow-up work -- gui_app only distinguishes idle from dial
            // today, so a new state means a new screen class.
            if (ble_link_is_faulted()) {
                shared_view.stale = true;
                if (shared_view.state == VIEW_ACTIVE) {
                    shared_view.state = VIEW_STALE;
                }
            }
            xSemaphoreGive(view_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void gui_task(void *argument) {
    (void)argument;
    // C++ screen classes (firmware/gui, §16.6) live behind gui_app_init/gui_app_update and must
    // only ever see the local snapshot below, never shared_view or view_mutex directly.
    lv_init();
    lv_tick_set_cb(platform_now_ms);
    display_driver_init();
    touch_driver_init();

    // The avatar is read here, on the one task that hands it to LVGL. Settings are already loaded (app_main), so gui_app_init draws the splash in
    // the rider's colour with their avatar from the very first frame, phone or no phone.
    uint8_t avatar[AVATAR_STORE_BYTES];
    gui_app_set_avatar(avatar_store_read(avatar) ? avatar : NULL);
    gui_app_init();
    gui_app_set_call_accept_handler(on_call_accept);
    gui_app_set_friend_accept_handler(on_friend_accept);

    // Paint the boot screen fully, *then* light the panel. lv_refr_now blocks until every pending
    // partial-flush area has been transferred (flush_cb is DMA-async), so the first thing the rider
    // sees is the complete logo rather than the panel's uninitialized GRAM. gui_app_init() has
    // already loaded SplashScreen, so no frame data is needed to draw it -- which is why this can
    // happen before the first snapshot below.
    lv_refr_now(NULL);
    display_driver_backlight_on();

    for (;;) {
        terminal_view_state_t frame;
        view_state_snapshot(&frame);
        if (avatar_store_take_changed()) {
            gui_app_set_avatar(avatar_store_read(avatar) ? avatar : NULL);
        }
        gui_app_update(&frame);
        // Sleep exactly until LVGL's next timer is due, not a fixed 16ms: a fixed poll beats against
        // the 33ms refresh period, so a frame landed on the 2nd or 3rd poll alternately (~32/48ms)
        // and animations judder. Clamped so the view snapshot is still read at least every 16ms,
        // and to 1 tick minimum so lower-priority tasks always get the CPU.
        uint32_t idle_ms = lv_timer_handler();
        if (idle_ms > 16) idle_ms = 16;
        TickType_t ticks = pdMS_TO_TICKS(idle_ms);
        vTaskDelay(ticks > 0 ? ticks : 1);
    }
}

void app_main(void) {
    display_driver_hold_dark();
    ESP_LOGI(TAG, "OpenApex terminal boot: PROTOTYPE_C3_GC9A01");
    pipeline_reset();
    call_state_reset();
    memset(&shared_view, 0, sizeof(shared_view));
    shared_view.state = VIEW_IDLE;

    // NVS first, ahead of the GUI: saved settings (accent, avatar choice, units) must be in place
    // before gui_task paints the splash, and must not wait on the phone. This used to live in
    // ble_link_init; it is a few ms on a normal boot, and the backlight stays off until the splash
    // is fully drawn regardless (gui_task).
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    settings_store_load();

    raw_packet_queue = xQueueCreate(RAW_PACKET_QUEUE_LEN, sizeof(raw_notif_t));
    call_event_queue = xQueueCreate(CALL_EVENT_QUEUE_LEN, sizeof(call_event_t));
    call_accept_queue = xQueueCreate(CALL_ACCEPT_QUEUE_LEN, sizeof(uint16_t));
    view_mutex = xSemaphoreCreateMutex();

    // Dev-only; no-ops entirely in the production build. Started before ble_link_init so the very
    // first connect/packet of a session is captured.
    drive_log_init();
    drive_log_boot("PROTOTYPE_C3_GC9A01");

    // Started before ble_link_init, not after. gui_task depends on nothing but LVGL and the panel,
    // while ble_link_init brings up NVS and the whole NimBLE host -- hundreds of milliseconds during
    // which the screen would otherwise sit dark. The boot screen's 2s minimum hold (gui_app.cpp) is
    // measured from here, so that startup work now happens *behind* the logo instead of before it.
    // 4096 was enough while the distance label only ever rendered "" (distance was always unknown
    // pre-fix, see pipeline.c), so LVGL's font/glyph rendering path was never exercised on real
    // hardware and its stack use went unnoticed until the first live packet with a known distance
    // caused a stack protection fault here.
    xTaskCreate(gui_task, "gui_task", 8192, NULL, 5, NULL);

    // ble_link_init spawns NimBLE's own host task (single-producer into raw_packet_queue); there
    // is no separate ble_handler_task to create. NVS (for BLE bonding, and heading_store_load()
    // below) is already up from the top of app_main -- load the cached heading/mount-offset only after this,
    // and before countdown_task starts decoding packets so the first packet already has a warm
    // offset estimator instead of reconverging from scratch every ride.
    // Before ble_link_init: the identity and friends characteristics call into mesh.c as soon as a
    // phone connects.
    mesh_init();
    // The iOS path. apple_client is the terminal's GATT-CLIENT role on the same link where
    // ble_link is the server: ANCS supplies notifications and calls, CTS the wall clock, and
    // both keep working with no phone app running -- which on iOS they have to, because iOS
    // will suspend and then kill it. Set up before ble_link_init: ble_link's GAP callback
    // starts calling into apple_client the moment a phone connects.
    {
        const apple_client_cfg_t apple_cfg = {
            .on_notification = ancs_bridge_on_notification,
            .on_time = ancs_bridge_on_time,
            .on_raw = log_ancs_raw,
        };
        ancs_bridge_init(raw_packet_queue, call_event_queue);
        apple_client_init(&apple_cfg);
    }
    ble_link_init(raw_packet_queue, call_event_queue);
    mesh_start();
    heading_store_load();
    odometer_store_load();
    xTaskCreate(countdown_task, "countdown_task", 4096, NULL, 4, NULL);
}
