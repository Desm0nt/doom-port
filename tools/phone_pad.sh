#!/bin/sh
# Drive DOOM Mobile's on-screen Joy-Con pad on the phone (landscape 2400x1080, density 420).
# Usage: tools/phone_pad.sh <button>... ; buttons: A B X Y UP DOWN LEFT RIGHT PLUS MINUS ZR ZL R L, shot:<name>, wait:<sec>
export MSYS_NO_PATHCONV=1
ADB="${ADB:-adb} -s ${ADB_SERIAL:?set ADB_SERIAL}"
W=2400; H=1080; DP=2.625
pos() {   # fx fy ox oy (dp)
  awk -v fx=$1 -v fy=$2 -v ox=$3 -v oy=$4 -v W=$W -v H=$H -v dp=$DP 'BEGIN{printf "%d %d", fx*W+ox*dp, fy*H+oy*dp}'
}
for b in "$@"; do
  case $b in
    A) p=$(pos 0.885 0.36 50 0);; B) p=$(pos 0.885 0.36 0 50);; X) p=$(pos 0.885 0.36 0 -50);; Y) p=$(pos 0.885 0.36 -50 0);;
    UP) p=$(pos 0.21 0.74 0 -50);; DOWN) p=$(pos 0.21 0.74 0 50);; LEFT) p=$(pos 0.21 0.74 -50 0);; RIGHT) p=$(pos 0.21 0.74 50 0);;
    PLUS) p=$(pos 0.62 0.09 0 0);; MINUS) p=$(pos 0.38 0.09 0 0);; R) p=$(pos 0.84 0.09 0 0);; ZR) p=$(pos 0.95 0.09 0 0);;
    L) p=$(pos 0.16 0.09 0 0);; ZL) p=$(pos 0.05 0.09 0 0);;
    shot:*) $ADB exec-out screencap -p > "$(dirname "$0")/../work/logs/${b#shot:}.png"; continue;;
    wait:*) sleep ${b#wait:}; continue;;
    *) echo "unknown $b"; continue;;
  esac
  $ADB shell input swipe $p $p 300; sleep 1.5
done
