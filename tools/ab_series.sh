#!/bin/bash
# A/B series over a flaky wireless adb: tools/ab_series.sh HOST:PORT [install]  -> work/logs/ab/<run>, then the report.
# Each run reconnects first and is skipped (not hung) when the phone is unreachable.
export MSYS_NO_PATHCONV=1
A=${ADB:-adb}; export ADB_SERIAL=$1
cd "$(dirname "$0")/.."
up() { $A connect $ADB_SERIAL >/dev/null 2>&1; timeout 10 $A -s $ADB_SERIAL shell true >/dev/null 2>&1; }
if [ "$2" = install ] || [ "$2" = deploy ]; then up || { echo "phone unreachable"; exit 1; }
  for f in main.patched.bin main.cuts.tsv main.obs.tsv main.gates.tsv main.sites.tsv main.fixups.tsv main.params.tsv; do
    [ -f work/patched/$f ] && $A -s $ADB_SERIAL push work/patched/$f "/sdcard/Download/DOOM Mobile/dm/out/$f" | tail -1; done; fi
[ "$2" = install ] && { up && $A -s $ADB_SERIAL install -r work/DOOM-port-debug.apk | tail -1 || { echo "install: phone unreachable"; exit 1; }; }
RUNS=(
  "base|"
  "backoff0|DM_YIELD_BACKOFF=0"
  "backoff50|DM_YIELD_BACKOFF=50"
  "slack_sys|DM_TIMERSLACK_NS=0"
  "handoff0|DM_HANDOFF=0"
  "lrz|DM_GFX_LRZ_TOUCH=1"
  "barrier|DM_GFX_GUEST_BARRIER=1 DM_GFX_STORAGE_CHAIN=0"
)
names=()
for r in "${RUNS[@]}"; do n=${r%%|*}; e=${r#*|}
  if ! up; then echo "$n: phone unreachable, skipped"; continue; fi
  timeout 600 bash tools/ab_run.sh "$n" "$e"; names+=("$n")
done
python tools/ab_report.py "${names[@]}"
