#include "mesh_console.h"

#if defined(OPENAPEX_MESH_CONSOLE) && OPENAPEX_MESH_CONSOLE

#include <stdio.h>
#include <string.h>

#include <math.h>
#include <stdlib.h>

#include "hal/usb_serial_jtag_ll.h"
#include "gui_app.hpp"
#include "mesh.h"
#include "settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "meshcon";

#define LINE_MAX_LEN 64
#define HEAP_LOG_PERIOD_MS 10000

static void log_heap(void) {
    ESP_LOGI(TAG, "heap free=%u min=%u largest=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// Stand-ins for the terminal UI (U1-U5), so a board with no screen can drive the radio side of each
// flow. One command per line:
//   add (open Add friend on screen) | heap | menu on|off | nearby | req|hold <n> | ok | rssi <dBm> | status | friends | forget | id |
//   pos [follow] <n> <e> | pos off | accent <hex>
static void hex4(const uint8_t *id, char out[9]) {
    static const char *kHex = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        out[2 * i] = kHex[id[i] >> 4];
        out[2 * i + 1] = kHex[id[i] & 15];
    }
    out[8] = '\0';
}

// `pos <north_m> <east_m>` feeds mesh_set_own_pos once a second from a fixed origin, so two boards on a
// desk can be placed relative to each other; `pos off` stops.
// `pos follow <north_m> <east_m>` uses the last fix a friend sent as the origin instead, so a board with
// no phone shows up beside a friend riding with real GPS.
static bool s_sim_on, s_sim_follow;
static double s_sim_n, s_sim_e;

static void sim_tick(void) {
    static uint32_t at;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_sim_on || now - at < 1000u) return;
    at = now;
    const double e7_per_m = 89.9322;
    int32_t lat0 = -338688000, lon0 = 1512093000;
    if (s_sim_follow) {
        mesh_pos_t f;
        if (!mesh_debug_friend_fix(&f)) return; // nothing heard yet
        lat0 = f.lat_e7;
        lon0 = f.lon_e7;
    }
    const double lon_e7_per_m = e7_per_m / cos((double)lat0 * 1e-7 * 3.14159265358979 / 180.0);
    const mesh_pos_t p = {.lat_e7 = lat0 + (int32_t)(s_sim_n * e7_per_m),
                          .lon_e7 = lon0 + (int32_t)(s_sim_e * lon_e7_per_m),
                          .speed_kmh_x10 = 0xFFFF, .heading_deg_x10 = 0xFFFF, .acc_m = 3};
    mesh_debug_set_pos(&p, true);
}

static void cmd_nearby(void) {
    mesh_candidate_t cand[3];
    const size_t n = mesh_get_candidates(cand, 3);
    for (size_t i = 0; i < n; i++) {
        char id[9];
        hex4(cand[i].id, id);
        ESP_LOGI(TAG, "card %u: '%s' avatar=%u rgb=%06x rssi=%d%s id=%s", (unsigned)i, cand[i].look.name,
                 cand[i].look.avatar, (unsigned)cand[i].look.accent_rgb, cand[i].rssi_dbm,
                 cand[i].waiting ? " WAITING" : "", id);
    }
    if (n == 0) ESP_LOGI(TAG, "card: nobody (both in Add friend? close enough? try `menu on`, `rssi -90`)");
}

static void cmd_status(void) {
    uint8_t pid[MESH_RIDER_ID_LEN];
    mesh_look_t peer, self;
    const mesh_prompt_state_t st = mesh_get_prompt(pid, &peer, &self);
    static const char *const kName[] = {"none", "OFFER (send `ok`)", "WAITING", "DONE"};
    char id[9];
    hex4(pid, id);
    if (st == MESH_PROMPT_NONE) ESP_LOGI(TAG, "prompt: none");
    else ESP_LOGI(TAG, "prompt: %s from '%s' id=%s", kName[st], peer.name, id);
    mesh_friend_marker_t m[3];
    const size_t n = mesh_get_friends_in_range(m, 3);
    for (size_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "friend '%s' bearing=%.0f dist=%.1f m", m[i].look.name, m[i].bearing_deg, m[i].dist_m);
    }
    if (n == 0) ESP_LOGI(TAG, "no friend with a fresh position");
}

