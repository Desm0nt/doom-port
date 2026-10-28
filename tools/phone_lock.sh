#!/bin/bash
# One phone, several agents: every script that starts the game holds this lock for its whole run.
#   source tools/phone_lock.sh; phone_lock "<who/what>"     waits until the phone is free (mkdir is atomic), releases on exit
# work/phone.lock/owner says who holds it and since when. A holder whose process is gone is taken over. A series script can take
# it once (phone_lock in its own shell) and the runs it starts reuse it (holder = their parent).
phone_lock() {
  local dir=work/phone.lock
  while ! mkdir $dir 2>/dev/null; do
    local pid=$(sed -n 's/^pid=//p' $dir/owner 2>/dev/null)
    if [ "$pid" = "$PPID" ]; then return 0; fi   # held by the series that runs us (it releases it)
    if [ -n "$pid" ] && ! kill -0 $pid 2>/dev/null; then echo "phone lock: holder $pid is gone, taking over"; rm -rf $dir; continue; fi
    sleep 10
  done
  printf 'pid=%s\nwho=%s\nsince=%s\n' $$ "$1" "$(date '+%F %T')" > $dir/owner
  trap 'rm -rf work/phone.lock' EXIT
}
