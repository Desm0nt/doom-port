#!/bin/bash
# A/B series that can be cut short: tools/ab_series_skip.sh "NAME|ENV LINES" ... -> each run is tools/ab_quick.sh NAME "ENV" (~2 min),
# work/logs/ab/NAME/{logcat.txt, shot.png}. While a run goes, touching work/ab.skip ends it at once: its logcat is saved as it is
# (marked by a "skipped" file), the game is stopped, the flag removed and the next run starts. Afterwards: tools/ab_report.py NAME...
export MSYS_NO_PATHCONV=1
A=${ADB:-adb}; S=${ADB_SERIAL:?set ADB_SERIAL}; PKG=org.doom.port; D="/sdcard/Download/DOOM Mobile/dm"
cd "$(dirname "$0")/.."; F=work/ab.skip; rm -f $F
. tools/phone_lock.sh; phone_lock "$(basename $0)"
names=()
killtree() { local c; for c in $(ps -ef | awk -v p=$1 '$3==p{print $2}'); do killtree $c; done; kill $1 2>/dev/null; }   # the run and its phone_pad / sleep children (no pkill in Git Bash)
for r in "$@"; do
    n=${r%%|*}; e=${r#*|}; [ "$e" = "$r" ] && e=""; names+=("$n")
    echo "== $n [$e] $(date +%T)"
    $A -s $S shell "dumpsys power | grep -q 'mWakefulness=Awake' || input keyevent KEYCODE_WAKEUP" 2>/dev/null
    bash tools/ab_quick.sh "$n" "$e" > /dev/null 2>&1 & P=$!
    while kill -0 $P 2>/dev/null; do
        if [ -f $F ]; then
            rm -f $F; killtree $P; sleep 1
            mkdir -p work/logs/ab/$n; $A -s $S logcat -d > work/logs/ab/$n/logcat.txt; touch work/logs/ab/$n/skipped
            $A -s $S shell am force-stop $PKG; echo "   skipped at $(date +%T)"; break
        fi
        sleep 2
    done
    wait $P 2>/dev/null; sleep 10
done
$A -s $S shell "rm -f '$D/env'"; $A -s $S shell am force-stop $PKG
python -I tools/ab_report.py "${names[@]}"
