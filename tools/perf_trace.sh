#!/bin/bash
# System trace with perfetto (no root): tools/perf_trace.sh NAME  -> work/logs/NAME/doom.pftrace
export MSYS_NO_PATHCONV=1
A=${ADB:-adb}; S=${ADB_SERIAL:?set ADB_SERIAL}
cd "$(dirname "$0")/.."; L=work/logs/$1; mkdir -p $L
$A -s $S push tools/perfetto/doom.cfg /data/local/tmp/doom.cfg >/dev/null
$A -s $S shell "cat /data/local/tmp/doom.cfg | perfetto --txt -c - -o /data/misc/perfetto-traces/doom.pftrace" 2>&1 | tail -2
$A -s $S pull /data/misc/perfetto-traces/doom.pftrace $L/doom.pftrace >/dev/null && ls -la $L/doom.pftrace