static void run_command(const char *line) {
    if (strcmp(line, "add") == 0) {
        gui_app_open_friend_menu();
        ESP_LOGI(TAG, "Add friend opened on screen (double tap to close)");
    } else if (strcmp(line, "heap") == 0) {
        log_heap();
    } else if (strcmp(line, "menu on") == 0 || strcmp(line, "menu off") == 0) {
        mesh_set_menu_open(line[6] == 'n');
        ESP_LOGI(TAG, "menu %s", line[6] == 'n' ? "open" : "closed");
    } else if (strcmp(line, "nearby") == 0) {
        cmd_nearby();
    } else if (strncmp(line, "req ", 4) == 0 || strncmp(line, "hold ", 5) == 0) {
        mesh_candidate_t cand[3];
        const size_t n = mesh_get_candidates(cand, 3);
        const long i = strtol(strchr(line, ' ') + 1, NULL, 10);
        if (i >= 0 && (size_t)i < n) {
            mesh_hold_friend(cand[i].id);
            ESP_LOGI(TAG, "held on '%s'", cand[i].look.name);
        } else {
            ESP_LOGW(TAG, "no card %ld", i);
        }
    } else if (strncmp(line, "rssi ", 5) == 0) {
        const long dbm = strtol(line + 5, NULL, 10);
        mesh_debug_set_pair_min_rssi((int8_t)dbm);
        ESP_LOGI(TAG, "pair threshold %ld dBm", dbm);
    } else if (strcmp(line, "ok") == 0) {
        uint8_t pid[MESH_RIDER_ID_LEN];
        mesh_look_t peer, self;
        if (mesh_get_prompt(pid, &peer, &self) == MESH_PROMPT_OFFER) {
            mesh_request_accept(pid);
            ESP_LOGI(TAG, "accepted '%s'", peer.name);
        } else {
            ESP_LOGW(TAG, "no incoming request");
        }
    } else if (strcmp(line, "status") == 0 || strcmp(line, "friends") == 0) {
        cmd_status();
    } else if (strncmp(line, "pos ", 4) == 0) {
        char *end;
        const char *args = line + 4;
        s_sim_follow = strncmp(args, "follow", 6) == 0;
        if (s_sim_follow) args += 6;
        s_sim_n = strtod(args, &end);
        s_sim_e = strtod(end, NULL);
        s_sim_on = strncmp(line + 4, "off", 3) != 0;
        if (!s_sim_on) mesh_debug_set_pos(NULL, false);
        ESP_LOGI(TAG, "sim position %s (%.0f m N, %.0f m E of %s)", s_sim_on ? "on" : "off", s_sim_n, s_sim_e,
                 s_sim_follow ? "a friend" : "the desk origin");
    } else if (strncmp(line, "accent ", 7) == 0) {
        terminal_settings_t st = *settings_get();
        st.accent_rgb = (uint32_t)strtoul(line + 7, NULL, 16) & 0xFFFFFFu;
        settings_set(&st);
        ESP_LOGI(TAG, "accent now %06x", (unsigned)st.accent_rgb);
    } else if (strcmp(line, "id") == 0) {
        ESP_LOGI(TAG, "%s", mesh_debug_make_identity() ? "dev identity created" : "identity already present");
    } else if (strcmp(line, "forget") == 0) {
        mesh_forget_all_friends();
        ESP_LOGI(TAG, "friends cleared on this terminal");
    } else if (line[0] != '\0') {
        ESP_LOGW(TAG, "unknown command '%s'", line);
    }
}

static void console_task(void *arg) {
    (void)arg;
    log_heap(); // the baseline, before the console's driver takes its own share
    char line[LINE_MAX_LEN];
    size_t n = 0;
    TickType_t heap_at = xTaskGetTickCount();
    for (;;) {
        // Polled straight from the peripheral: the driver's IRAM ISR cost ~3.3 KB of heap, which
        // is more than the boot margin (see log_heap at start).
        uint8_t ch;
        if (usb_serial_jtag_ll_rxfifo_data_available() && usb_serial_jtag_ll_read_rxfifo(&ch, 1) == 1) {
            if (ch == '\n' || ch == '\r') {
                line[n] = '\0';
                run_command(line);
                n = 0;
            } else if (n < sizeof(line) - 1) {
                line[n++] = (char)ch;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        sim_tick();
        if (xTaskGetTickCount() - heap_at >= pdMS_TO_TICKS(HEAP_LOG_PERIOD_MS)) {
            heap_at = xTaskGetTickCount();
            log_heap();
        }
    }
}

// Boot heap is nearly exhausted: creating the task here (its stack is allocated up front) made the
// display's DMA buffers fail with NO_MEM. So the task is created after gui_task has allocated them.
static void spawn_console(void *arg) {
    (void)arg;
    xTaskCreate(console_task, "mesh_console", 3072, NULL, 2, NULL);
}

void mesh_console_start(void) {
    static const esp_timer_create_args_t args = {.callback = spawn_console, .name = "meshcon"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_once(t, 5000000);
}

#endif
