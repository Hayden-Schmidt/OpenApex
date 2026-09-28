#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Current Time Service (Bluetooth SIG 0x1805), the iOS side of the terminal's clock.
//
// The C3 has no RTC, so on Android the wall clock rides along in the nav packet (v4, epoch_s +
// tz_offset_min). On iOS there is no packet when the companion app is asleep -- which iOS will
// make it be -- so the clock comes from the phone's own CTS instead, read once on connect and
// then by notification whenever the phone's time changes. That keeps the idle screen's clock
// correct with no app running at all, which is the same property the ANCS nav path has.
//
// This file is the parsing half: pure, no NimBLE, host-tested. apple_client.c does the GATT.

#define CTS_SERVICE_UUID16 0x1805u
#define CTS_CHR_CURRENT_TIME_UUID16 0x2A2Bu

// Exact-Time-256 plus the adjust-reason byte: the Current Time characteristic value.
#define CTS_CURRENT_TIME_LEN 10u

// Parses a Current Time value into seconds since the Unix epoch.
//
// CTS carries LOCAL wall-clock time with no zone, so what comes out is a "local epoch": the count
// that pipeline.c wants when paired with a zero tz offset. There is deliberately no attempt to
// recover a real UTC instant -- the terminal only ever displays HH:MM, and inventing a zone to
// subtract would be a fabricated value.
//
// Returns false on a short buffer or an out-of-range field. A rejected value leaves the clock
// showing nothing rather than a wrong time: epoch 0 is a zero-filled field, not 1970.
bool cts_parse_current_time(const uint8_t *data, size_t len, uint32_t *local_epoch_s);

// Days from 1970-01-01 to y-m-d, proleptic Gregorian. Exposed for the host tests.
int32_t cts_days_from_civil(int year, int month, int day);

#ifdef __cplusplus
}
#endif
