#pragma once

#include <cstdint>
#include <cstdio>

#include "settings.h"

// Distance presentation in the rider's unit (settings.h). The model stays metric throughout; only
// what is drawn changes.

inline bool units_imperial() { return settings_get()->units == SETTINGS_UNITS_IMPERIAL; }

// Odometer distance in thousandths of the long unit: metres for km, thousandths of a mile for mi,
// so existing km formatting ("/ 1000", "tenths") works unchanged for either.
inline uint32_t units_odometer_milli(uint32_t meters) {
    return units_imperial() ? static_cast<uint32_t>(static_cast<uint64_t>(meters) * 1000u / 1609u)
                            : meters;
}

inline const char *units_long_label() { return units_imperial() ? "mi" : "km"; }

// Distance to the next maneuver: "250 m" metric (as before), or feet to 1,000 ft in 10 ft steps
// then tenths of a mile -- how imperial satnavs count down.
inline void units_format_maneuver(char *out, size_t out_len, uint32_t meters) {
    if (!units_imperial()) {
        std::snprintf(out, out_len, "%u m", static_cast<unsigned>(meters));
        return;
    }
    const uint32_t feet = static_cast<uint32_t>(static_cast<uint64_t>(meters) * 3281u / 1000u);
    if (feet < 1000u) {
        std::snprintf(out, out_len, "%u ft", static_cast<unsigned>((feet + 5u) / 10u * 10u));
        return;
    }
    const uint32_t tenths = (meters * 10u + 804u) / 1609u;
    std::snprintf(out, out_len, "%u.%u mi", static_cast<unsigned>(tenths / 10u),
                  static_cast<unsigned>(tenths % 10u));
}
