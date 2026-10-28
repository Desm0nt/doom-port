#!/bin/bash
# One A/B run: tools/ab_run.sh NAME "ENV LINES" -> work/logs/ab/NAME/{logcat.txt,shot.png,gbuf.raw}; then tools/ab_report.py
# Starts the game with dm/env = DM_GFX_PERF_SAMPLE=4 + ENV, enters gameplay from the save, dumps the G-buffer (texture serial 2),
# "Press B to continue" comes when the level is loaded; B is pressed from 40 s on, every 10 s, six times, so it lands right after a fast or a slow load.
# screenshots, saves logcat and stops the game.
export MSYS_NO_PATHCONV=1
A=${ADB:-adb}; S=${ADB_SERIAL:?set ADB_SERIAL}; D="/sdcard/Download/DOOM Mobile/dm"; PKG=org.doom.port
cd "$(dirname "$0")/.."; L=work/logs/ab/$1; mkdir -p $L
. tools/phone_lock.sh; phone_lock "$(basename $0) $1"
$A -s $S shell "printf '%s\n' DM_GFX_PERF_SAMPLE=4 $2 > '$D/env'; rm -f '$D/dump/'*"
$A -s $S logcat -c; $A -s $S shell am force-stop $PKG; $A -s $S shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sh tools/phone_pad.sh wait:55 A wait:10 A wait:10 A wait:40 B wait:10 B wait:10 B wait:30
$A -s $S shell "echo 2 > '$D/gfx-dump-now'"; sleep 6
sh tools/phone_pad.sh shot:ab_$1; mv work/logs/ab_$1.png $L/shot.png
$A -s $S logcat -d > $L/logcat.txt
f=$($A -s $S shell "ls '$D/dump'" | tr -d '\r' | grep -E "^2_n" | head -1); [ -n "$f" ] && $A -s $S pull "$D/dump/$f" $L/gbuf.raw >/dev/null
$A -s $S shell am force-stop $PKG
echo "run $1 done"
