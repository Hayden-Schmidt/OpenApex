# T01 — Repo cleanup: stray files, root Gradle wrapper, retire `simulator/`

Read `00_CONTEXT.md` first.

## First: commit the pack

This pack (`docs/restructure/`) and `docs/RiderMesh_Phase2_Plan.md` may still be untracked. If so,
commit them first on `chore/restructure`, as `Restructure: add task pack and rider mesh plan`.

## 1. Stray images

`maneuver_icon_debug.png` and `maneuver_icon_debug2.png` at the repo root are tracked, and nothing
references them. Confirm with `git grep maneuver_icon_debug`, then `git rm` both.

## 2. Root Gradle wrapper

There are two wrappers on different Gradle versions:

- **Root:** `gradlew`, `gradlew.bat`, `gradle/wrapper/*`. Only the VS Code task "Android Auto:
  assembleDebug" uses it (`.vscode/tasks.json`, `${workspaceFolder}/gradlew.bat -p phone …`).
- **`phone/`:** `phone/gradlew`, `phone/gradlew.bat`, `phone/gradle/wrapper/*`. This is the one the
  docs use (`docs/DEVELOPMENT_SETUP.md`: `.\phone\gradlew.bat -p phone …`).

Steps:

1. Keep `phone/`'s wrapper.
2. Point the VS Code task at `${workspaceFolder}/phone/gradlew.bat`, keeping `-p phone`, or set
   `cwd` to `phone` and drop `-p`. Either works; be consistent with DEVELOPMENT_SETUP.
3. `git rm` the root wrapper files.
4. `git grep -n "gradlew"` and fix any other reference to the root wrapper.

## 3. Retire `simulator/` and the root `platformio.ini`

`simulator/` holds the original browser mock (`index.html`) and a native countdown stub
(`native/main.cpp`, `native/countdown_bridge.cpp`). The root `platformio.ini` exists only to build
that stub (`src_dir = simulator/native`, env `native`). `firmware/sim_lvgl` has replaced both as the
GUI simulator, and `scripts/build-countdown.ps1` runs the countdown host tests. The owner confirmed
retiring it.

Remove:

- `simulator/` (`git rm -r`)
- the root `platformio.ini`
- the `.gitignore` line `simulator/native/CMakeLists.txt`. Leave `/CMakeLists.txt` and its comment
  alone if they still apply to anything; if they only existed for the root PlatformIO project,
  remove them too and say so in the report.

Update every reference. Known ones:

| File | What to change |
|---|---|
| `.vscode/tasks.json` | Delete tasks "Simulator: open round display" and "Simulator: PlatformIO native". Optionally add "Simulator: LVGL" running `pio run -d firmware/sim_lvgl` |
| `README.md` "Hardware-free development" | Remove the `simulator/index.html`, `Simulator: open round display` and `pio run -e native` bullets, and the paragraph saying the native target and browser simulator share the countdown. Keep the `sim_lvgl` bullet and mention `scripts/build-countdown.ps1` for host tests |
| `firmware/README.md` (around line 23) | Replace the "use `../simulator/index.html` … `native` environment" sentence with `sim_lvgl` + host tests |
| `firmware/platformio.ini` header comment | It explains why it is separate from the root `platformio.ini`. Reword: the firmware project stands alone now |
| `docs/DEVELOPMENT_SETUP.md` | Layout bullet `simulator/`; the "Hardware-free screen development" section's browser/native paragraphs; the VS Code task list; "Verified working" bullet `pio run -e native` |

Then run `git grep -n -E "simulator/|-e native|Simulator: "`. Every hit must be a `sim_lvgl`
mention, or in `docs/OpenApex_SPEC.md` (left for T05-T14), or in this pack. Record any SPEC hits
in Carry-over notes.

## Done when

- `git grep maneuver_icon_debug` is empty (outside this pack).
- There is no root `gradlew*` and no `gradle/`. `cd phone; .\gradlew.bat :shared:jvmTest
  :androidApp:assembleDebug` passes. The edited VS Code task's command line is correct (read it
  back).
- `simulator/` and the root `platformio.ini` are gone, and the grep above is clean.
- `pio run -d firmware/sim_lvgl` and `powershell -NoProfile -File scripts/build-countdown.ps1`
  still pass.
- `00_CONTEXT.md` is updated and one commit is made.

## Out of scope

Stale content in README / DEVELOPMENT_SETUP beyond the simulator lines, such as "touch or
non-touch variant", `gui_screens.c`, or the 2026-09-21 "Verified working" figures. Log them as
carry-over; T15 rewrites those files.
