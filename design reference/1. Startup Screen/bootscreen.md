# Page 1 — Startup / Boot Screen

## Requirements

- Display for at least as long as the app takes to load and background processes to start —
  **minimum 2 seconds**.
- **Simple:** static screen with a logo. *Maybe* a fade-in if the C3 can handle it.
- **Rich:** logo animation (logo TBC — defer).

## Reference

Beeline Moto 2 boot screen: plain black with a simple white logo — high contrast, easy to see.



**Not yet implemented:**

- No `SplashScreen` class exists (`firmware/gui/` has only the `gui_app.cpp/.hpp` seam).
- `gui_app_init()` (`firmware/gui/gui_app.cpp`) just creates a black screen — there is no boot
  sequence, no 2-second minimum hold, and no logo asset.
- No logo asset chosen yet (use cat-svgrepo-com.svg as a placeholder).

**Blocking:** the shared `Screen` base + `GuiTheme` interface (§16.6 Layer 1) must be written
before any `SplashScreen` can subclass it., so boot on C3 is effectively an instant transition into the idle dial. 

Change from other docs, C3 WILL now have boot screen same as S3, we can bake this into the hardware as a boot screen hopefully, otherwise, sofftware driven. 


