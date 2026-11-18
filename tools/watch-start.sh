#!/bin/sh
# Turn on crash logging on the MPC: Decks' own log (/data/decks.log, from the next MPC app start) and the background
# monitor (/data/decks-watch.log, from now until the MPC reboots). Run again after a reboot to restart the monitor.
#   tools/watch-start.sh          start      tools/watch-start.sh stop      stop both and remove the flag
set -eu
cd "$(dirname "$0")"
HOST=${MPC_HOST:-mpc-live-ii.local}
SSH="ssh -o ConnectTimeout=8 -o BatchMode=yes root@$HOST"
if [ "${1:-}" = stop ]; then
  $SSH 'for p in $(ps | grep "[d]ata/decks-watch" | sed "s/^ *//" | cut -d" " -f1); do kill $p; done; rm -f /data/decks-debug; echo stopped'
  exit
fi
$SSH 'cat > /data/decks-watch.sh && chmod +x /data/decks-watch.sh' < watch.sh
$SSH 'touch /data/decks-debug; for p in $(ps | grep "[d]ata/decks-watch" | sed "s/^ *//" | cut -d" " -f1); do kill $p; done; setsid nohup /data/decks-watch.sh >/dev/null 2>&1 < /dev/null & sleep 1; ps | grep -q "[d]ata/decks-watch" && echo "monitor running"'
