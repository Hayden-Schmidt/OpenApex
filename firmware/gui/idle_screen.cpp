#include "idle_screen.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ease.hpp"
#include "gui_app.hpp"
#include "gui_font.hpp"
#include "icons.hpp"
#include "theme.hpp"

namespace {

// Geometry lifted from "design reference/2. Idle Screen/Idle Screen Ref.svg", expressed in that
// file's 240px reference frame and scaled at runtime -- same convention as the icon manifest's
// size_240 and nav_renderer.cpp's 600-unit design diameter. The SVG draws both link states side by
// side in one 520x240 canvas; the numbers below are its coordinates translated so each circle's
// centre is at (120, 120).
constexpr float kBubbleW = 111.0f;
constexpr float kBubbleH = 30.0f;

constexpr float kBubbleCyDisconnected = 122.0f;
constexpr float kBubbleCyConnected = 141.0f;
constexpr float kLinkCyDisconnected = 155.75f;
constexpr float kLinkCyConnected = 173.5f;

// Final (connected) resting places of the two widgets that rise out from behind the bubble.
constexpr float kBatteryCy = 68.5f;
constexpr float kClockCy = 100.7f;

// How far below their final positions the clock/battery group starts. Must be deep enough that the
// group's topmost edge (battery top, kBatteryCy - half the 24px icon) is still below the bubble's
// *disconnected* top edge, or the battery peeks out before the animation starts.
constexpr float kRiseTravel = 52.0f;

constexpr uint32_t kTweenMs = 1000;

// Page entry ("design reference/Screen Transitions.md"): the bubble rises onto the page from below
// the bottom edge, then the link glyph slides out from behind it. Leaving plays it all in reverse.
// The bubble arrives fast and settles (exponential ease-out) and leaves as the exact reverse
// (ease-in: slow to start, then away).
constexpr uint32_t kBubbleInMs = 470;
constexpr uint32_t kLinkInMs = 400;

// Text sizes measured off the SVG's glyph bounds: the clock's digits are 30.5px tall (~44px
// Montserrat) and the odometer's are 13.3px (~20px).
constexpr float kClockFontRef = 44.0f;
constexpr float kOdometerFontRef = 20.0f;

// The page spec suggested one battery outline plus a drawn level bar, to avoid "wasting" memory on
// the whole battery_android_0..6 set. Measured, that trade does not pay: the eight 24px A8 masks
// are 4.6kB of flash on the C3 (0.1% of a 4MB part), while the drawn bar needs four hand-measured
// interior coordinates that silently go wrong whenever the source icon is re-exported -- and at
// 240px the icon's interior is only ~10px wide, so a "continuous" bar has no more visible steps
// than the eight rasters do. Using the rasters instead.
const icon_id_t kBatteryIcons[] = {
    ICON_BATTERY_ANDROID_0, ICON_BATTERY_ANDROID_1, ICON_BATTERY_ANDROID_2,
    ICON_BATTERY_ANDROID_3, ICON_BATTERY_ANDROID_4, ICON_BATTERY_ANDROID_5,
    ICON_BATTERY_ANDROID_6, ICON_BATTERY_ANDROID_FULL,
};
constexpr int kBatteryLevels = static_cast<int>(sizeof(kBatteryIcons) / sizeof(kBatteryIcons[0]));

// Rounds to the nearest bar rather than truncating, so 87% reads as nearly-full rather than
// dropping a whole bar. 0xFF (unknown, view_state.h) is handled by the caller.
icon_id_t battery_icon_for(uint8_t percent) {
    const int pct = percent > 100 ? 100 : percent;
    int level = (pct * (kBatteryLevels - 1) + 50) / 100;
    if (level < 0) level = 0;
    if (level >= kBatteryLevels) level = kBatteryLevels - 1;
    return kBatteryIcons[level];
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

// "3,500 km", matching the Figma reference. Grouping is done by hand: printf's %'u is a
// glibc/locale extension that newlib on the C3 does not provide.
// TODO(backend): unit choice (km vs mi) belongs in the runtime config store, which does not exist
// yet -- the reference is metric, so that is what ships for now.
void format_odometer(char *out, size_t out_len, uint32_t meters) {
    const uint32_t km = (meters + 500u) / 1000u;
    char digits[12];
    const int n = std::snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(km));
    size_t w = 0;
    for (int i = 0; i < n && w + 5 < out_len; ++i) {
        if (i > 0 && ((n - i) % 3) == 0) {
            out[w++] = ',';
        }
        out[w++] = digits[i];
    }
    std::snprintf(out + w, out_len - w, " km");
}

// Centres an object horizontally in its parent and puts its top edge at `top`.
void place_centred(lv_obj_t *obj, int32_t parent_w, int32_t top) {
    lv_obj_set_pos(obj, (parent_w - lv_obj_get_width(obj)) / 2, top);
}

} // namespace

int32_t IdleScreen::px(float ref_240) const {
    return static_cast<int32_t>(std::lround(ref_240 * scale_));
}

