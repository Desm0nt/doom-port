#!/bin/sh
# Push the CLI host to the emulator / device, run it for N seconds, pull the log.
# Usage: tools/run_emu.sh <run-name> [seconds] [extra env...]
export MSYS_NO_PATHCONV=1
ADB="${ADB:-adb} -s ${ADB_SERIAL:-emulator-5554}"
NAME=${1:-run}; SECS=${2:-60}; shift 2 2>/dev/null
W=$(cd "$(dirname "$0")/.." && pwd)/work
$ADB push $W/build-android/dmhost /data/local/tmp/doom/dmhost >/dev/null && $ADB shell chmod 755 /data/local/tmp/doom/dmhost
$ADB shell "cd /data/local/tmp/doom && DM_ROOT=/data/local/tmp/doom/root DM_ROMFS='/sdcard/Download/DOOM Mobile/romfs' DM_SAVE=/data/local/tmp/doom/save $* timeout $SECS ./dmhost /data/local/tmp/doom/mods main sdk > $NAME.log 2>&1; echo exit \$? >> $NAME.log"
$ADB pull /data/local/tmp/doom/$NAME.log $W/logs/$NAME.log >/dev/null
echo "== $W/logs/$NAME.log ($(wc -l < $W/logs/$NAME.log) lines)"
