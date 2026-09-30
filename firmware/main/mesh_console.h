#pragma once

// Mesh serial console -- DEVELOPMENT ONLY.
// ----------------------------------------
// Drives a screenless terminal over USB-Serial-JTAG: one command per line, one log line back. It
// stands in for the terminal UI (docs/blueprints/04_rider_mesh.md, U-table) so the radio side of
// each flow can be exercised on a board with no screen, and it logs free heap for the C3 baseline.
//
// GATED OFF BY DEFAULT: compiles to a no-op unless OPENAPEX_MESH_CONSOLE=1 is defined, which only
// the `prototype_c3_meshdev` PlatformIO env does. Never ship that env.

#ifdef __cplusplus
extern "C" {
#endif

#if defined(OPENAPEX_MESH_CONSOLE) && OPENAPEX_MESH_CONSOLE
#include "esp_heap_caps.h"
#include "esp_log.h"
#define HEAPMARK(l) ESP_LOGI("heapmark", "%-16s free=%u", l, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL))
#else
#define HEAPMARK(l) ((void)0)
#endif

#if defined(OPENAPEX_MESH_CONSOLE) && OPENAPEX_MESH_CONSOLE
void mesh_console_start(void);
#else
static inline void mesh_console_start(void) {}
#endif

#ifdef __cplusplus
}
#endif
