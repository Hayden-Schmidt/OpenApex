#pragma once

// Compile-time board profile selection. Exactly one of these is set (via PlatformIO build flag or
// menuconfig) before board_profile.h is included anywhere.
//
//   #define BOARD_PROFILE_PROTOTYPE_C3_GC9A01 1
//   #define BOARD_PROFILE_S3_AMOLED_175 1
//
// Board-specific pins/drivers must never leak into the navigation model, packet decoder, or
// countdown engine. Profiles only select hardware providers; feature tasks compile out cleanly
// when their provider is disabled.
//
// GUI tier/capability flags (see docs/OpenApex_SPEC.md §16.6): every profile sets BOARD_GFX_TIER to
// exactly one of BOARD_GFX_TIER_BASIC or BOARD_GFX_TIER_RICH, selecting the GuiTheme implementation
// (icon fidelity, palette, state-change animation) used by the shared Layer 1 DialScreen. This is
// richness only -- it never changes which states exist or what they mean. BOARD_HAS_SPLASH and
// BOARD_HAS_MAP_RENDER gate whether the corresponding Layer 2 screen compiles in at all; they are
// independent of BOARD_GFX_TIER and of each other.
#define BOARD_GFX_TIER_BASIC 0
#define BOARD_GFX_TIER_RICH 1

#if defined(BOARD_PROFILE_PROTOTYPE_C3_GC9A01)

// Generic ESP32-C3 1.28-inch round 240x240 GC9A01 IPS LCD (AliExpress 1005006194839720).
// Single-core RISC-V. Touch and non-touch variants exist.
//
// Pin map below is corroborated by the Elecrow CrowPanel 1.28" round display (same board
// family) LovyanGFX/ESPHome configs — SCLK=6, MOSI=7, DC=2, CS=10, BL=3, I2C SDA=4/SCL=5,
// touch INT=0. Reset is typically left unconnected (software reset); backlight is GPIO3.
// STILL VERIFY on the delivered board before flashing — cheap AliExpress variants drift.

#define BOARD_HAS_TOUCH 0          // set 1 for the CST816 touch variant once confirmed
#define BOARD_HAS_GNSS 0           // no onboard GNSS in Phase 1
#define BOARD_HAS_IMU 0

// GUI: leanest target -- flat/instant dial only, no splash, no map-following.
#define BOARD_GFX_TIER BOARD_GFX_TIER_BASIC
#define BOARD_HAS_SPLASH 0
#define BOARD_HAS_MAP_RENDER 0

// Display: GC9A01 over SPI.
#define BOARD_DISP_SPI_HOST SPI2_HOST
#define BOARD_DISP_PIN_SCLK 6
#define BOARD_DISP_PIN_MOSI 7
#define BOARD_DISP_PIN_DC 2
#define BOARD_DISP_PIN_CS 10
#define BOARD_DISP_PIN_RST -1      // typically unconnected — software reset only
#define BOARD_DISP_PIN_BL 3        // backlight (LEDC/PWM)
#define BOARD_DISP_WIDTH 240
#define BOARD_DISP_HEIGHT 240

// Touch: CST816/CST816D over I2C (touch variant only). Compiles out when BOARD_HAS_TOUCH == 0.
#define BOARD_TOUCH_PIN_SDA 4
#define BOARD_TOUCH_PIN_SCL 5
#define BOARD_TOUCH_PIN_INT 0
#define BOARD_TOUCH_PIN_RST -1

#elif defined(BOARD_PROFILE_S3_AMOLED_175)

// Reference production target: Waveshare ESP32-S3-Touch-AMOLED-1.75 (see
// docs/OpenApex_SPEC.md §6.3). ESP32-S3R8, dual-core Xtensa LX7, 8MB PSRAM, 16MB flash.
// 1.75" circular AMOLED, 466x466, CO5300 driver over QSPI. CST9217 capacitive touch over I2C.
// QMI8658 6-axis IMU, PCF85063 RTC, AXP2101 PMIC. GPS variant adds an LC76G module (not modeled
// here -- separate BOARD_HAS_GNSS profile variant once the GPS board revision is in hand).
//
// STUB PROFILE: resolution/tier/capability flags only. No real SPI/QSPI/I2C pin assignments yet --
// this profile is not buildable against real S3 hardware until the CO5300/CST9217/QMI8658 drivers
// exist and pins are sourced from the Waveshare schematic. It exists now so firmware/gui and
// firmware/sim_lvgl can target the S3 tier/resolution ahead of the display driver work.

#define BOARD_HAS_TOUCH 1
#define BOARD_HAS_GNSS 0          // GPS variant not modeled by this profile yet
#define BOARD_HAS_IMU 1

// GUI: richest target -- animated splash, transitions, map-following once §5.4 exists.
#define BOARD_GFX_TIER BOARD_GFX_TIER_RICH
#define BOARD_HAS_SPLASH 1
#define BOARD_HAS_MAP_RENDER 0    // flips on once the polyline stream (§5.4) is implemented

// Display: CO5300 over QSPI. Pins not yet sourced -- see stub note above.
#define BOARD_DISP_WIDTH 466
#define BOARD_DISP_HEIGHT 466

#else
#error "No board profile selected. Define BOARD_PROFILE_PROTOTYPE_C3_GC9A01 or BOARD_PROFILE_S3_AMOLED_175 (or add a new profile)."
#endif
