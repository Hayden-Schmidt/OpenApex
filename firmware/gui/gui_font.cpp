#include "gui_font.hpp"

// Considers every size LVGL ships, guarded so only the ones actually enabled compile in. The idle
// screen carried a hand-written subset of this list, which silently rounded 38px type down to 28
// because 38 simply was not in its table -- the point of enumerating all of them here is that
// enabling a size in lv_conf.h/sdkconfig is then the ONLY step needed to make it available.
const lv_font_t *montserrat_at_most(int32_t px) {
    const lv_font_t *best = LV_FONT_DEFAULT;
    int32_t best_px = 0;

#define GUI_FONT_TRY(n)                                       \
    if (LV_FONT_MONTSERRAT_##n && (n) <= px && (n) > best_px) { \
        best = &lv_font_montserrat_##n;                       \
        best_px = (n);                                        \
    }

#if LV_FONT_MONTSERRAT_8
    GUI_FONT_TRY(8)
#endif
#if LV_FONT_MONTSERRAT_10
    GUI_FONT_TRY(10)
#endif
#if LV_FONT_MONTSERRAT_12
    GUI_FONT_TRY(12)
#endif
#if LV_FONT_MONTSERRAT_14
    GUI_FONT_TRY(14)
#endif
#if LV_FONT_MONTSERRAT_16
    GUI_FONT_TRY(16)
#endif
#if LV_FONT_MONTSERRAT_18
    GUI_FONT_TRY(18)
#endif
#if LV_FONT_MONTSERRAT_20
    GUI_FONT_TRY(20)
#endif
#if LV_FONT_MONTSERRAT_22
    GUI_FONT_TRY(22)
#endif
#if LV_FONT_MONTSERRAT_24
    GUI_FONT_TRY(24)
#endif
#if LV_FONT_MONTSERRAT_26
    GUI_FONT_TRY(26)
#endif
#if LV_FONT_MONTSERRAT_28
    GUI_FONT_TRY(28)
#endif
#if LV_FONT_MONTSERRAT_30
    GUI_FONT_TRY(30)
#endif
#if LV_FONT_MONTSERRAT_32
    GUI_FONT_TRY(32)
#endif
#if LV_FONT_MONTSERRAT_34
    GUI_FONT_TRY(34)
#endif
#if LV_FONT_MONTSERRAT_36
    GUI_FONT_TRY(36)
#endif
#if LV_FONT_MONTSERRAT_38
    GUI_FONT_TRY(38)
#endif
#if LV_FONT_MONTSERRAT_40
    GUI_FONT_TRY(40)
#endif
#if LV_FONT_MONTSERRAT_42
    GUI_FONT_TRY(42)
#endif
#if LV_FONT_MONTSERRAT_44
    GUI_FONT_TRY(44)
#endif
#if LV_FONT_MONTSERRAT_46
    GUI_FONT_TRY(46)
#endif
#if LV_FONT_MONTSERRAT_48
    GUI_FONT_TRY(48)
#endif

#undef GUI_FONT_TRY
    return best;
}
