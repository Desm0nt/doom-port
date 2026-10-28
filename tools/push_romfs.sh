#!/bin/sh
# Push only missing / size-mismatched RomFS files to the device (adb push --sync is unreliable here).
export MSYS_NO_PATHCONV=1
ADB="${ADB:-adb} -s ${ADB_SERIAL:-emulator-5554}"
SRC=${ROMFS:-$(dirname "$0")/../work/romfs}; DST=${DST:-"/sdcard/Download/DOOM Mobile/romfs"}
$ADB shell "cd '$DST' 2>/dev/null && find . -type f -exec stat -c '%s %n' {} +" | tr -d '\r' | sed 's| \./| |' | sort -k2 > /tmp/remote.txt
cd $SRC && find . -type f -printf '%s %P\n' | sort -k2 > /tmp/local.txt
n=0
while read size path; do
  r=$(awk -v p="$path" '$2==p{print $1}' /tmp/remote.txt)
  if [ "$r" != "$size" ]; then
    $ADB shell mkdir -p "'$DST/$(dirname "$path")'" </dev/null; $ADB push "$path" "$DST/$path" </dev/null >/dev/null 2>&1 || echo "FAIL $path"; n=$((n+1))
  fi
done < /tmp/local.txt
echo "pushed $n files"
