#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "packet.h"

#ifdef __cplusplus
extern "C" {
#endif

// One decoded packet, assembled from producers that each only know half of it.
//
// On Android there is one producer and this is a passthrough: the relay's v5 packet carries nav
// and telemetry together, so every accept overwrites everything and the output is the input.
//
// On iOS there are two, and neither can be asked to carry the other's fields:
//   ancs_bridge  -> nav text off the phone's notification centre, no telemetry at all
//   the iOS app  -> v6 telemetry-only writes, no nav (ANCS owns it, SPEC 5.2)
// Applying either one alone to the pipeline would blank the other -- a telemetry write would end
// the route, an ANCS write would drop the speed to unknown. So each half is held and re-stamped
// independently, and only the half a packet speaks for is overwritten.
//
// A held half is not held forever. When its producer goes quiet past the hold below, the fields
// revert to their sentinels rather than freezing: a killed iOS app must show an unknown speed,
// not the last one it managed to send (SPEC 3). For nav that revert is also what returns the
// terminal to idle, since it clears the fields the pipeline's nav gate reads.
//
// Pure C, no FreeRTOS -- host-tested in test_host/packet_merge_test.c.

// Matches countdown.c's MAX_HOLD_MS. The display already calls a route stale after this long
// without a packet; holding the fields that describe it any longer would only mean the nav page
// outlives the staleness rule that is meant to govern it.
#define PACKET_MERGE_NAV_HOLD_MS 10000U

// The telemetry producer is the app's publish loop (~1-3 Hz on Android, the same on iOS). Five
// seconds is four missed publishes -- long enough to ride out a scheduling hiccup, short enough
// that a backgrounded-and-killed iOS app blanks the speed rather than leaving a plausible lie on
// the dial. It is also the fix-staleness window the relay itself already uses.
#define PACKET_MERGE_TELEMETRY_HOLD_MS 5000U

void packet_merge_reset(void);

// Folds `in` into the held state, overwriting only the groups in in->source_groups, and writes
// the result to `out`. Returns false (leaving `out` untouched) only if a pointer is NULL or the
// packet claims no groups at all.
bool packet_merge_accept(const raw_notif_t *in, uint32_t now_ms, raw_notif_t *out);

// True while the given group is within its hold. Exposed for the console dump and the tests.
bool packet_merge_group_fresh(raw_group_t group, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
