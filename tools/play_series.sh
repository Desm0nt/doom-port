#!/bin/bash
# A/B series while the owner plays (no scripted input): tools/play_series.sh SECONDS "name|ENV;LINES" ...
# For each config: writes dm/env (lines separated by ';'), restarts the game, has the phone write logcat (work/logs/play/<name>/logcat.txt)
# and has the phone take a screenshot every 10 s; after SECONDS stops the game, pulls the screenshots and deletes them from the
# phone (all at the end of the series); NO_SHOTS=1 skips the screenshots (log only, e.g. over mobile data). Holds the phone lock (tools/phone_lock.sh) for the whole series.
export MSYS_NO_PATHCONV=1
A=${ADB:-adb}; S=${ADB_SERIAL:?set ADB_SERIAL}; D="/sdcard/Download/DOOM Mobile/dm"; PKG=org.doom.port
cd "$(dirname "$0")/.."; . tools/phone_lock.sh; phone_lock "play_series"
SECS=$1; shift
P="$D/shots"
printf '#!/system/bin/sh\nmkdir -p "%s"\nwhile true; do screencap -p "%s/s_$(date +%%H%%M%%S).png"; sleep 10; done\n' "$P" "$P" > work/shots.sh
$A -s $S push work/shots.sh /data/local/tmp/dm_shots.sh >/dev/null
for cfg in "$@"; do
  name=${cfg%%|*}; env=${cfg#*|}; L=work/logs/play/$name; mkdir -p $L
  printf '%s\n' "$env" | tr ';' '\n' > work/diag_env.txt; $A -s $S push work/diag_env.txt "$D/env" >/dev/null
  $A -s $S shell "rm -rf '$P'"
  $A -s $S logcat -c; $A -s $S logcat -G 64M >/dev/null 2>&1
  # the phone writes the log itself (a streamed adb logcat dies on Wi-Fi hiccups) into the screenshot folder, pulled with it
  $A -s $S shell "mkdir -p '$P'; setsid sh -c \"logcat -v threadtime -f '$P/logcat.txt'\" </dev/null >/dev/null 2>&1 &"   # setsid: survives the adb shell
  $A -s $S shell am force-stop $PKG; $A -s $S shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
  [ -z "$NO_SHOTS" ] && { $A -s $S shell sh /data/local/tmp/dm_shots.sh >/dev/null 2>&1 & SH=$!; }   # NO_SHOTS=1: log only (mobile data)
  echo "$(date +%T) $name started: $env"
  sleep $SECS
  $A -s $S shell "pkill -f dm_shots.sh"; kill $SH 2>/dev/null
  $A -s $S shell am force-stop $PKG; sleep 1; $A -s $S shell "pkill -f 'logcat -v threadtime -f'"
  # screenshots and log move aside on the phone; everything is pulled after the series (pulls during it starved the next
  # config's adb commands over Wi-Fi and the game started minutes late)
  $A -s $S shell "mv '$P' '$D/shots_$name'"
  echo "$(date +%T) $name done"
done
for cfg in "$@"; do name=${cfg%%|*}; mkdir -p work/logs/play/$name
  $A -s $S pull "$D/shots_$name/." work/logs/play/$name/ >/dev/null 2>&1 && $A -s $S shell "rm -rf '$D/shots_$name'"
  echo "$(date +%T) $name pulled: $(ls work/logs/play/$name/*.png 2>/dev/null | wc -l) shots"; done
$A -s $S shell "rm -f '$D/env' /data/local/tmp/dm_shots.sh"