// Sizes off the live lv_display rather than BOARD_DISP_WIDTH, same as DialScreen -- one binary has
// to render correctly at whatever resolution the display was created at.
IdleScreen::IdleScreen() {
    width_ = lv_display_get_horizontal_resolution(lv_display_get_default());
    scale_ = static_cast<float>(width_) / 240.0f;

    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(root_, false);

    const lv_color_t accent = gui_theme().palette();

    // --- Clip window for the rising clock/battery group. An lv_obj clips its children to its own
    // area unless LV_OBJ_FLAG_OVERFLOW_VISIBLE is set, which is exactly the mask the spec asks for
    // ("only shown above the top line of the bubble"); shrinking its height in apply_layout() makes
    // the mask travel with the bubble for free.
    reveal_ = lv_obj_create(root_);
    lv_obj_remove_style_all(reveal_);
    lv_obj_set_width(reveal_, width_);
    lv_obj_set_pos(reveal_, 0, 0);
    lv_obj_set_scrollable(reveal_, false);

    battery_icon_ = lv_image_create(reveal_);
    lv_image_set_src(battery_icon_, icon_image(battery_icon_for(0)));
    lv_obj_set_style_image_recolor(battery_icon_, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(battery_icon_, LV_OPA_COVER, 0);

    clock_label_ = lv_label_create(reveal_);
    lv_obj_set_style_text_color(clock_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(clock_label_, montserrat_at_most(px(kClockFontRef)), 0);
    lv_label_set_text(clock_label_, "--:--");

    // --- Odometer bubble.
    bubble_ = lv_obj_create(root_);
    lv_obj_remove_style_all(bubble_);
    lv_obj_set_size(bubble_, px(kBubbleW), px(kBubbleH));
    lv_obj_set_style_bg_color(bubble_, accent, 0);
    lv_obj_set_style_bg_opa(bubble_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_scrollable(bubble_, false);

    odometer_label_ = lv_label_create(bubble_);
    lv_obj_set_style_text_color(odometer_label_, lv_color_black(), 0);
    lv_obj_set_style_text_font(odometer_label_, montserrat_at_most(px(kOdometerFontRef)), 0);
    lv_label_set_text(odometer_label_, "0 km");
    lv_obj_center(odometer_label_);

    // --- Phone link status. Sent to the back so the bubble covers it: that stacking is the mask the
    // entry needs for the glyph to emerge from *under* the bubble rather than over it.
    link_icon_ = lv_image_create(root_);
    lv_image_set_src(link_icon_, icon_image(ICON_BLUETOOTH_DISABLED));
    lv_obj_set_style_image_recolor(link_icon_, accent, 0);
    lv_obj_set_style_image_recolor_opa(link_icon_, LV_OPA_COVER, 0);
    lv_obj_move_background(link_icon_);

    lv_obj_update_layout(root_);
    apply_layout();
}

void IdleScreen::set_progress(void *var, int32_t value) {
    auto *self = static_cast<IdleScreen *>(var);
    self->progress_ = value;
    self->apply_layout();
}

void IdleScreen::set_bubble_in(void *var, int32_t value) {
    auto *self = static_cast<IdleScreen *>(var);
    self->bubble_in_ = value;
    self->apply_layout();
}

void IdleScreen::set_link_in(void *var, int32_t value) {
    auto *self = static_cast<IdleScreen *>(var);
    self->link_in_ = value;
    self->apply_layout();
}

IdleScreen *IdleScreen::from_anim(lv_anim_t *a) { return static_cast<IdleScreen *>(a->var); }

// Duration scales with the distance left to cover, so a reversal from part-way takes part of the
// time rather than crawling the last few pixels over the full duration.
void IdleScreen::tween(lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t full_ms,
                       lv_anim_completed_cb_t completed, lv_anim_path_cb_t path) {
    const int32_t span = to > from ? to - from : from - to;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, full_ms * static_cast<uint32_t>(span) / 1000u);
    lv_anim_set_path_cb(&a, path);  // default: the spec's "S curve"
    lv_anim_set_completed_cb(&a, completed);
    lv_anim_start(&a);
}

void IdleScreen::start_link_tween() {
    lv_anim_delete(this, set_progress);
    tween(set_progress, progress_, connected_ ? 1000 : 0, kTweenMs, nullptr);
}

void IdleScreen::enter() {
    lv_anim_delete(this, nullptr);
    leaving_ = false;
    landed_ = false;
    bubble_in_ = 0;
    link_in_ = 0;
    progress_ = 0;
    apply_layout();
    tween(set_bubble_in, 0, 1000, kBubbleInMs, [](lv_anim_t *a) {
        IdleScreen *self = from_anim(a);
        self->tween(set_link_in, 0, 1000, kLinkInMs, [](lv_anim_t *a2) {
            IdleScreen *self2 = from_anim(a2);
            // The connected layout only plays once bubble and glyph have fully landed -- even when
            // the phone connected while they were still on their way in.
            self2->landed_ = true;
            if (self2->connected_) self2->start_link_tween();
        });
    }, anim_path<ease_out_expo>);
}

bool IdleScreen::entered() const {
    return landed_ && lv_anim_get(const_cast<IdleScreen *>(this), set_progress) == nullptr &&
           progress_ == (connected_ ? 1000 : 0);
}

// Reverse of enter(): the connected text first, then the glyph back under the bubble, then the
// bubble off the bottom.
void IdleScreen::leave(LeaveDone done, void *ctx) {
    lv_anim_delete(this, nullptr);
    leaving_ = true;
    done_ = done;
    done_ctx_ = ctx;
    tween(set_progress, progress_, 0, kTweenMs, [](lv_anim_t *a) {
        IdleScreen *self = from_anim(a);
        self->tween(set_link_in, self->link_in_, 0, kLinkInMs, [](lv_anim_t *a2) {
            IdleScreen *self2 = from_anim(a2);
            self2->tween(set_bubble_in, self2->bubble_in_, 0, kBubbleInMs, [](lv_anim_t *a3) {
                IdleScreen *self3 = from_anim(a3);
                self3->done_(self3->done_ctx_);
            }, anim_path<ease_in_expo>);
        });
    });
}

void IdleScreen::apply_layout() {
    const float t = static_cast<float>(progress_) / 1000.0f;

    const int32_t bubble_h = px(kBubbleH);
    const int32_t rest_cy = px(lerp(kBubbleCyDisconnected, kBubbleCyConnected, t));
    const int32_t rest_top = rest_cy - bubble_h / 2;
    // Entry offset: at bubble_in_ == 0 the bubble's top edge sits on the bottom of the panel.
    const int32_t entry_dy = static_cast<int32_t>(
        std::lround((width_ - rest_top) * (1.0f - static_cast<float>(bubble_in_) / 1000.0f)));
    const int32_t bubble_cy = rest_cy + entry_dy;
    const int32_t bubble_top = rest_top + entry_dy;
    lv_obj_set_pos(bubble_, (width_ - px(kBubbleW)) / 2, bubble_top);

    // The clip window's bottom edge *is* the bubble's top edge, so the group is revealed exactly as
    // it clears the bubble. It tracks the *resting* edge, not the entry-offset one: while the bubble
    // is still rising in from below, following it down would uncover the hidden group.
    lv_obj_set_height(reveal_, rest_top > 0 ? rest_top : 0);

    const int32_t rise = px(lerp(kRiseTravel, 0.0f, t));

    const int32_t battery_h = lv_obj_get_height(battery_icon_);
    const int32_t battery_w = lv_obj_get_width(battery_icon_);
    lv_obj_set_pos(battery_icon_, (width_ - battery_w) / 2, px(kBatteryCy) - battery_h / 2 + rise);

    place_centred(clock_label_, width_, px(kClockCy) - lv_obj_get_height(clock_label_) / 2 + rise);

    // The glyph starts tucked behind the bubble's centre and slides out to its resting place.
    const int32_t rest_link_cy = px(lerp(kLinkCyDisconnected, kLinkCyConnected, t)) + entry_dy;
    const float link_t = static_cast<float>(link_in_) / 1000.0f;
    const int32_t link_cy = static_cast<int32_t>(std::lround(
        lerp(static_cast<float>(bubble_cy), static_cast<float>(rest_link_cy), link_t)));
    place_centred(link_icon_, width_, link_cy - lv_obj_get_height(link_icon_) / 2);
}

void IdleScreen::update(const terminal_view_state_t &state) {
    // The glyph always shows the live link state, whatever the page animation is doing; only the
    // layout change it triggers waits for the entry to land (and never plays while leaving).
    if (state.phone_connected != connected_) {
        connected_ = state.phone_connected;
        lv_image_set_src(link_icon_, icon_image(connected_ ? ICON_BLUETOOTH_CONNECTED
                                                           : ICON_BLUETOOTH_DISABLED));
        if (landed_ && !leaving_) start_link_tween();
    }

    if (state.odometer_meters != last_odometer_meters_) {
        last_odometer_meters_ = state.odometer_meters;
        char text[24];
        format_odometer(text, sizeof(text), state.odometer_meters);
        lv_label_set_text(odometer_label_, text);
    }

    // clock[0] == '\0' means no time source has reported yet (view_state.h) -- show placeholder
    // dashes rather than a fabricated time.
    lv_label_set_text(clock_label_, state.clock[0] != '\0' ? state.clock : "--:--");

    // 0xFF = unknown (view_state.h): hide the icon outright rather than implying an empty battery.
    if (state.battery_percent != last_battery_percent_) {
        last_battery_percent_ = state.battery_percent;
        const bool known = state.battery_percent != 0xFF;
        lv_obj_set_hidden(battery_icon_, !known);
        if (known) {
            lv_image_set_src(battery_icon_, icon_image(battery_icon_for(state.battery_percent)));
        }
    }
}
