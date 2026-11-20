# mpSEEJAY

Two DJ decks for MPC standalone, made to play into an external DJ mixer. Each deck is its own plugin on its own track
and output, so the mixer does the mixing and mpSEEJAY does the decks: loading, beatmatching, cueing, looping.

Built with the [Open MPC](https://open-mpc.github.io/) toolchain. Tested on an MPC Live II with MPC OS 3.10.1.

## What it does
- **Two decks, A and B**, each a plugin on its own Plugin track. Set deck A's track to Out 1/2 and deck B's to Out 3/4
  in the MPC mixer, and run each output into its own channel on your DJ mixer.
- **Control either deck from either screen:** the DECK A / DECK B buttons (or pads 13 and 14) switch the screen, the
  pads and the Q-Links between the decks. Each deck keeps playing out of its own output.
- **Both decks at a glance:** both tempos and both overview waveforms are always on screen, A in blue, B in red.
- **Beatmatching:** tempo and beat-grid detection on load, SYNC to the other deck (or to the MPC's tempo), a phase
  meter, a vinyl-style pitch fader (8, 16 or 50%), BPM + / - buttons (0.1 BPM, or 1 with SHIFT), nudge, and a jog.
  Touch JOG, then the MPC's data wheel or + / - nudges.
- **The 16 pads, on screen and lit on the MPC:** the PLAY page mirrors the pads, coloured by what they do, and the
  MPC's own pads light up in the same colours. CUE, SYNC, the jumps and the hot cues take the colour of the deck they
  act on, so you always know which deck you're about to hit.
- **Cue and loops:** CDJ-style CUE (hold to preview), 8 hot cues, loops of 2, 4 and 8 bars, auto loops of 1 to 8
  beats, loop rolls, beat jumps.
- **Track browser** over every drive: big rows readable in a dark room, each with the title, the artist and the
  tempo, sortable by ORDER, ARTIST, TITLE or BPM. Tracks on a deck are shaded in its colour, tracks you've played greyed
  out. Artist, title and tempo come from rekordbox, an MP3's tags or an "Artist - Title" file name, and a deck's own
  tempo once it has loaded the track. Load onto either deck from either screen. The browser stays open after a load, ready for the next track. A deck that's playing needs LOAD pressed twice,
  so a slip can't cut it off.
- **Two sections:** the browser opens on FOLDERS (every drive and its folders) and REKORDBOX.
- **Rekordbox playlists:** a stick exported from rekordbox (Export mode, for CDJs) gets a REKORDBOX entry at the top of
  the browser, with your playlist folders and playlists in rekordbox's order, with
  rekordbox's titles, artists and tempos. Read-only: mpSEEJAY never writes to the stick. WAV and MP3 tracks only (others are left out).
- **Safety for live use:** tracks load into memory, so the drive they came from can come out; the jog ring flashes in
  the last 30 seconds of a track; EJECT in the browser says when a drive is safe to pull.
- **Silent intros skipped:** a track that starts with silence (or hiss) loads with its cue on the first beat of the
  music.
- **Stars:** pads 3 and 4 in the browser (STARS - / +) rate the selected track 0 to 5 stars, shown on its row and
  remembered for the file; rekordbox's own ratings show too. SORT has STARS 5-1 and STARS 1-5.
- **CHILL / HYPE:** a switch per track (both off until you pick one): pads 7 and 8 in the browser, or the CHILL and
  HYPE buttons under the pad map. The row shows the tag, and the browser's top has CHILL and HYPE sections listing every
  tagged track, from any drive or playlist.
- **Clock:** time played, time left at the current tempo (big) and the length on every page, and each deck's time left
  under its tempo.
- **Pitch faders:** SETUP learns a MIDI fader for each deck's pitch (LEARN DECK A / B FADER, then move it), with soft
  takeover so the tempo never jumps, and UP+ / UP- for which way is faster. CC 22 also moves the pitch.
- **START page:** tap the waveform to see the start of the track up close and move where it starts (by a bar, a beat or
  10 ms, to the playhead, the top, or back to AUTO), and play from there. A start set by hand is remembered for that
  file, in a hidden file in mpSEEJAY's Tracks folder.
- WAV and MP3, up to 15 minutes a track. Saves each deck's track, cues, tempo and settings with the project.

## Install
**With the Open MPC Plugin Manager:** find mpSEEJAY in the catalog and install it.

**By hand (MPC OS 3.10.1 or later):**
1. Unzip the release and copy its `VST2 Plugins` folder (the one holding `DJ - VST - mpSEEJAY`) to the top level of the
   MPC's internal drive, or of a USB stick or SD card. If a `VST2 Plugins` folder is already there, copy just
   `DJ - VST - mpSEEJAY` into it. The internal drive is best: then a stick can come out while mpSEEJAY runs.
2. On the MPC, turn on **Preferences > Enhancements > External Plugin Support (VST2 Support)** and restart the MPC.
3. Add a Plugin track and choose **External VST Plugins > DJ > mpSEEJAY** (deck A). Add a second Plugin track with
   mpSEEJAY (it becomes deck B). Set each track's output in the MPC mixer.
4. On **SETUP**, tap **LEARN PADS** and hit pads 1 to 16 on bank A, then 1 to 16 on bank B. This works with any pad
   mode and is saved with the project.

## Pads
Pad 1 is bottom left. The screen and the pad lights follow the bank you last played.

**Bank A, the deck** (hold **SHIFT**, pad 8, for the second function)

| Row | Pads |
|---|---|
| Top | 13 DECK A, 14 DECK B, 15 LOOP, 16 BPM + (SHIFT: +1) |
| 3 | 9 / 10 / 11 loops of 2 / 4 / 8 bars, 12 BPM - (SHIFT: -1) |
| 2 | 5 SYNC (SHIFT: make leader), 6 JUMP - (SHIFT: halve length), 7 JUMP + (SHIFT: double), 8 SHIFT |
| Bottom | 1 CUE (hold to preview), 2 PLAY/PAUSE, 3 NUDGE -, 4 NUDGE + (hold to bend; paused: step) |

**Bank B, hot cues**

| Row | Pads |
|---|---|
| Top two rows | hot cues 1-8 (empty: set; set: jump and play) |
| 2 | loop rolls of 1/8, 1/4, 1/2, 1 beat (while held) |
| Bottom | loops of 1, 2, 4, 8 beats |

**In the browser:** 14 UP, 10 DOWN (both repeat while held), 13 PAGE UP, 5 PAGE DOWN, 9 BACK, 11 OPEN, 15 SORT, 3 STARS -, 4 STARS +, 7 CHILL, 8 HYPE, 1 LOAD A,
2 LOAD B, 16 CLOSE.

## Pages
- **PLAY:** both tempos and waveforms, the 16 pads, jog, SYNC, BPM + / -, pitch fader, BROWSE.
- **LOOP:** loop length, loop on/off, beat jumps, what the hot cue pads do (play, set or clear).
- **SETUP:** which deck this plugin is, MPC PLAY / STOP follow, output level, sync leader, pitch range, LEARN PADS,
  MPC pad lights on/off.

## Good to know
- **MPC pad lights:** mpSEEJAY colours the MPC's pads by sending colour messages through MPC's own connection to its
  pads (it can't open that port itself). Tested on a Live II. While a deck plays it keeps the pads coloured, so if you
  switch to another track on the MPC mid-set its pads may show mpSEEJAY's colours: turn **MPC PAD LIGHTS** off on SETUP
  if that matters to you.
- **MPC transport:** with **MPC PLAY / STOP** on FOLLOW (the default), MPC's PLAY plays, PLAY START plays from the cue
  and STOP pauses both decks.
- **The MPC's own Shift and data-wheel press** don't reach plugins, so SHIFT is pad 8 and loading is on the screen or
  the pads.

## Build and test
Needs the [Open MPC toolchain](https://github.com/open-mpc/mpc-vst-plugins) cloned next to this folder, and Docker for
the device build.
```bash
MPC_VST=../mpc-vst-plugins
cc -O1 -g -fsanitize=address,undefined -I$MPC_VST/wrapper -Isrc tests/deck_test.c -lm -lpthread -o build/deck_test && build/deck_test
$MPC_VST/tools/test_port.sh vst.mactest.json     # Open MPC's host test (the same plugin, built for a desktop)
$MPC_VST/tools/build_port.sh vst.json            # device build and skin (Docker)
```
`tests/deck_test.c` checks tempo detection, playback, cues, loops, sync, both decks together, the pads, the browser
and save/restore (157 checks). `docs/DEVELOPMENT.md` has the development notes, including `tools/deploy.sh` (install
over SSH on a rooted MPC).

## Credits and licence
mpSEEJAY is MIT licensed (`LICENSE`). It uses [dr_libs](https://github.com/mackron/dr_libs) (`dr_wav.h`, `dr_mp3.h`;
public domain / MIT-0) for reading WAV and MP3, unchanged, at the commit in `src/vendor/COMMIT`. The pad-light message
format comes from TheKikGen's [MPC-LiveXplore](https://github.com/TheKikGen/MPC-LiveXplore) notes.
MPC is a trademark of inMusic Brands; mpSEEJAY is an independent project and is not made or endorsed by Akai or inMusic.
