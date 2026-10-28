#!/bin/bash
# End the current config of tools/play_series.sh early (the owner has answered): kills the `sleep SECONDS` the series is in,
# so it stops the game, saves that run's screenshots / log and starts the next config at once.
for s in $(ps -ef | grep -v grep | grep "play_series" | awk '{print $2}'); do
  for p in $(ps -ef | awk -v s=$s '$3==s && $0 ~ /sleep/ {print $2}'); do kill $p && echo "ended run early (sleep $p of series $s)"; done
done
