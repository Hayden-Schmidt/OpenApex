// Same convention as simulator/native/countdown_bridge.cpp: pull the shared, hardware-independent
// source directly into this target's build rather than compiling all of firmware/main/ (which also
// contains ESP-IDF-only files like ble_central.c that cannot build for `native`).
#include "../../gui/gui_screens.c"
