# mpSEEJAY: development notes (this file was the README until 1.0)

A DJ deck as a plugin for MPC standalone hardware, built with the [Open MPC](https://open-mpc.github.io/) toolchain.
Version 0.9 (called Decks until 0.8): a media-player deck (two decks, A and B) for an external DJ mixer. Target: MPC Live II (Gen1, armv7).

## What it does
A media-player deck, in the spirit of standalone DJ players: each Decks plugin is one deck with its own output, and an external DJ mixer does the mixing. MPC uses only one stereo output per plugin, so run one Decks per Plugin track and set each track's output in the MPC mixer (Deck A to Out 1/2, Deck B to Out 3/4).

- **Deck switch and tempo:** the big DECK A / DECK B buttons at the top of every page pick the deck on screen (its controls, pads and readouts). The BPM is large at the top right, with pitch, range and sync source under it; BPM + / BPM - move the tempo 0.1 BPM a tap. LEADER and PITCH RANGE are on SETUP.
- **Each deck keeps its own output level**, whichever deck its screen shows: MPC sets a plugin's first parameter (the level) on every STOP, and in 0.6 that reached the other deck and could silence it.
- **Safety for live use:** a deck that is playing only takes a new track on a second LOAD within 3 s (the browser says so); the platter ring flashes amber in the last 30 s of a playing track; tracks longer than 15 minutes load their first 15 minutes, read only that far, so a long mix can't run the MPC out of memory.
- **Pads on screen:** PLAY is built round a 4x4 grid that mirrors the MPC pads (pad 1 bottom left), each coloured by its function, lit while held and while its function is on (PLAY playing, SYNC on, a hot cue set). A tap on a tile works too.
- **Deck colours:** A blue, B red. The badge, rules, waveform, beat lights and jog ring take the colour of the deck the screen is showing.
- **PLAY page:** both decks' overview waveforms (A on top in blue, B below in red), each filling in as it plays, deck letter, title, time and tempo, a beat-of-the-bar light and a phase meter, CUE, PLAY/PAUSE, BROWSE, a vinyl-style pitch fader (±8 / 16 / 50%) with RESET, NUDGE + / NUDGE -, a JOG control (playing: bends the speed; paused: moves 1/32 beat per step), SYNC and LEADER.
- **Sync between decks:** all decks run in one MPC process, so SYNC locks to the leader deck's tempo and beat, or to the MPC's tempo when there's no leader. The first deck played becomes the leader; LEADER hands it to another deck.
- **Control another deck:** CONTROL A / B (top left of PLAY) points this screen, its Q-Links and its pads at another deck. Controls go to that deck and its readouts and waveform show here; each deck still plays out of its own track and output. Each deck still saves only its own track.
- **MPC +/- and wheel:** they adjust the control last touched. Touch JOG, then +/- nudge and the wheel jogs; in the browser, touch SCROLL and the wheel scrolls.
- **MPC transport (SETUP, on by default):** MPC PLAY plays the deck, PLAY START plays from the cue point, STOP pauses.
- Track browser over every drive, tempo and beat-grid detection on load, CDJ-style cue, 8 hot cues, loops, slip loop rolls, beat jumps.
- Saves the track (by path, found by name if it moved), browser folder, hot cues, corrected tempo, deck letter and settings with the project.
- The EQ and filter from earlier versions are off the screen; old projects that used them still sound the same.

## Pads
Pad 1 is bottom left. Pads are found by their note: chromatic from **Pad Base** (SETUP, default 36, bank B from base+16), or tap **LEARN PADS** on SETUP and hit pads 1 to 16 on bank A, then 1 to 16 on bank B (tap LEARN again to stop after bank A). Learning works with any pad mode and is saved with the project. The tiles on PLAY and the MPC's own pad lights show the bank you last played.

**Bank A, the deck** (with **SHIFT**, pad 8, held: the second label)

| Row | Pads |
|---|---|
| Top | 13 DECK A, 14 DECK B (lit in the deck's colour when it is the one on screen), 15 LOOP, 16 BPM + (SHIFT: +1) |
| 3 | 9-11 loops of 2, 4 and 8 bars (the same pad again ends it), 12 BPM - (SHIFT: -1) |
| 2 | 5 SYNC (SHIFT: LEADER), 6 JUMP - (SHIFT: halve the length), 7 JUMP + (SHIFT: double it), 8 SHIFT |
| Bottom | 1 CUE (hold to preview), 2 PLAY/PAUSE, 3 NUDGE -, 4 NUDGE + (held: bend; paused: step 1/32 beat) |

A tap on SHIFT on the screen holds it for the next tile tap.

CUE, pads 5-8 and the hot cue pads (bank B) light in the colour of the deck they act on (A blue, B red), on screen and on the MPC, so you always know which deck you are about to hit.

**Bank B, hot cues**

| Row | Pads |
|---|---|
| Top | 13-16 hot cues 1-4 |
| 3 | 9-12 hot cues 5-8 |
| 2 | 5-8 loop rolls 1/8, 1/4, 1/2, 1 beat (while held) |
| Bottom | 1-4 loops of 1, 2, 4, 8 beats (the same pad again ends it) |

An empty hot cue pad sets the cue, a set one jumps to it and plays. **Hot Cue Pads** on the LOOP page: Set overwrites, Clear deletes.

## MPC screen on your computer
The MPC Remote addin (installed on this MPC) shows the MPC's screen in a browser and takes clicks as touches. It listens on the MPC only: open a tunnel, then browse to http://localhost:6720.
```bash
ssh -N -L 6720:127.0.0.1:6720 root@mpc-live-ii.local
```

## Browser
On PLAY, tap **BROWSE** and the browser takes the lower part of the screen (the waveforms come back when it closes): 6 rows of 40 px bold names beside a map of the 16 pads (no .mp3 / .wav; folders end in ">"), the path as "USB DRIVE > House (154)", and one bar of buttons at the bottom: BACK, PAGE UP, PAGE DOWN, SCROLL, LOAD A, LOAD B, EJECT, CLOSE. Touch **SCROLL**, then the data wheel moves one row per click (Q-Link 1 too). Tap a row to select it, tap it again to open a folder. Rows show what is in use by colour: a track on a deck has that deck's colour bar and shading; a track that has played 30 s since the MPC app started is greyed out. **LOAD A** and **LOAD B** load the selection onto either deck, whichever deck's screen you are on.

While the browser is open the pads drive it (they light up with these functions, and the map beside the rows shows them): pad 14 UP and pad 10 DOWN (both repeat while held), pad 13 PAGE UP, pad 5 PAGE DN, pad 9 BACK (up a folder), pad 11 OPEN, pad 1 LOAD A, pad 2 LOAD B, pad 16 CLOSE. The MPC keeps the data wheel's press and its Shift button to itself (a plugin sees only the pad notes), so neither can do anything in Decks. **UP A FOLDER**, **PG UP** / **PG DN** move around.

## Installed on the internal drive
`tools/deploy.sh` installs to the MPC's internal drive (`/sdcard/VST2 Plugins`, "Internal" in MPC's browser), so the USB stick can come out while MPCjay runs: loaded tracks live in memory. A copy left in a USB drive's `VST2 Plugins` is moved out of it (to `.mpcjay-moved-to-internal` on that drive), so MPC never sees two. The browser's **EJECT** flushes the drive on screen and says when it is safe to pull (MPC's own browser does the full unmount).

## Install on MPC OS 3.10.1 or later
1. Copy the `VST2 Plugins` folder from `dist/usb-0.9.9/` to the top level of a USB stick or SD card. If the card already has a `VST2 Plugins` folder, copy just `DJ - VST - MPCjay` into it.
2. Put your WAV or MP3 files anywhere on a drive the MPC can see; the browser lists every drive and folder.
3. On the MPC, turn on **Preferences > Enhancements > External Plugin Support (VST2 Support)**, then restart the MPC.
4. Add a Plugin track and choose **External VST Plugins > DJ > MPCjay**. Add a second Plugin track with another Decks for deck B, and set each track's output in the MPC mixer.

**Over the network (MPC with SSH on):** `tools/deploy.sh` sends the newest `dist/usb-*` build to the MPC (`mpc-live-ii.local`, or set `MPC_HOST`), into the drive's `VST2 Plugins` folder that already has Decks. It keeps your `Tracks` folder and saves the previous version as `.decks-previous` next to `VST2 Plugins`. Add `--restart` to restart the MPC app so it loads the new version.

`dist/MPCjay-0.9.9-mpc-armv7.zip` is the standard Open MPC release zip, with the root-SSH installer for older MPC OS versions.

## Build and test
```bash
MPC_VST=../vendor/mpc-vst-plugins
$MPC_VST/tools/test_port.sh vst.json                       # Open MPC's host test (ASan/UBSan)
cc -O1 -g -fsanitize=address,undefined -I$MPC_VST/wrapper -Isrc tests/deck_test.c -lm -lpthread -o build/deck_test && build/deck_test
$MPC_VST/tools/build_port.sh vst.json                      # device build + skin: needs Docker
```
`deck_test` makes synthetic drum tracks at known tempos and checks tempo detection, playback, cues, rolls, loops, sync and save/restore. It also writes `build/demo.wav` (a short performance) to listen to.

## MPC pad lights (test)
**MPC PAD LIGHTS** on SETUP (on by default) colours the MPC's own pads like the tiles on screen. The MPC app holds the pads' MIDI port, so Decks, which runs inside the app, writes through the app's own handle: `F0 47 7F 47 65 00 04 <pad> <r> <g> <b> F7` to the controller's Public port (format from TheKikGen's MPC-LiveXplore wiki; product 47 = Live II, confirmed on a Live II: the pads lit, while 3B, 46 and 4A did nothing). It repaints five times a second, because the MPC paints a hit pad itself, and only while Decks is in use (a deck playing, or a touch in the last 30 s). Which pad number is which pad is not yet confirmed.

## Crash logging
`tools/watch-start.sh` turns on two logs on the MPC's internal drive, which survive a forced restart: Decks' own (`/data/decks.log`: every parameter change and pad note, and a heartbeat every 2 s per deck with the longest audio block) and a monitor (`/data/decks-watch.log`: load, the MPC app's memory and threads, and the top processes every 2 s). Run it again after a reboot to restart the monitor. `tools/watch-start.sh stop` turns both off.

## Not verified yet
Nothing has run on an MPC. Sync uses Open MPC's `HAS_SONG_POS`, which Open MPC itself lists as offline-only so far. The pad notes your Live II sends depend on its pad mode. CPU use on the Live II is unmeasured (about 1.3 µs per block on an Apple-silicon Mac).

## Third-party code
`src/vendor/dr_wav.h`, `dr_mp3.h`: [dr_libs](https://github.com/mackron/dr_libs) (public domain / MIT-0), commit in `src/vendor/COMMIT`, unchanged.
