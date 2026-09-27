# OpenApex: Design System (Phone App)

OpenApex is a round, high-contrast handlebar display for motorcycles. The phone app is its companion: it pairs with the terminal, streams GNSS/motion to it, pushes settings, and updates firmware. The app must look like **the same product as the terminal**: true black, one bright accent, big clean numerals, calm confident motion.

Token source of truth: `openapex.tokens.json` (extracted from `firmware/gui/`). If this file and the tokens disagree, the tokens win.

## 1. Principles
1. **Contrast first.** It has to be readable in direct sun, with gloves on, at a glance. White on true black. No low-contrast grey-on-grey for anything the user has to read.
2. **One accent.** `accent` is the only brand colour, and users can re-theme it. Always use it as a variable, never hard-code the hex. Design every screen so it still works if the accent is purple or green.
3. **Semantic colour never follows the theme.** Success, north, traffic and error keep their meaning whatever the accent is.
4. **The terminal is the hero.** The live round-display preview is the centrepiece of the home screen. The phone UI frames it; it doesn't compete with it.
5. **Quiet chrome, loud data.** Numbers and state are large. Labels, borders and navigation are small and secondary.
6. **Motion has meaning.** Animate to show state change, cause and effect, or continuity. Never animate for decoration.

## 2. Colour
| Token | Hex | Use |
|---|---|---|
| `surface.background` | `#000000` | App background (true black, dark-only) |
| `surface.raised` | `#0F0F10` | Cards, sheets (phone-only addition) |
| `surface.raisedHigh` | `#1A1A1A` | Pressed / selected card, input fields |
| `surface.outline` | `#2A2A2C` | Hairline dividers, card borders (phone-only addition) |
| `brand.accent` | `#00FFE8` | Primary actions, active state, progress, focus |
| `text.primary` | `#FFFFFF` | Values, titles, body |
| `text.secondary` | `#969696` | Labels, helper text |
| `text.disabled` | `#808080` | Stale / unavailable data |
| `text.icon` | `#E3E3E3` | Icons |
| `semantic.success` | `#1E8D3E` | Connected-and-healthy, update complete |
| `semantic.warning` | `#F5A623` | Degraded (weak signal, permission missing) |
| `semantic.error` | `#E0392B` | Failed, disconnected with error |
| `semantic.north` | `#FF3B30` | Compass north only |
| `traffic.free/slow/heavy/stopped` | `#009AA6` `#F5A623` `#E0392B` `#8B1A12` | Traffic ring only |

Rules: accent text on black is fine; for text *on* accent fills, use black `#000000`. No gradients except a subtle accent glow behind the terminal preview. Light mode isn't planned.

## 3. Typography
Family: **Montserrat** (matches the terminal). Use tabular figures for anything that changes live.

| Role | Size / line | Weight | Use |
|---|---|---|---|
| Display | 56 / 60 | 500 | Hero value (speed, odometer, update %) |
| Headline | 32 / 38 | 500 | Screen titles |
| Title | 22 / 28 | 600 | Card titles, section headers |
| Body | 16 / 24 | 400 | Content |
| Label | 14 / 20 | 500 | Buttons, list labels, values |
| Caption | 12 / 16 | 400 | Helper text, timestamps, units |

The terminal uses Regular only, at 14–44 px on a 240 px screen. On the phone, keep the same *feel*: large, sparse, numeric. Numerals are the hero; units sit beside them at Caption size in `text.secondary`.

