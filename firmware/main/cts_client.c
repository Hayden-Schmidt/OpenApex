#include "cts_client.h"

// Howard Hinnant's days_from_civil: exact, branch-light, and no <time.h> -- mktime() would drag
// in the host's timezone database and give a different answer on the terminal than in a test.
int32_t cts_days_from_civil(int year, int month, int day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);                            // 0..399
    const unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                   // 0..146096
    return (int32_t)era * 146097 + (int32_t)doe - 719468;
}

bool cts_parse_current_time(const uint8_t *data, size_t len, uint32_t *local_epoch_s) {
    if (data == NULL || local_epoch_s == NULL || len < CTS_CURRENT_TIME_LEN) {
        return false;
    }
    // [0..1] year u16 LE, [2] month 1-12, [3] day 1-31, [4] hours, [5] minutes, [6] seconds,
    // [7] day of week, [8] fractions256, [9] adjust reason. We use the date and time only.
    const int year = (int)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
    const int month = data[2];
    const int day = data[3];
    const int hour = data[4];
    const int minute = data[5];
    const int second = data[6];

    // "Unknown" in CTS is a zero year/month/day. Reject it rather than render 1970 or a year 0
    // clock: a missing field must never become a fabricated value.
    if (year < 2000 || year > 2200 || month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    if (hour > 23 || minute > 59 || second > 59) {
        return false;
    }

    const int32_t days = cts_days_from_civil(year, month, day);
    if (days < 0) {
        return false;
    }
    *local_epoch_s =
        (uint32_t)days * 86400u + (uint32_t)hour * 3600u + (uint32_t)minute * 60u + (uint32_t)second;
    return true;
}
