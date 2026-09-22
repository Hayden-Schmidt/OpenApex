# Page 1 — Startup / Boot Screen

## Requirements

- Display for at least as long as the app takes to load and background processes to start —
  **minimum 2 seconds**.
- **Simple:** static screen with a logo. *Maybe* a fade-in if the C3 can handle it.
- **Rich:** logo animation (logo TBC — defer).

## Reference

Beeline Moto 2 boot screen: plain black with a simple white logo — high contrast, easy to see.

## Current code status: ⬜ not started

The splash is specified as a **Layer 2, capability-gated screen** in `docs/OpenApex_SPEC.md`
§16.6: `SplashScreen : Screen`, compiled only when `BOARD_HAS_SPLASH` is set.

| Board | `BOARD_HAS_SPLASH` | Status |
|-------|--------------------|--------|
| C3 (`PROTOTYPE_C3_GC9A01`) | 0 | not compiled |
| S3 (`S3_AMOLED_175`) | 1 | compiles-in once `SplashScreen` is written |

**Not yet implemented:**
- No `SplashScreen` class exists (`firmware/gui/` has only the `gui_app.cpp/.hpp` seam).
- `gui_app_init()` (`firmware/gui/gui_app.cpp`) just creates a black screen — there is no boot
  sequence, no 2-second minimum hold, and no logo asset.
- No logo asset chosen yet (deferred by design).

**Blocking:** the shared `Screen` base + `GuiTheme` interface (§16.6 Layer 1) must be written
before any `SplashScreen` can subclass it. The C3 profile will never show a splash (its
`BOARD_HAS_SPLASH = 0`), so boot on C3 is effectively an instant transition into the idle dial. 