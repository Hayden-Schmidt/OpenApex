#pragma once

// Compile-time board profile selection. Exactly one of these is set (via PlatformIO build flag or
// menuconfig) before board_profile.h is included anywhere.
//
//   #define BOARD_PROFILE_PROTOTYPE_C3_GC9A01 1
//
// Board-specific pins/drivers must never leak into the navigation model, packet decoder, or
// countdown engine. Profiles only select hardware providers; feature tasks compile out cleanly
// when their provider is disabled.

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

#else
#error "No board profile selected. Define BOARD_PROFILE_PROTOTYPE_C3_GC9A01 (or add a new profile)."
#endif
