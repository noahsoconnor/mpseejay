#!/bin/sh
# Copy an mpSEEJAY build to the MPC over SSH (no USB shuffling).
#   tools/deploy.sh [version] [--restart]
# version: the dist/usb-<version> folder to send (default: the newest one).
# --restart: restart the MPC app afterwards so it loads the new plugin (this stops any playback on the MPC).
# MPC: set MPC_HOST (default mpc-live-ii.local) and MPC_DIR (default: /sdcard/VST2 Plugins, the internal drive).
set -eu
cd "$(dirname "$0")/.."
HOST=${MPC_HOST:-mpc-live-ii.local}
VER=""; RESTART=0
for a in "$@"; do case "$a" in --restart) RESTART=1 ;; *) VER=$a ;; esac; done
[ -n "$VER" ] || VER=$(ls -d dist/usb-[0-9]* | sed 's|dist/usb-||' | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)
PLUG="DJ - VST - mpSEEJAY"; OLD1="DJ - VST - MPCjay"; OLD2="DJ - VST - Decks"   # earlier names: MPCjay (0.9), Decks (to 0.8)
SRC="dist/usb-$VER/VST2 Plugins/$PLUG"
[ -f "$SRC/mpseejay.so" ] || { echo "no build at $SRC" >&2; exit 1; }
SSH="ssh -o ConnectTimeout=8 -o BatchMode=yes root@$HOST"

# Where it goes: the MPC's internal drive (/sdcard, "Internal" in MPC's browser), so a USB stick can come out while it
# runs. MPC OS 3.10.1 loads VST2 plugins from a "VST2 Plugins" folder at the top of any drive, the internal one included.
DIR=${MPC_DIR:-/sdcard/VST2 Plugins}
$SSH "mkdir -p \"$DIR\""
echo "sending mpSEEJAY $VER to $HOST:$DIR"

# Stage beside the live folder (outside VST2 Plugins, so MPC never sees it as a plugin), keep the user's Tracks folder,
# then swap with renames. The running MPC keeps the old .so it has open until it restarts.
STAGE="$DIR/../.decks-new"; BACKUP="$DIR/../.decks-previous"
$SSH "rm -rf \"$STAGE\" && mkdir -p \"$STAGE\""
COPYFILE_DISABLE=1 tar -C "$SRC" --exclude Tracks --exclude .DS_Store -cf - . | $SSH "tar -C \"$STAGE\" -xf -"
$SSH "set -e; L=\"$DIR/$PLUG\"
  for O in \"$DIR/$OLD1\" \"$DIR/$OLD2\"; do   # renamed: take over the old folder (and its Tracks)
    if [ ! -d \"\$L\" ] && [ -d \"\$O\" ]; then mv \"\$O\" \"\$L\"; fi
  done
  [ -f \"$STAGE/mpseejay.so\" ] || { echo 'upload incomplete' >&2; exit 1; }
  if [ -d \"\$L/Tracks\" ]; then mv \"\$L/Tracks\" \"$STAGE/Tracks\"; else mkdir -p \"$STAGE/Tracks\"; fi
  rm -rf \"$BACKUP\"; [ -d \"\$L\" ] && mv \"\$L\" \"$BACKUP\"
  mv \"$STAGE\" \"\$L\"; sync
  grep '\"version\"' \"\$L/plugin.json\""
echo "installed. The previous version is kept in $(dirname "$DIR")/.decks-previous"
# A copy on a USB drive (where it lived before 0.9.3) would be a second plugin with the same ID: move it out of that
# drive's plugin folder, its Tracks folder's files into the internal one first. A "drive" on the same storage as $DIR
# (MPC mounts the internal drive twice) is skipped: the first version of this moved the fresh install out (2026-11-18).
$SSH "for d in /media/*/\"VST2 Plugins\"; do for n in \"$PLUG\" \"$OLD1\" \"$OLD2\"; do
    o=\"\$d/\$n\"; [ -d \"\$o\" ] || continue
    # the same folder under another name (/media/az01-internal-sd is /sdcard): not a copy, leave it
    [ \"\$d\" -ef \"$DIR\" ] && continue
    [ \"\$(stat -c %d \"\$d\")\" = \"\$(stat -c %d \"$DIR\")\" ] && continue
    [ -d \"\$o/Tracks\" ] && cp -n \"\$o/Tracks/\"* \"$DIR/$PLUG/Tracks/\" 2>/dev/null
    rm -rf \"\$d/../.mpseejay-moved-to-internal\"; mv \"\$o\" \"\$d/../.mpseejay-moved-to-internal\"
    echo \"moved the old copy on \$d out of the plugin folder\"
  done; done; sync"

if [ $RESTART = 1 ]; then
  echo "restarting the MPC app"
  $SSH 'systemctl restart acvs'
else
  echo "restart the MPC (or run with --restart) to load it"
fi
