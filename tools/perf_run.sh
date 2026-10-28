#!/bin/bash
# Measurement helpers for the DOOM Android port.
#   tools/perf_run.sh zctest            push and run work/zc_test (zero-copy feasibility: AHB / mremap / exported fd)
#   tools/perf_run.sh install           install work/DOOM-port-debug.apk
#   tools/perf_run.sh env "K=V K=V"     write dm/env (DM_GFX_* switches read at start)
#   tools/perf_run.sh start             launch the game (logcat cleared)
#   tools/perf_run.sh grab NAME         save logcat + gameplay PERF summary + 6 s dwarf call-graph profile to work/logs/NAME
export MSYS_NO_PATHCONV=1
A=${ADB:-adb}; S=${ADB_SERIAL:?set ADB_SERIAL}; D="/sdcard/Download/DOOM Mobile/dm"; PKG=org.doom.port
cd "$(dirname "$0")/.."
case "$1" in
zctest)
    $A -s $S push work/zc_test /data/local/tmp/zc_test >/dev/null && $A -s $S shell "chmod 755 /data/local/tmp/zc_test && /data/local/tmp/zc_test ${2:-123}" ;;
install)
    $A -s $S install -r work/DOOM-port-debug.apk ;;
env)
    $A -s $S shell "printf '%s\n' $2 > '$D/env'; cat '$D/env'" ;;
start)
    $A -s $S logcat -c; $A -s $S shell am force-stop $PKG; $A -s $S shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1; echo started ;;
grab)
    L=work/logs/$2; mkdir -p $L; P=$($A -s $S shell pidof $PKG | tr -d '\r')
    $A -s $S logcat -d > $L/logcat.txt
    grep -a -E "NVN PERF (v=1|parts_ms|gpu-indirect|storage-chain|zerocopy|hash_sites|partial-tex|pass_ends)" $L/logcat.txt | tail -16 | cut -c45-600 > $L/perf_tail.txt; cat $L/perf_tail.txt
    if [ -n "$P" ]; then
        $A -s $S shell "run-as $PKG simpleperf record -e cpu-clock --call-graph dwarf -p $P -f 500 --duration 6 -o /data/data/$PKG/dw.data" 2>&1 | tail -1
        $A -s $S shell "run-as $PKG simpleperf report -i /data/data/$PKG/dw.data -g caller --percent-limit 1 --sort symbol" > $L/profile_callers.txt 2>/dev/null
        $A -s $S shell "run-as $PKG simpleperf report -i /data/data/$PKG/dw.data --sort tid,symbol --percent-limit 0.3" > $L/profile_flat.txt 2>/dev/null
        wc -l $L/profile_*.txt
    fi ;;
*) sed -n 2,8p "$0" ;;
esac
