#!/usr/bin/env bash
# Reset the C3, capture its serial log and the phone's logcat concurrently for DURATION
# seconds, then print a lean pass/fail summary of the BLE peripheral/central handshake.
# Full raw logs are kept under logs/ for follow-up; this script prints only the summary.
#
# Usage: scripts/test-ble-link.sh [COM_PORT] [DURATION_SECONDS]
set -euo pipefail

COM_PORT="${1:-COM5}"
DURATION="${2:-90}"

ADB="$USERPROFILE/AppData/Local/Android/Sdk/platform-tools/adb.exe"
PY="$USERPROFILE/.platformio/penv/Scripts/python.exe"

cd "$(dirname "$0")/.."
mkdir -p logs
STAMP="$(date +%Y%m%d_%H%M%S)"
SERIAL_LOG="logs/ble_test_${STAMP}_serial.log"
LOGCAT_LOG="logs/ble_test_${STAMP}_logcat.log"

echo "== BLE link test: ${DURATION}s window on ${COM_PORT}, logs in logs/ble_test_${STAMP}_* =="

# --- Start capturing filtered phone logcat first, so nothing below is lost ---
"$ADB" logcat -c
"$ADB" logcat -v time \
    OpenApexMain:I OpenApexCompanion:I RelayBleClient:I RelayService:I \
    AndroidRuntime:E *:S > "$LOGCAT_LOG" 2>&1 &
LOGCAT_PID=$!

# Kill the app process (but do NOT relaunch it — no am start here). This is the real scenario
# under test: CDM observer mode should wake OpenApexCompanionService/RelayService from a fully
# killed process on its own when the terminal's advertisement reappears, with zero app interaction.
# Association/observation registration is a one-time setup step done separately, not part of this
# test loop — relaunching MainActivity here would retrigger CDM association every run.
"$ADB" shell am force-stop org.openapex.androidauto || true

# --- Reset the C3 and start capturing its serial output in the background ---
"$PY" - "$COM_PORT" "$DURATION" "$SERIAL_LOG" <<'PYEOF' &
import serial, sys, time
port, duration, outfile = sys.argv[1], float(sys.argv[2]), sys.argv[3]
ser = serial.Serial(port, 115200, timeout=1)
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
end = time.time() + duration
with open(outfile, "wb") as f:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            f.write(data)
            f.flush()
ser.close()
PYEOF
SERIAL_PID=$!

echo "C3 reset issued, capturing for ${DURATION}s (app not touched — testing cold wake)..."
sleep "$DURATION"

kill "$LOGCAT_PID" 2>/dev/null || true
wait "$SERIAL_PID" 2>/dev/null || true

# --- Point-in-time system state ---
DUMPSYS_SERVICES="$("$ADB" shell dumpsys activity services org.openapex.androidauto 2>/dev/null || true)"
DUMPSYS_CDM="$("$ADB" shell dumpsys companiondevice 2>/dev/null || true)"

check() { # check "label" "grep pattern" "file/string"
    if grep -qE "$2" <<<"$3" 2>/dev/null; then echo "PASS  $1"; else echo "FAIL  $1"; fi
}

echo
echo "== Summary =="
check "C3 booted"                      "openapex: OpenApex terminal boot"      "$(cat "$SERIAL_LOG")"
check "C3 advertising"                 "GAP procedure initiated: advertise"    "$(cat "$SERIAL_LOG")"
check "C3 saw central connect"         "central connected, conn_handle"        "$(cat "$SERIAL_LOG")"
check "C3 decoded a nav packet"        "decoded packet seq="                   "$(cat "$SERIAL_LOG")"
check "Phone: presence observed"       "observing device presence"             "$(cat "$LOGCAT_LOG")"
check "Phone: terminal appeared"       "OpenApexCompanion: terminal appeared"  "$(cat "$LOGCAT_LOG")"
check "Phone: RelayBleClient connecting" "RelayBleClient: connecting to"       "$(cat "$LOGCAT_LOG")"
check "Phone: GATT connected"          "connection state change: status=0 newState=2" "$(cat "$LOGCAT_LOG")"
check "Phone: characteristic found"    "characteristicFound=true"              "$(cat "$LOGCAT_LOG")"
check "RelayService running (point-in-time)" "ServiceRecord.*\.RelayService"   "$DUMPSYS_SERVICES"

if grep -q "AndroidRuntime: FATAL EXCEPTION\|AndroidRuntime: java.lang" "$LOGCAT_LOG" 2>/dev/null; then
    echo "FAIL  No app crash during window"
else
    echo "PASS  No app crash during window"
fi

DECODED_COUNT=$(grep -c "decoded packet seq=" "$SERIAL_LOG" 2>/dev/null || true)
DECODED_COUNT="${DECODED_COUNT:-0}"
echo
echo "Nav packets decoded by C3 this window: $DECODED_COUNT"
if [ "$DECODED_COUNT" -gt 0 ]; then
    echo "Last packet: $(grep "decoded packet seq=" "$SERIAL_LOG" | tail -1)"
fi

CDM_PRESENT=$(sed -n '/Companion Device Present/,/Companion Device Application Controller/p' <<<"$DUMPSYS_CDM" | head -5)
echo
echo "CDM presence snapshot:"
echo "$CDM_PRESENT"

echo
echo "Full logs: $SERIAL_LOG , $LOGCAT_LOG"
