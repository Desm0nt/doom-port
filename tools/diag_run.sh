#!/bin/bash
# Graphics diagnostics run: tools/diag_run.sh NAME "ENV;LINES" [stand_seconds]   (env lines separated by ';')  -> work/logs/diag/NAME/{logcat.txt, *.png}
# Enters gameplay from the save (like ab_run.sh), then: screenshot looking ahead, look down (screenshot), look up (screenshot),
# back to level, then stands still for stand_seconds (default 180) taking a screenshot every 30 s (texture / LOD streaming).
# Logcat is streamed to the file during the whole run (validation output is large). Stops the game at the end.
export MSYS_NO_PATHCONV=1
A=${ADB:-adb}; S=${ADB_SERIAL:?set ADB_SERIAL}; D="/sdcard/Download/DOOM Mobile/dm"; PKG=org.doom.port
cd "$(dirname "$0")/.."; L=work/logs/diag/$1; mkdir -p $L; STAND=${3:-30}; [ $STAND -gt 30 ] && [ -z "$DIAG_LONG" ] && STAND=30   # ~3 min a run (DIAG_LONG=1 keeps a longer stand)
. tools/phone_lock.sh; phone_lock "$(basename $0) $1"
printf '%s\n' "$2" | tr ';' '\n' > work/diag_env.txt; $A -s $S push work/diag_env.txt "$D/env" >/dev/null   # env lines separated by ';' (values may contain spaces)
$A -s $S logcat -c; $A -s $S logcat -G 64M >/dev/null 2>&1
$A -s $S logcat -v threadtime > $L/logcat.txt 2>/dev/null & LC=$!
$A -s $S shell am force-stop $PKG; $A -s $S shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sh tools/phone_pad.sh wait:55 A wait:10 A wait:10 A wait:40 B wait:10 B wait:10 B wait:5
shot() { $A -s $S exec-out screencap -p > $L/$1.png; }
# look: a slow drag on a free spot in the middle of the screen (DoomPadView look); y > 0 = finger down
look() { $A -s $S shell input swipe 1200 $1 1200 $2 1500; sleep 2; }
shot ahead
look 300 800; look 300 800; shot down
look 800 300; look 800 300; look 800 300; look 800 300; shot up
look 300 800; look 300 800; shot level
# motion (TAA ghosting / trails, motion blur, streaming while moving): hold the move stick forward 4 s and turn while
# screenshots are taken mid-motion (the input swipe runs in the background; the stick is at (0.12, 0.42) of 2400x1080)
moving() { $A -s $S shell input swipe 288 454 288 250 $1 & sleep $2; shot $3; wait $!; }
moving 4000 2 walk_1; moving 4000 2 walk_2
$A -s $S shell input swipe 900 540 1500 540 2500 & sleep 1.2; shot turn_1; wait $!
$A -s $S shell input swipe 1500 540 900 540 2500 & sleep 1.2; shot turn_2; wait $!
for i in $(seq 15 15 $STAND); do sleep 15; shot stand_$i; done
$A -s $S shell am force-stop $PKG; sleep 1; kill $LC 2>/dev/null
$A -s $S shell "rm -f '$D/env'"
echo "diag $1 done: $(wc -l < $L/logcat.txt) log lines"
