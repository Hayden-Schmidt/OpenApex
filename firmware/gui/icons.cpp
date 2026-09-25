// The one translation unit that instantiates the generated icon pixel data. Every other file goes
// through icons.hpp's accessors, so the A8 payload exists exactly once in the binary.
#define ICON_DATA_IMPL
#include "icons.hpp"

const lv_image_dsc_t *icon_image(icon_id_t id) {
    if (id < 0 || id >= ICON_COUNT) {
        return nullptr;
    }
    return &ICON_DESC[id];
}

const char *icon_name(icon_id_t id) {
    if (id < 0 || id >= ICON_COUNT) {
        return "?";
    }
    return ICON_NAMES[id];
}