## 4. Space, shape, elevation
- **Spacing scale:** 4, 8, 12, 16, 24, 32, 48. Screen side margin is 20. Card padding is 16.
- **Radius:** 4 (small chips, odometer-style boxes, as on the terminal), 16 (cards, sheets), pill (buttons, status bubble, as the terminal's odometer bubble).
- **Stroke:** 1 hairline borders, 2 outlined controls, 7 progress rings (the terminal's trip arc).
- **Elevation:** no drop shadows. Separate layers with `surface.raised` plus an `outline` hairline.
- **Circles are the brand motif.** Rings, dials and round previews echo the round display. Use them for progress, status and the device avatar.

## 5. Motion
From `../Screen Transitions.md` and firmware `ease.hpp`:
- **Never linear.** Use an S-curve (`standard`, cubic-bezier(0.65,0,0.35,1)) or exponential (`enter` cubic-bezier(0.16,1,0.3,1), `exit` cubic-bezier(0.7,0,0.84,0)).
- **Exit = entry reversed.** Whatever slides in slides back out the way it came.
- **Minimum holds are floors, not delays.** Never make the user wait for an animation to reveal data that's already ready.
- **Live data is never frozen behind a tween.** State (connected, %, speed) is accurate from the first frame of a transition.

| Token | ms | Use |
|---|---|---|
| fast | 400 | Fades, icon swaps, small slides |
| medium | 600 | Text/content slide-in, sheet open |
| slow | 900 | Hero element entry (terminal preview, device card expand) |
| tween | 1000 | State change on the hero (connecting → connected) |
| reveal | 2000 | Celebratory reveal (update complete) |

Phone additions: spring physics for anything gesture-driven (drag, swipe, pull), with a critically damped feel and no bounce. Button press is a 0.97 scale over 100 ms.

Signature moments:
1. **Connect:** the terminal preview ring draws around in accent, then the status bubble rises from below it. This mirrors the terminal's idle connect animation.
2. **Device card → detail:** shared-element expand; the round avatar grows into the hero preview.
3. **Firmware update:** a thick 7-stroke accent ring fills with the percentage in Display type at its centre. On completion it plays a green success ring closing inward (the terminal's arrived reveal).

## 6. Components
- **Terminal preview:** a round frame mirroring what the terminal shows now (idle / turn-by-turn / odometer). Black disc, thin outline ring, accent glow when connected, desaturated when stale.
- **Status bubble:** a pill showing link state: icon + label. Accent fill when connected, `raisedHigh` fill when searching, error colour on failure.
- **Card:** `surface.raised`, radius 16, 1 px outline, 16 padding. Title + value pairs inside.
- **Primary button:** pill, accent fill, black label, height 52.
- **Secondary button:** pill, 2 px accent outline, accent label.
- **Tertiary button:** plain accent text.
- **List row:** 56 tall, leading icon (24, `text.icon`), label, trailing value or chevron in `text.secondary`.
- **Toggle / segmented:** accent when on; `raisedHigh` track when off.
- **Progress ring:** 7 stroke, `neutral #585F68` track, accent fill, round caps.
- **Icons:** Material Symbols Outlined, weight 400, 24 px (the terminal's source set).

## 7. Screens (v1)
1. **Home:** large terminal preview at the top, status bubble under it, then cards: telemetry health (GNSS fix, heading, stream rate), terminal battery/firmware, quick actions.
2. **Devices:** known terminals as cards with a round avatar and name/state. "Add terminal" opens the scan sheet. Detail view covers rename, forget and info.
3. **Terminal settings:** grouped list (Display, Units, Theme colour, Elements). Theme colour shows a live preview on the terminal ring. There's a sync indicator for "pushed to device / pending".
4. **Firmware update:** current vs available version, release notes, primary "Update" button, then a full-screen progress ring.
5. **Setup / permissions:** a stepped onboarding flow with one permission per step, a clear why, and a single action (Bluetooth, location, notification access on Android, battery exemptions).
6. **Diagnostics:** packet log, recorder toggle, export. Dense but still high contrast.

## 8. Don't
- Don't use a light theme, stock Material purple, or default platform components left unstyled.
- Don't use yellow as the accent.
- Don't use more than one accent-filled primary action per screen.
- Don't put decorative motion or looping animation on data screens.
- Don't use grey text for critical values.
- Don't use drop shadows or glassmorphism as a crutch for hierarchy.
- Don't draw square device imagery. The terminal is round.
