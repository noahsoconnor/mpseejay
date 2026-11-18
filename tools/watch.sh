#!/bin/sh
# Background monitor for the MPC (runs ON the MPC, started by tools/watch-start.sh): every 2 s, one line in
# /data/decks-watch.log with the time, load, the MPC app's memory and threads, and the top CPU users. Synced to disk every
# 10 s, so a forced restart keeps all but the last few seconds. Over 1 MB it moves to decks-watch.log.1.
L=/data/decks-watch.log
n=0
while :; do
  P=$(pidof MPC)
  {
    printf '%s load %s | ' "$(date +%H:%M:%S)" "$(cut -d' ' -f1-3 /proc/loadavg)"
    if [ -n "$P" ]; then
      printf 'MPC %s %s %s | ' "$P" "$(grep VmRSS /proc/$P/status | tr -s ' ' | cut -d' ' -f2-3)" \
        "$(grep Threads /proc/$P/status | tr -s '\t ' ' ')"
    else
      printf 'MPC not running | '
    fi
    printf 'mem free %s kB | ' "$(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2)"
    top -bn1 2>/dev/null | sed -n '2p;8,11p' | tr -s ' ' | tr '\n' ';'
    echo
  } >> $L
  n=$((n + 1))
  if [ $((n % 5)) = 0 ]; then
    sync
    [ "$(wc -c < $L)" -gt 1048576 ] && mv $L $L.1
  fi
  sleep 2
done
