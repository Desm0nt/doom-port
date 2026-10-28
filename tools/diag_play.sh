#!/bin/bash
# Diagnostics while the owner plays: tools/diag_play.sh start NAME ["ENV LINES"] | tools/diag_play.sh stop NAME
#   start: writes dm/env (default: game log, file read stats, per-frame perf, alias log), starts the game, streams logcat to
#          work/logs/play/NAME/logcat.txt and takes a screenshot every 10 s (shot_<HHMMSS>.png, phone clock) in the background.
#   Holds the phone lock (tools/phone_lock.sh) from start to stop.
#   stop:  stops the screenshots and the logcat stream, removes dm/env, stops the game.
export MSYS_NO_PATHCONV=1
A=${ADB:-adb}; S=${ADB_SERIAL:?set ADB_SERIAL}; D="/sdcard/Download/DOOM Mobile/dm"; PKG=org.doom.port
cd "$(dirname "$0")/.."; L=work/logs/play/$2; mkdir -p $L
case $1 in
start)
  . tools/phone_lock.sh; phone_lock "diag_play $2 (owner plays)"; trap - EXIT   # held until stop: owner pid = the screenshot loop
  ENV=${3:-"DM_DBG=1 DM_FS_STATS=1 DM_GFX_PERF_SAMPLE=4 DM_GFX_ALIAS_LOG=1"}
  $A -s $S shell "printf '%s\n' $ENV > '$D/env'"
  $A -s $S logcat -c; $A -s $S logcat -G 64M >/dev/null 2>&1
  nohup $A -s $S logcat -v threadtime > $L/logcat.txt 2>/dev/null & echo $! > $L/logcat.pid
  $A -s $S shell am force-stop $PKG; $A -s $S shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
  nohup bash -c "while true; do sleep 10; t=\$($A -s $S shell date +%H%M%S | tr -d '\r'); $A -s $S exec-out screencap -p > $L/shot_\$t.png; done" >/dev/null 2>&1 & echo $! > $L/shots.pid
  sed -i "s/^pid=.*/pid=$(cat $L/shots.pid)/" work/phone.lock/owner
  echo "play $2 started: $ENV";;
stop)
  kill $(cat $L/shots.pid) $(cat $L/logcat.pid) 2>/dev/null
  $A -s $S shell "rm -f '$D/env'"; $A -s $S shell am force-stop $PKG; rm -rf work/phone.lock
  echo "play $2 stopped: $(wc -l < $L/logcat.txt) log lines, $(ls $L/*.png 2>/dev/null | wc -l) screenshots";;
esac
