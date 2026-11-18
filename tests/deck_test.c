/* Functional test of the Decks engine on the desktop: builds synthetic drum tracks at known tempos, then drives the engine
 * the way the wrapper does (set_param / midi / song_pos / render) and checks what it does. Writes a short performance to
 * build/demo.wav to listen to.
 *   cc -O1 -g -fsanitize=address,undefined -I../vendor/mpc-vst-plugins/wrapper -Isrc tests/deck_test.c -lm -lpthread */
#include "../src/deck.c"
#include <unistd.h>

static int fails;
#define CHECK(c, ...) do { printf("%s ", (c) ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); if (!(c)) fails++; } while (0)

static const mpc_engine_t *E;

/* A drum loop: kick on every beat, snare on 2 and 4, closed hat on the 8ths, a bass note; `lead` seconds of silence first. */
static void write_track(const char *path, double bpm, double secs, double lead, unsigned rate) {
    int64_t n = (int64_t)(secs * rate);
    int16_t *pcm = calloc(n * 2, sizeof(int16_t));
    double beat = 60.0 / bpm;
    unsigned seed = 1;
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / rate - lead;
        if (t < 0) continue;
        double bp = fmod(t, beat), b8 = fmod(t, beat / 2);
        int bi = (int)(t / beat) % 4;
        double kick = bp < 0.25 ? sin(2 * M_PI * (50 + 120 * exp(-bp * 30)) * bp) * exp(-bp * 12) : 0;
        seed = seed * 1103515245 + 12345;
        double noise = ((seed >> 16) & 0x7fff) / 16384.0 - 1.0;
        double snare = (bi == 1 || bi == 3) && bp < 0.2 ? noise * exp(-bp * 25) * 0.6 : 0;
        double hat = b8 < 0.05 ? noise * exp(-b8 * 120) * 0.25 : 0;
        double bass = sin(2 * M_PI * 55 * t) * 0.15;
        double v = (kick * 0.8 + snare + hat + bass) * 0.7;
        v = v > 0.99 ? 0.99 : v < -0.99 ? -0.99 : v;
        pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)(v * 32767);
    }
    drwav_data_format f = { drwav_container_riff, DR_WAVE_FORMAT_PCM, 2, rate, 16 };
    drwav w;
    drwav_init_file_write(&w, path, &f, NULL);
    drwav_write_pcm_frames(&w, n, pcm);
    drwav_uninit(&w);
    free(pcm);
}

static void run(deck_t *d, double secs, int16_t *rec, int64_t *rec_n) {
    int16_t buf[256];
    int blocks = (int)(secs * SR / 128);
    for (int b = 0; b < blocks; b++) {
        E->render(d, buf, 128);
        if (rec) { memcpy(rec + *rec_n * 2, buf, sizeof buf); *rec_n += 128; }
    }
}

static double rms(deck_t *d, double secs) {
    int16_t buf[256]; double s = 0; int64_t n = 0;
    for (int b = 0; b < (int)(secs * SR / 128); b++) {
        E->render(d, buf, 128);
        for (int i = 0; i < 256; i++) { s += (double)buf[i] * buf[i]; n++; }
    }
    return sqrt(s / n) / 32768.0;
}

static int wait_status(deck_t *d, const char *prefix, double secs) {
    char b[128]; int16_t buf[256];
    for (int i = 0; i < secs * 100; i++) {
        E->render(d, buf, 128);
        if (E->get_param(d, "status", b, sizeof b) > 0 && strstr(b, prefix)) return 1;
        usleep(10000);
    }
    return 0;
}

static int wait_loaded(deck_t *d, const char *name, double secs) {
    int16_t buf[256];
    for (int i = 0; i < secs * 100; i++) {
        E->render(d, buf, 128);
        if (d->t && !strcmp(d->t->name, name)) return 1;
        usleep(10000);
    }
    return 0;
}

static char g_dir[1024];   /* absolute test tracks folder */

static int wait_dir(deck_t *d, const char *dir) {
    for (int i = 0; i < 500; i++) {
        pthread_mutex_lock(&d->mu);
        int ok = d->bl && !strcmp(d->bl->dir, dir) && !(d->jobs & JOB_LIST);
        pthread_mutex_unlock(&d->mu);
        if (ok) return 1;
        usleep(10000);
    }
    return 0;
}

static int index_of(deck_t *d, const char *name) {
    pthread_mutex_lock(&d->mu);
    int r = -1;
    for (int i = 0; d->bl && i < d->bl->n; i++) if (!strcmp(d->bl->name[i], name)) { r = i; break; }
    pthread_mutex_unlock(&d->mu);
    return r;
}

static void select_and_load(deck_t *d, const char *name) {
    char v[16]; snprintf(v, sizeof v, "%d", index_of(d, name));
    E->set_param(d, "track", v);
    E->set_param(d, "load", "1"); E->set_param(d, "load", "0");
}

/* load a track through the browser: open the tracks folder, turn to the file, press LOAD */
static void load(deck_t *d, const char *name) {
    pthread_mutex_lock(&d->mu); br_open_dir(d, g_dir, NULL); pthread_mutex_unlock(&d->mu);
    wait_dir(d, g_dir);
    select_and_load(d, name);
}

static void tap_row(deck_t *d, int row) {
    char k[16]; snprintf(k, sizeof k, "br_%d", row);
    E->set_param(d, k, "1");
    E->set_param(d, k, "1");   /* MPC delivers one tap as two presses */
    usleep(300000);
}

static void note(deck_t *d, int n, int on) { uint8_t m[3] = { (uint8_t)(on ? 0x90 : 0x80), (uint8_t)n, (uint8_t)(on ? 100 : 0) }; E->midi(d, m, 3); }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "build/test_tracks";
    char p[1024];
    mkdir("build", 0755); mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/a_124.wav", dir); write_track(p, 124, 60, 0.30, 44100);
    snprintf(p, sizeof p, "%s/b_97_48k.wav", dir); write_track(p, 97, 40, 0.11, 48000);
    snprintf(p, sizeof p, "%s/c_172.wav", dir); write_track(p, 172, 40, 0.0, 44100);
    snprintf(p, sizeof p, "%s/._a_124.wav", dir); FILE *junk = fopen(p, "w"); fputs("not audio", junk); fclose(junk);
    mkdir("build/test_tracks/Sub Folder", 0755);
    if (!realpath(dir, g_dir)) return 1;
    setenv("DECKS_DIR", g_dir, 1);
    E = mpc_engine();

    deck_t *d = E->create(NULL);
    char b[128];
    /* browser: top level lists the tracks folder; open it with two row taps (select, then open) */
    CHECK(wait_dir(d, ""), "browser opens on the drives list");
    E->get_param(d, "br_path", b, sizeof b);
    CHECK(!strncmp(b, "DRIVES", 6), "top level is called DRIVES (%s)", b);
    int ti = index_of(d, "test_tracks");
    CHECK(ti >= 0, "drives list shows the tracks folder (row %d)", ti + 1);
    E->set_param(d, "br_open", "1"); E->set_param(d, "br_open", "0");
    CHECK(d->browse == 1, "BROWSE opens the browser");
    char b2[128]; snprintf(b2, sizeof b2, "br_%d_on", ti + 1); E->get_param(d, b2, b2, sizeof b2);
    CHECK(d->sel == ti && atoi(b2) == 1, "the first entry starts selected and its row reports _on");
    tap_row(d, ti + 1);
    CHECK(wait_dir(d, g_dir), "tapping the selected folder opens it (one tap, sent as two presses)");
    tap_row(d, 3);
    CHECK(d->sel == 2 && d->bl->dir[0], "tapping another row only selects it");
    pthread_mutex_lock(&d->mu); int nn = d->bl->n; pthread_mutex_unlock(&d->mu);
    E->get_param(d, "br_1", b, sizeof b);
    CHECK(nn == 4 && !strcmp(b, "Sub Folder  >"), "folder lists the subfolder first, then 3 tracks, no ._ file (%d, row 1 %s)", nn, b);
    E->get_param(d, "br_2", b, sizeof b);
    CHECK(!strcmp(b, "a_124"), "row 2 is a_124, without .wav (%s)", b);
    E->get_param(d, "br_path", b, sizeof b);
    CHECK(strstr(b, "test_tracks   (4)") != NULL && !strchr(b, '/'), "the path line reads with > and the item count (%s)", b);
    E->set_param(d, "track", "2");
    E->get_param(d, "track_display", b, sizeof b);
    CHECK(!strcmp(b, "b_97_48k.wav"), "the Browse knob selects and shows the file (%s)", b);
    E->set_param(d, "br_up", "1");
    CHECK(wait_dir(d, ""), "UP goes back to the drives");
    CHECK(d->sel == ti, "UP selects the folder it came from");
    tap_row(d, ti + 1);   /* already selected: opens */
    CHECK(wait_dir(d, g_dir), "tapping the selected folder opens it");
    tap_row(d, 2); tap_row(d, 2);
    CHECK(wait_loaded(d, "a_124.wav", 20) && d->browse == 0, "tapping a file twice loads it and closes the browser");

    /* tempo detection */
    struct { const char *name; double bpm, lead, rate; } tr[] = { {"a_124.wav", 124, 0.30, 44100}, {"b_97_48k.wav", 97, 0.11, 48000}, {"c_172.wav", 172, 0, 44100} };
    for (int k = 0; k < 3; k++) {
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        load(d, tr[k].name);
        int ok = wait_loaded(d, tr[k].name, 20);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        CHECK(ok, "%s loads (%.0f ms incl. analysis)", tr[k].name, ms);
        if (!ok) continue;
        double beat = 60.0 * SR / tr[k].bpm, want = fmod(tr[k].lead * SR, beat);
        double err = fabs(d->anchor - want); if (err > beat / 2) err = beat - err;
        CHECK(fabs(d->bpm - tr[k].bpm) < 0.1, "%s: detected %.2f BPM (true %.0f)", tr[k].name, d->bpm, tr[k].bpm);
        CHECK(err < 0.012 * SR, "%s: beat grid off by %.1f ms", tr[k].name, err * 1000 / SR);
    }
    double want_len = 40.0 * 44100;
    CHECK(fabs(d->t->frames - want_len) < 2, "48 kHz file resampled to 44.1 kHz length (%lld frames)", (long long)d->t->frames);

    /* play, pitch, stop */
    load(d, "a_124.wav"); wait_loaded(d, "a_124.wav", 20);
    double start = d->pos;
    CHECK(fabs(start - d->anchor) < 1, "loaded track is cued to its first beat");
    E->set_param(d, "play", "1");
    double r = rms(d, 2.0);
    CHECK(r > 0.05, "playing makes sound (rms %.3f)", r);
    CHECK(fabs(d->pos - start - 2.0 * SR) < 300, "2 s of play moves 2 s (%.0f frames)", d->pos - start);
    E->set_param(d, "pitch", "1");
    double p0 = d->pos; run(d, 1.0, NULL, NULL);
    CHECK(fabs((d->pos - p0) / (1.0 * (int)(SR / 128) * 128) - 1.08) < 0.002, "pitch +1 at 8%% range plays 1.08x (%.4f)", (d->pos - p0) / ((int)(SR / 128) * 128));
    E->get_param(d, "tempo", b, sizeof b);
    CHECK(!strcmp(b, "133.9 BPM"), "tempo readout follows pitch (%s)", b);
    E->set_param(d, "pitch", "0");

    /* hot cues: bank B pad 13 (base+28) is cue 1. An empty pad sets, a set pad jumps */
    note(d, 64, 1); note(d, 64, 0);
    double c0 = d->cues[0];
    CHECK(c0 > 0 && fabs(c0 - d->pos) < 1, "bank B pad 13 on an empty cue sets hot cue 1");
    run(d, 1.0, NULL, NULL);
    note(d, 64, 1); note(d, 64, 0);
    CHECK(fabs(d->pos - c0) < 1, "bank B pad 13 again jumps back to hot cue 1");
    note(d, 60, 1); note(d, 60, 0);   /* bank B pad 9 */
    CHECK(d->cues[4] >= 0 && d->bank == 1, "bank B pad 9 is hot cue 5, and the tiles switch to bank B");
    { char v[16]; E->get_param(d, "pad_13", v, sizeof v); CHECK(atoi(v) / 3 == PF_HC1, "bank B tile 13 is CUE 1 (%s)", v); }
    note(d, 52, 1); note(d, 52, 0);   /* bank B pad 1: a 1-beat loop */
    CHECK(d->loop_on && fabs(d->loop_b - d->loop_a - beat_len(d)) < 1, "bank B pad 1 sets a 1-beat loop");
    note(d, 52, 1); note(d, 52, 0);
    CHECK(!d->loop_on, "the same pad again ends it");
    E->set_param(d, "loop_len", "4");

    /* bottom row: CUE, PLAY, NUDGE -, NUDGE + */
    {
        double p1 = d->pos;
        note(d, 38, 1);   /* hold NUDGE - */
        run(d, 0.5, NULL, NULL);
        double held = (d->pos - p1) / (0.5 * SR);
        CHECK(held < 0.985 && held > 0.9 && d->bend < 0, "holding pad 3 slows the deck down (%.3fx)", held);
        note(d, 38, 0);
        CHECK(d->bend == 0 && d->hold_dir == 0, "letting go of pad 3 ends the bend");
        note(d, 39, 1); run(d, 1.5, NULL, NULL);
        CHECK(fabs(d->bend - 0.08) < 1e-9, "holding pad 4 speeds up, growing to 8%% (%.3f)", d->bend);
        note(d, 39, 0);
        note(d, 37, 1); note(d, 37, 0);
        CHECK(!d->playing, "pad 2 pauses");
        run(d, 0.1, NULL, NULL);   /* let the pause fade finish */
        double p2 = d->pos;
        note(d, 39, 1); run(d, 0.2, NULL, NULL);
        CHECK(fabs(d->pos - p2 - beat_len(d) / 32) < 1, "paused, a tap on pad 4 steps 1/32 beat (%.1f vs %.1f)", d->pos - p2, beat_len(d) / 32);
        run(d, 0.5, NULL, NULL); note(d, 39, 0);
        CHECK(d->pos - p2 > beat_len(d) / 32 * 3, "paused, holding pad 4 keeps stepping");
        note(d, 37, 1); note(d, 37, 0);
        CHECK(d->playing, "pad 2 plays again");
        double cue = d->cue;
        note(d, 36, 1); note(d, 36, 0); run(d, 0.1, NULL, NULL);
        CHECK(!d->playing && fabs(d->pos - cue) < 1, "pad 1 (CUE) while playing goes back to the cue and stops");
        note(d, 37, 1); note(d, 37, 0);
        jump(d, c0); d->park = -1;
        note(d, 40, 1); note(d, 40, 0);
        CHECK(d->sync == 1, "pad 5 turns sync on");
        note(d, 40, 1); note(d, 40, 0);
        CHECK(d->sync == 0, "pad 5 again turns it off");
        note(d, 50, 1); note(d, 50, 0);
        CHECK(d->loop_on && d->bank == 0, "pad 15 turns the loop on (and the tiles are back on bank A)");
        note(d, 50, 1); note(d, 50, 0);
        CHECK(!d->loop_on, "pad 15 again turns it off");
        /* BPM pads and SHIFT (pad 8) */
        E->set_param(d, "pitch", "0");
        double t0 = round(d->bpm * 10) / 10;
        note(d, 51, 1); note(d, 51, 0);   /* pad 16 */
        CHECK(fabs(d->bpm * (1 + d->pitch * 0.08) - (t0 + 0.1)) < 0.01, "pad 16 is BPM +0.1");
        note(d, 47, 1); note(d, 47, 0);   /* pad 12 */
        CHECK(fabs(d->bpm * (1 + d->pitch * 0.08) - t0) < 0.01, "pad 12 is BPM -0.1");
        note(d, 43, 1);                   /* hold SHIFT */
        { char v[16]; E->get_param(d, "pad_7", v, sizeof v); CHECK(atoi(v) / 3 == PF_DOUBLE, "with SHIFT held, tile 7 reads DOUBLE (%s)", v); }
        note(d, 51, 1); note(d, 51, 0);
        CHECK(fabs(d->bpm * (1 + d->pitch * 0.08) - (round(t0) + 1)) < 0.01, "SHIFT + pad 16 is BPM +1");
        int lb = d->loop_beats;
        note(d, 41, 1); note(d, 41, 0);   /* SHIFT + JUMP - */
        CHECK(d->loop_beats == lb - 1, "SHIFT + pad 6 halves the length");
        note(d, 43, 0);
        /* bank A pads 9-11: loops of 2, 4 and 8 bars */
        note(d, 44, 1); note(d, 44, 0);
        CHECK(d->loop_on && fabs(d->loop_b - d->loop_a - 8 * beat_len(d)) < 1, "bank A pad 9 sets a 2-bar loop");
        { char v[16]; E->get_param(d, "pad_9", v, sizeof v); CHECK(atoi(v) == PF_BAR1 * 3 + 1, "and its tile lights (%s)", v); }
        note(d, 46, 1); note(d, 46, 0);
        CHECK(d->loop_on && fabs(d->loop_b - d->loop_a - 32 * beat_len(d)) < 1, "pad 11 switches it to 8 bars");
        note(d, 46, 1); note(d, 46, 0);
        CHECK(!d->loop_on, "pad 11 again ends it");
        d->cues[0] = c0; d->loop_beats = lb;
        E->set_param(d, "pitch", "0");
    }
    E->get_param(d, "pads", b, sizeof b);
    CHECK(!strncmp(b, "1---5---", 8), "pads readout shows the set cue (%s)", b);

    /* loop roll with slip: playback repeats, the track position keeps moving, release resumes there */
    run(d, 0.3, NULL, NULL);
    double before = d->pos;
    note(d, 57, 1);   /* bank B pad 6: the 1/4-beat roll */  /* pads 9-12 set to rolls: pad 10 is the 1/4-beat roll */
    double ra = d->roll_a, rb = d->roll_b;
    run(d, 1.5, NULL, NULL);
    CHECK(d->pos >= ra - 1 && d->pos <= rb + 1, "during a roll the playhead stays inside the 1/4 beat");
    note(d, 57, 0);
    double expect = before + 1.5 * SR;
    CHECK(fabs(d->pos - expect) < 300, "after the roll it resumes where the track would be (off by %.0f frames)", d->pos - expect);

    /* loop: 4 beats, stays inside */
    E->set_param(d, "loop_len", "4");
    E->set_param(d, "loop", "1");
    double la = d->loop_a, lb = d->loop_b;
    CHECK(fabs((lb - la) - 4 * 60.0 * SR / d->bpm) < 1, "4-beat loop is 4 beats long");
    int inside = 1;
    for (int i = 0; i < 400; i++) { run(d, 0.025, NULL, NULL); if (d->pos < la - 1 || d->pos > lb + 300) inside = 0; }
    CHECK(inside, "10 s of a 4-beat loop stays inside it");
    E->set_param(d, "loop", "0");

    /* beat jump */
    double bj = d->pos; note(d, 42, 1); note(d, 42, 0);   /* pad 7: forward by the loop length */
    CHECK(fabs(d->pos - bj - 4 * 60.0 * SR / d->bpm) < 1, "beat jump forward moves 4 beats");

    /* sync to the MPC at 128 BPM */
    E->set_param(d, "sync", "1");
    E->song_pos(d, 10.0, 128.0, 1, 128);
    { int16_t bb[256]; E->render(d, bb, 128); }
    E->get_param(d, "tempo", b, sizeof b);
    CHECK(!strncmp(b, "128.", 4) && strstr(b, "SYNC"), "sync follows the MPC tempo (%s)", b);
    /* phase lock: host at a beat boundary, deck a quarter beat off; after a few seconds the phases meet */
    double ppq = 100.0;
    double dph = beat_at(d, d->pos); jump(d, frame_of_beat(d, floor(dph) + 0.25));
    for (int i = 0; i < 6 * SR / 128; i++) {
        E->song_pos(d, ppq, 128.0, 1, 128);
        int16_t buf[256]; E->render(d, buf, 128);
        ppq += 128 * 128.0 / 60 / SR;
    }
    double err = (ppq - floor(ppq)) - (beat_at(d, d->pos) - floor(beat_at(d, d->pos)));
    if (err > 0.5) err -= 1; else if (err < -0.5) err += 1;
    CHECK(fabs(err) < 0.02, "phase lock pulls a quarter-beat offset in within 6 s (%.3f beats left)", err);
    E->set_param(d, "sync", "0");

    /* cue: playing -> back to cue point and stop */
    E->set_param(d, "cue", "1");
    run(d, 0.1, NULL, NULL);
    CHECK(!d->playing && fabs(d->pos - d->cue) < 1, "cue while playing returns to the cue point and stops");

    /* waveform overview: 80 columns with levels, playhead column marked, played columns before it */
    {
        int lv = 0, ok = 1;
        for (int c = 0; c < WF_COLS; c++) lv += d->t->wf[c];
        char k[16];
        int col = d->wf_col;
        snprintf(k, sizeof k, "wf_%d", col + 1); E->get_param(d, k, b, sizeof b);
        if (atoi(b) / WF_LEVELS != 2) ok = 0;
        if (col > 0) { snprintf(k, sizeof k, "wf_%d", col); E->get_param(d, k, b, sizeof b); if (atoi(b) / WF_LEVELS != 1) ok = 0; }
        snprintf(k, sizeof k, "wf_%d", WF_COLS); E->get_param(d, k, b, sizeof b); if (atoi(b) / WF_LEVELS != 0) ok = 0;
        CHECK(lv > WF_COLS * 8 && ok, "waveform has levels (avg %.1f) and marks played / playhead / to play", lv / (double)WF_COLS);
    }
    /* beat counter */
    E->get_param(d, "beat", b, sizeof b);
    CHECK(atoi(b) >= 1 && atoi(b) <= 4, "beat counter shows a beat of the bar (%s)", b);

    /* nudge: a tap speeds up 4% for a moment, then back */
    E->set_param(d, "play", "1"); E->set_param(d, "sync", "0"); run(d, 0.1, NULL, NULL);
    E->set_param(d, "nudge_up", "1");
    double n0 = d->pos; run(d, 0.1, NULL, NULL);
    double sp = (d->pos - n0) / ((int)(0.1 * SR / 128) * 128);
    run(d, 0.3, NULL, NULL);
    double n1 = d->pos; run(d, 0.1, NULL, NULL);
    double sp2 = (d->pos - n1) / ((int)(0.1 * SR / 128) * 128);
    CHECK(fabs(sp - 1.04) < 0.003 && fabs(sp2 - 1.0) < 0.001, "NUDGE + runs 4%% fast briefly, then normal (%.3f, %.3f)", sp, sp2);
    /* jog while playing bends, while paused scrubs */
    E->set_param(d, "jog", "0.015625");
    CHECK(d->bend > 0.015, "jog while playing bends the speed (%.3f)", d->bend);
    E->get_param(d, "jog", b, sizeof b);
    CHECK(!strcmp(b, "0"), "jog always reads back 0, so every step is relative");
    E->set_param(d, "play", "0"); run(d, 0.1, NULL, NULL);
    d->jog_last = 0;
    double j0 = d->pos; E->set_param(d, "jog", "-0.015625");
    CHECK(fabs((j0 - d->pos) - beat_len(d) / 32) < 1, "jog while paused scrubs back 1/32 beat");
    {   /* the wheel as MPC sends it: a running total -0.02, -0.04, ... each click is one step, not a growing one */
        d->jog_last = 0;
        double p0 = d->pos;
        for (int i = 1; i <= 10; i++) { char v[16]; snprintf(v, sizeof v, "%g", -0.02 * i); E->set_param(d, "jog", v); }
        CHECK(fabs((p0 - d->pos) - 10 * beat_len(d) / 32) < 1, "10 wheel clicks back scrub 10 steps (%.1f steps)", (p0 - d->pos) / (beat_len(d) / 32));
        p0 = d->pos;
        for (int i = 9; i >= 7; i--) { char v[16]; snprintf(v, sizeof v, "%g", -0.02 * i); E->set_param(d, "jog", v); }
        CHECK(fabs((d->pos - p0) - 3 * beat_len(d) / 32) < 1, "turning back the other way steps forward (%.1f steps)", (d->pos - p0) / (beat_len(d) / 32));
    }
    /* pitch reset */
    E->set_param(d, "pitch", "0.5"); E->set_param(d, "pitch_reset", "1");
    CHECK(d->pitch == 0, "RESET centres the pitch");

    /* MPC transport: PLAY plays from here, STOP pauses, PLAY START plays from the cue; a looping sequence changes nothing */
    E->set_param(d, "follow", "1");
    E->song_pos(d, 8.0, 124.0, 0, 128); run(d, 0.05, NULL, NULL);
    double h0 = d->pos;
    E->song_pos(d, 8.0, 124.0, 1, 128); run(d, 0.2, NULL, NULL);
    CHECK(d->playing && d->pos > h0 + 0.15 * SR, "MPC PLAY starts the deck where it was");
    E->song_pos(d, 9.0, 124.0, 1, 128);
    E->song_pos(d, 0.0, 124.0, 1, 128);   /* the MPC's sequence loops back while playing */
    run(d, 0.05, NULL, NULL);
    CHECK(d->playing && d->pos > h0 + 0.15 * SR, "the MPC looping its sequence doesn't jump the deck");
    E->song_pos(d, 1.0, 124.0, 0, 128); run(d, 0.05, NULL, NULL);
    CHECK(!d->playing, "MPC STOP pauses the deck");
    E->song_pos(d, 0.0, 124.0, 1, 128); run(d, 0.01, NULL, NULL);
    CHECK(d->playing && fabs(d->pos - d->cue) < 0.02 * SR, "MPC PLAY START plays from the cue point");
    /* phase meter: deck on the MPC's beat reads 17 (centre) */
    {
        double ppq = 32.0;
        E->set_param(d, "sync", "1");
        for (int i = 0; i < 3 * SR / 128; i++) { E->song_pos(d, ppq, 124.0, 1, 128); int16_t bb[256]; E->render(d, bb, 128); ppq += 128 * 124.0 / 60 / SR; }
        E->get_param(d, "phase", b, sizeof b);
        CHECK(abs(atoi(b) - 17) <= 1, "phase meter centres once synced (%s)", b);
        E->set_param(d, "sync", "0");
        E->song_pos(d, ppq, 124.0, 0, 128);
    }
    E->set_param(d, "follow", "0");
    E->set_param(d, "play", "1");

    /* two decks in one process: letters, the first deck played leads, the other syncs to it */
    {
        E->set_param(d, "sync", "0");
        set_leader(NULL);
        deck_t *b2 = E->create(NULL);
        CHECK(d->deck_id == 0 && b2->deck_id == 1, "decks get letters A and B (%d, %d)", d->deck_id, b2->deck_id);
        E->set_param(d, "play", "0"); run(d, 0.05, NULL, NULL);
        load(b2, "b_97_48k.wav"); wait_loaded(b2, "b_97_48k.wav", 20);
        E->set_param(d, "pitch", "0"); E->set_param(d, "range", "1"); E->set_param(d, "pitch", "0.25");   /* A at 124 * 1.04 */
        E->set_param(d, "play", "1");
        E->get_param(d, "deck_label", b, sizeof b);
        CHECK(!strcmp(b, "DECK A  LEADER"), "the first deck to play becomes the leader (%s)", b);
        E->set_param(b2, "sync", "1"); E->set_param(b2, "play", "1");
        int16_t bb[256];
        for (int i = 0; i < 4 * SR / 128; i++) { E->render(d, bb, 128); E->render(b2, bb, 128); }
        double ta = d->bpm * rate_now(d), tb = b2->bpm * rate_now(b2);
        E->get_param(b2, "tempo", b, sizeof b);
        CHECK(fabs(ta - tb) < 0.3 && strstr(b, "SYNC A"), "deck B syncs to deck A's tempo (%.2f vs %.2f, %s)", tb, ta, b);
        double e = beat_at(d, d->pos) - beat_at(b2, b2->pos); e -= floor(e + 0.5);
        CHECK(fabs(e) < 0.03, "and to its beat (%.3f beats apart)", e);
        E->get_param(b2, "phase", b, sizeof b);
        CHECK(abs(atoi(b) - 34 - 17) <= 1, "deck B's phase meter is centred on deck A (%s)", b);
        E->get_param(b2, "wfb_1", b, sizeof b);
        CHECK(atoi(b) / (3 * WF_LEVELS) == 1, "deck B's waveform uses deck B's colour frames (%s)", b);
        E->get_param(b2, "deck_color", b, sizeof b);
        CHECK(!strcmp(b, "1"), "deck B's platter colour is B's (%s)", b);
        E->get_param(b2, "pitch_pct", b, sizeof b);
        CHECK(strstr(b, "%") && b[0] == (tb >= b2->bpm ? '+' : '-'), "pitch readout in percent (%s)", b);
        /* deck A's screen controls deck B */
        E->set_param(d, "target", "1");
        E->get_param(d, "target_label", b, sizeof b);
        CHECK(!strcmp(b, "DECK B  (FROM A)"), "CONTROL B on deck A's screen shows deck B (%s)", b);
        E->get_param(d, "title", b, sizeof b);
        CHECK(!strcmp(b, "b_97_48k.wav"), "deck A's screen reads deck B's track (%s)", b);
        E->set_param(d, "pitch", "0.5");
        E->get_param(d, "pitch", b, sizeof b);
        CHECK(atof(b) == 0.5 && b2->pitch == 0, "a control sent to B reads back at once, and waits for B's audio block");
        E->set_param(d, "play", "0");
        CHECK(b2->playing, "B keeps playing until its next block");
        E->render(b2, bb, 128); E->render(d, bb, 128);
        CHECK(!b2->playing && b2->pitch == 0.5f && d->playing && d->pitch == 0.25f, "B pauses and takes the pitch; A is untouched");
        uint8_t on[3] = {0x90, 65, 100}; E->midi(d, on, 3); E->render(b2, bb, 128);
        CHECK(b2->cues[1] >= 0 && d->cues[1] < 0, "deck A's pads play deck B while CONTROL is B");
        int sl = E->get_param(d, "state", b, 0); (void)sl;
        static char sa[8192]; E->get_param(d, "state", sa, sizeof sa);
        CHECK(strstr(sa, "name=a_124.wav") != NULL, "deck A still saves its own track in the project");
        E->set_param(d, "target", "0");
        E->get_param(d, "title", b, sizeof b);
        CHECK(!strcmp(b, "a_124.wav"), "CONTROL A goes back to deck A (%s)", b);
        E->set_param(b2, "play", "1");
        E->set_param(b2, "leader", "1");
        char la[16], lb2[16]; E->get_param(d, "leader", la, sizeof la); E->get_param(b2, "leader", lb2, sizeof lb2);
        CHECK(atoi(la) == 0 && atoi(lb2) == 1, "LEADER on deck B takes over from A");
        /* the header shows both tempos, whichever deck the screen is on */
        {
            char ba[16], bb2[16], a2[16];
            E->get_param(d, "bpm_a", ba, sizeof ba); E->get_param(d, "bpm_b", bb2, sizeof bb2);
            E->get_param(b2, "bpm_a", a2, sizeof a2);
            char wa[16], wb[16]; get_local(d, "bpm_big", wa, sizeof wa); get_local(b2, "bpm_big", wb, sizeof wb);
            CHECK(!strcmp(ba, wa) && !strcmp(bb2, wb) && !strcmp(a2, wa), "both screens show A's and B's tempo (%s / %s)", ba, bb2);
            char w1[16], w2[16], w3[16], w4[16];
            E->get_param(d, "wfb_40", w1, sizeof w1); get_local(b2, "wf_40", w2, sizeof w2);
            E->get_param(b2, "wf_40", w3, sizeof w3); get_local(d, "wf_40", w4, sizeof w4);
            CHECK(!strcmp(w1, w2) && !strcmp(w3, w4) && atoi(w1) >= 3 * WF_LEVELS,
                  "both screens show deck A's waveform on top and deck B's below (%s, %s)", w3, w1);
        }
        /* letters stay unique: deck B asking for A swaps with deck A */
        E->set_param(b2, "deck_id", "0");
        CHECK(b2->deck_id == 0 && d->deck_id == 1, "deck B taking letter A swaps the two letters");
        E->set_param(b2, "deck_id", "1");
        CHECK(b2->deck_id == 1 && d->deck_id == 0, "and back");
        E->set_param(d, "target", "0"); E->set_param(b2, "target", "1");
        /* pads 13 / 14 pick the deck, and its tile lights */
        {
            uint8_t p14[3] = {0x90, 49, 100}, p14o[3] = {0x80, 49, 0}, p13[3] = {0x90, 48, 100}, p13o[3] = {0x80, 48, 0};
            E->set_param(d, "target", "0");
            E->midi(d, p14, 3); E->midi(d, p14o, 3);
            char v[16]; E->get_param(d, "pad_14", v, sizeof v);
            CHECK(d->target == 1 && atoi(v) == PF_DECKB * 3 + 1, "pad 14 switches to deck B and its tile lights (%s)", v);
            E->midi(d, p13, 3); E->midi(d, p13o, 3);
            CHECK(d->target == 0, "pad 13 switches back to deck A");
            E->set_param(d, "target", "1"); E->set_param(d, "tp_13", "1");
            CHECK(d->target == 0, "a tap on the DECK A tile switches too");
        }
        /* STOP's parameter-0 toggle on deck A's screen while it shows B must not touch B's output level */
        {
            E->set_param(d, "target", "1");
            float bv = b2->vol, av = d->vol;
            E->set_param(d, "volume", "1"); E->set_param(d, "volume", "0");
            E->render(b2, bb, 128);
            CHECK(b2->vol == bv && d->vol == 0.0f, "volume on A's screen stays A's own, even showing B");
            E->set_param(d, "volume", "1"); (void)av;
            E->set_param(d, "show_a", "1");
            CHECK(d->target == 0, "the A switch shows deck A");
            E->set_param(d, "show_b", "1");
            CHECK(d->target == 1, "the B switch shows deck B");
            E->set_param(d, "target", "0");
            /* BPM + / -: 0.1 BPM a tap */
            E->set_param(d, "sync", "0");
            E->set_param(d, "pitch", "0");
            E->set_param(d, "tempo_up", "1"); E->set_param(d, "tempo_up", "1");
            char t1[32]; E->get_param(d, "bpm_big", t1, sizeof t1);
            double want = round(d->bpm * 10) / 10 + 0.2;
            CHECK(fabs(atof(t1) - want) < 0.051, "two taps on BPM + add 0.2 BPM (%s, want %.1f)", t1, want);
            E->set_param(d, "tempo_dn", "1");
            E->get_param(d, "bpm_big", t1, sizeof t1);
            CHECK(fabs(atof(t1) - (want - 0.1)) < 0.051, "BPM - takes 0.1 off (%s)", t1);
            E->get_param(d, "tempo_sub", t1, sizeof t1);
            CHECK(t1[0] == '+' && strstr(t1, "8%"), "the line under the BPM shows pitch and range (%s)", t1);
            E->set_param(d, "pitch", "0.25");
        }
        /* pad tiles, screen taps, a release that follows its press across a CONTROL switch */
        {
            char v[16];
            E->get_param(d, "pad_2", v, sizeof v);
            CHECK(atoi(v) == 1 * 3 + 1, "the PLAY tile is lit while deck A plays (%s)", v);
            uint8_t n3[3] = {0x90, 38, 100}, n3off[3] = {0x80, 38, 0};
            E->midi(d, n3, 3);
            E->get_param(d, "pad_3", v, sizeof v);
            CHECK(atoi(v) == 2 * 3 + 2, "a held pad shows pressed (%s)", v);
            E->set_param(d, "target", "1");
            E->midi(d, n3off, 3);
            E->set_param(d, "target", "0");
            CHECK(d->hold_dir == 0 && !(d->pad_held & 4), "its release reaches deck A after switching CONTROL to B");
            E->set_param(d, "tp_5", "1");
            CHECK(d->sync == 1, "a tap on the SYNC tile turns sync on");
            E->get_param(d, "pad_5", v, sizeof v);
            CHECK(atoi(v) == 4 * 3 + 2, "and lights it for a moment (%s)", v);
            for (int i = 0; i < 60; i++) E->render(d, bb, 128);   /* 0.17 s */
            E->get_param(d, "pad_5", v, sizeof v);
            CHECK(atoi(v) == 4 * 3 + 1, "then it stays on, not pressed (%s)", v);
            E->set_param(d, "tp_5", "1");
            /* learn: a scale pad mode sending 60, 62, 64 ... */
            E->set_param(d, "pad_learn", "1");
            E->get_param(d, "pad_1", v, sizeof v);
            CHECK(atoi(v) == 2, "learning lights pad 1 to hit first (%s)", v);
            static const int scale[16] = {60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79, 81, 83, 84, 86};
            int was_playing = d->playing;
            for (int k = 0; k < 16; k++) {
                uint8_t on1[3] = {0x90, (uint8_t)scale[k], 90}, off1[3] = {0x80, (uint8_t)scale[k], 0};
                E->midi(d, on1, 3); E->midi(d, off1, 3);
                if (k == 3) { E->midi(d, on1, 3); E->midi(d, off1, 3); }   /* a double hit is ignored */
            }
            E->get_param(d, "learn_label", b, sizeof b);
            CHECK(g_learn == 17 && strstr(b, "BANK B, HIT PAD 1"), "after bank A it asks for bank B (%s)", b);
            E->set_param(d, "pad_learn", "1");   /* stop there: bank B keeps its notes */
            E->get_param(d, "learn_label", b, sizeof b);
            CHECK(!g_learn && g_padnote[15] == 86 && g_padnote[4] == 67 && d->playing == was_playing,
                  "16 hits learn the pad notes without playing anything (%s)", b);
            uint8_t p2[3] = {0x90, 62, 100}, p2off[3] = {0x80, 62, 0};
            E->midi(d, p2, 3); E->midi(d, p2off, 3);
            CHECK(d->playing != was_playing, "then pad 2 (note 62) is PLAY/PAUSE");
            E->midi(d, p2, 3); E->midi(d, p2off, 3);
            E->get_param(d, "state", sa, sizeof sa);
            CHECK(strstr(sa, "pmap=60,62,64") != NULL, "the learned pads are saved with the project");
            E->set_param(d, "pad_base", "36");
            CHECK(g_padnote[0] == 36 && g_padnote[15] == 51, "PAD BASE puts the pads back on chromatic notes");
            restore_state(d, sa);
            CHECK(g_padnote[1] == 62, "a saved learned map comes back after PAD BASE on restore");
            E->set_param(d, "pad_base", "36");
        }
        /* browser: the relative wheel scroll, LOAD B from deck A's browser */
        {
            pthread_mutex_lock(&d->mu); br_open_dir(d, g_dir, NULL); pthread_mutex_unlock(&d->mu);
            wait_dir(d, g_dir);
            E->set_param(d, "track", "0");
            d->scroll_last = 0;
            E->set_param(d, "br_scroll", "0.02"); E->set_param(d, "br_scroll", "0.04");
            CHECK(d->sel == 2, "two wheel clicks on SCROLL move two rows (%d)", d->sel);
            E->set_param(d, "br_scroll", "0.02");
            CHECK(d->sel == 1, "one back moves up a row (%d)", d->sel);
            char v[16]; E->get_param(d, "br_scroll", v, sizeof v);
            CHECK(atof(v) == 0, "SCROLL reads back 0, so the wheel never runs out");
            int i = index_of(d, "b_97_48k.wav");
            snprintf(v, sizeof v, "%d", i); E->set_param(d, "track", v);
            E->set_param(b2, "pitch", "0");
            load(b2, "a_124.wav"); wait_loaded(b2, "a_124.wav", 20);
            E->set_param(d, "load_b", "1");
            CHECK(wait_loaded(b2, "b_97_48k.wav", 20) && !strcmp(d->t->name, "a_124.wav"), "LOAD B on deck A's browser loads deck B");
            /* browser pads: pad 14 UP, pad 10 DOWN (repeats while held), pad 1 LOAD A; deck pads do nothing meanwhile */
            E->set_param(d, "br_open", "1");
            E->set_param(d, "track", "0");
            uint8_t dn[3] = {0x90, 45, 100}, dnoff[3] = {0x80, 45, 0}, upn[3] = {0x90, 49, 100}, upoff[3] = {0x80, 49, 0};
            E->midi(d, dn, 3); E->midi(d, dnoff, 3);
            CHECK(d->sel == 1, "pad 10 moves the browser down a row (%d)", d->sel);
            E->midi(d, upn, 3); E->midi(d, upoff, 3);
            CHECK(d->sel == 0, "pad 14 moves it up (%d)", d->sel);
            char pv[16]; E->get_param(d, "bpad_10", pv, sizeof pv);
            CHECK(atoi(pv) / 3 == PF_B_DOWN, "the browser's pad map shows the browser functions (%s)", pv);
            E->get_param(d, "pad_10", pv, sizeof pv);
            CHECK(atoi(pv) == PAD_BLANK, "and the big PLAY tiles go empty under the browser (%s)", pv);
            E->midi(d, dn, 3);
            for (int r = 0; r < (int)(SR * 0.65 / 128); r++) E->render(d, bb, 128);
            E->midi(d, dnoff, 3);
            int n = d->bl ? d->bl->n : 0;
            CHECK(d->sel >= (n > 4 ? 4 : n - 1), "holding pad 10 keeps moving down (%d of %d)", d->sel, n);
            int was_playing = d->playing;
            i = index_of(d, "b_97_48k.wav"); snprintf(v, sizeof v, "%d", i); E->set_param(d, "track", v);
            E->get_param(d, "bpad_2", pv, sizeof pv);
            CHECK(atoi(pv) == PF_B_LOADB * 3 + 1, "LOAD B is lit when a file is selected (%s)", pv);
            uint8_t p2[3] = {0x90, 37, 100}, p2off[3] = {0x80, 37, 0};
            E->midi(d, p2, 3);   /* pad 2 is LOAD B here, not PLAY */
            CHECK(!d->browse && d->playing == was_playing, "pad 2 loads onto deck B and closes the browser, no play/pause");
            E->midi(d, p2off, 3);
            CHECK(!d->pad_held && d->playing == was_playing, "its release, after the browser closed, doesn't reach PLAY");
        }
        /* browser rows: on a deck, played, selected; EJECT off a drive */
        {
            pthread_mutex_lock(&d->mu); br_open_dir(d, g_dir, NULL); pthread_mutex_unlock(&d->mu);
            wait_dir(d, g_dir);
            int ia = index_of(d, "a_124.wav"), ib = index_of(d, "b_97_48k.wav");
            char v[16]; snprintf(v, sizeof v, "%d", ia); E->set_param(d, "track", v);
            int ra = ia - d->top + 1, rb = ib - d->top + 1;
            char k[16], t[300];
            snprintf(k, sizeof k, "br_%d", ra); E->get_param(d, k, t, sizeof t);
            CHECK(!strcmp(t, "a_124"), "the row shows the name alone (%s)", t);
            snprintf(k, sizeof k, "brs_%d", ra); E->get_param(d, k, t, sizeof t);
            CHECK(atoi(t) == 4, "and is shaded blue, selected (%s)", t);
            if (strcmp(b2->t->name, "b_97_48k.wav")) {
                pthread_mutex_lock(&g_mu); char pth[1100]; snprintf(pth, sizeof pth, "%s/b_97_48k.wav", g_dir); played_add(pth); pthread_mutex_unlock(&g_mu);
                snprintf(k, sizeof k, "br_%d", rb); E->get_param(d, k, t, sizeof t);
                char k2[16], t2[16]; snprintf(k2, sizeof k2, "brd_%d", rb); E->get_param(d, k2, t2, sizeof t2);
                CHECK(!strchr(t, 0x5b) && atoi(t2) == 1, "a played track is greyed, the name alone (%s, veil %s)", t, t2);
            }
            /* deck A has played well over 30 s: the next poll puts it on the played list */
            d->play_frames = 31 * SR; d->played_marked = 0;
            char r2[16]; E->get_param(d, "display_rev", r2, sizeof r2);
            CHECK(d->played_marked, "30 s of play puts a track on the played list");
            snprintf(k, sizeof k, "brt_%d", rb); E->set_param(d, k, "1");
            CHECK(d->sel == ib, "a tap on a row selects it");
            usleep(300000);
            E->set_param(d, k, "0");   /* the release of a long press */
            usleep(50000);
            CHECK(d->browse && !(d->jobs & JOB_LOAD) && strcmp(d->load_path, "") >= 0 && d->sel == ib, "releasing a long press doesn't act as a second tap");
            E->set_param(d, "target", "1");   /* deck B's rows, read from deck A's screen (the lock that is already held) */
            snprintf(k, sizeof k, "brs_%d", 1); E->get_param(d, k, t, sizeof t);
            E->set_param(d, "target", "0");
            CHECK(1, "deck A's screen reads deck B's rows without locking up");
            E->set_param(d, "br_eject", "1");
            E->get_param(d, "br_path", t, sizeof t);
            CHECK(strstr(t, "PICK A DRIVE") != NULL, "EJECT off a drive asks for one (%s)", t);
            d->warn_until = 0;
        }
        /* a playing deck needs LOAD pressed twice; pad pressure never blocks a release */
        {
            pthread_mutex_lock(&d->mu); br_open_dir(d, g_dir, NULL); pthread_mutex_unlock(&d->mu);
            wait_dir(d, g_dir);
            int i = index_of(d, "c_172.wav"); char v[16]; snprintf(v, sizeof v, "%d", i); E->set_param(d, "track", v);
            for (int w = 0; w < 500 && (b2->loading || b2->ready || (b2->jobs & JOB_LOAD)); w++) { E->render(b2, bb, 128); usleep(10000); }
            E->render(b2, bb, 128);   /* let an earlier test's reload of deck B land first */
            E->set_param(b2, "play", "1"); E->render(b2, bb, 128);
            CHECK(b2->playing, "deck B playing for the load guard");
            E->set_param(d, "load_b", "1");
            usleep(200000); for (int r = 0; r < 5; r++) E->render(b2, bb, 128);
            char w[96]; E->get_param(d, "br_path", w, sizeof w);
            CHECK(strcmp(b2->t->name, "c_172.wav") && strstr(w, "PRESS LOAD B AGAIN"), "one LOAD B onto a playing deck B only warns (%s)", w);
            E->set_param(d, "load_b", "1");
            CHECK(wait_loaded(b2, "c_172.wav", 20), "a second LOAD B within 3 s loads it");
            /* a flood of pad pressure into the other deck's queue, then the release: the hold must end */
            E->set_param(d, "target", "1");
            E->set_param(b2, "play", "1"); E->render(b2, bb, 128);
            uint8_t on3[3] = {0x90, 38, 100}, off3[3] = {0x80, 38, 0}, at[3] = {0xA0, 38, 50};
            E->midi(d, on3, 3);
            for (int r = 0; r < 200; r++) E->midi(d, at, 3);
            E->midi(d, off3, 3);
            E->render(b2, bb, 128);
            CHECK(b2->hold_dir == 0 && !(b2->pad_held & 4), "200 pressure messages don't push the pad's release out of the queue");
            E->set_param(d, "target", "0");
            /* end of track: the platter ring flashes */
            E->set_param(b2, "loop", "0");
            jump(b2, b2->t->frames - 20.0 * SR); b2->park = -1;
            int seen_warn = 0;
            for (int r = 0; r < (int)(SR * 0.6 / 128); r++) { E->render(b2, bb, 128); char c2[8]; E->get_param(b2, "deck_color", c2, sizeof c2); if (atoi(c2) == 3) seen_warn = 1; }
            CHECK(seen_warn, "the last 30 s flash deck B's platter ring amber");
            E->set_param(b2, "play", "0");
        }
        E->set_param(d, "target", "1");
        E->destroy(b2);
        CHECK(__atomic_load_n(&g_leader, __ATOMIC_ACQUIRE) == NULL, "closing the leader deck clears the leader");
        E->get_param(d, "target_label", b, sizeof b);
        char tt[64]; E->get_param(d, "title", tt, sizeof tt);
        CHECK(d->target == 0 && strstr(tt, "NO DECK B"), "with deck B gone, deck A's screen goes back to deck A and says so (%s)", tt);
        E->get_param(d, "target_label", b, sizeof b);
        CHECK(!strcmp(b, "DECK A"), "and the header is deck A's again (%s)", b);
        E->set_param(d, "target", "0");
        E->set_param(d, "pitch", "0"); E->set_param(d, "range", "0");
    }

    /* project save / restore */
    E->set_param(d, "volume", "0.5"); E->set_param(d, "eq_low", "-1"); E->set_param(d, "pad_base", "48");
    d->bpm = 124.5;   /* a user-corrected tempo is kept */
    static char st[8192];
    int sl = E->get_param(d, "state", st, sizeof st);
    CHECK(sl > 0, "state saved (%d bytes)", sl);
    deck_t *d2 = E->create(NULL);
    E->set_param(d2, "state", st);
    int ok = wait_loaded(d2, "a_124.wav", 20);
    CHECK(ok, "restore reloads the track by name before the scan finished");
    CHECK(ok && fabs(d2->cues[0] - c0) < 1 && d2->cues[1] < 0, "restore brings back the hot cues (%.0f vs %.0f, %.0f)", d2->cues[0], c0, d2->cues[1]);
    CHECK(d2->vol == 0.5f && d2->eq[0] == -1.0f && d2->pad_base == 48, "restore brings back the settings");
    CHECK(fabs(d2->bpm - 124.5) < 0.01, "restore keeps the corrected tempo (%.2f)", d2->bpm);
    E->destroy(d2);

    /* a project whose track moved: the saved path fails, a search by name finds it */
    deck_t *d4 = E->create(NULL);
    E->set_param(d4, "state", "decks1\nname=c_172.wav\npath=/nowhere/c_172.wav\ndir=\nc0=1000.0\n");
    CHECK(wait_loaded(d4, "c_172.wav", 20) && fabs(d4->cues[0] - 1000) < 1, "a moved track is found by name, cues kept");
    E->destroy(d4);

    /* missing track */
    deck_t *d3 = E->create(NULL);
    E->set_param(d3, "state", "decks1\nname=gone.wav\nvolume=0.3\n");
    CHECK(wait_status(d3, "Missing", 5), "a missing track shows as missing, settings still restored (vol %.1f)", d3->vol);
    E->destroy(d3);

    /* demo: 8 bars, a roll, filter sweep, cue jump */
    E->set_param(d, "volume", "0.8"); E->set_param(d, "eq_low", "0");
    load(d, "a_124.wav"); wait_loaded(d, "a_124.wav", 20);
    int16_t *rec = calloc((size_t)SR * 40 * 2, sizeof(int16_t)); int64_t rn = 0;
    double bar = 4 * 60.0 / 124;
    E->set_param(d, "play", "1"); run(d, bar * 2, rec, &rn);
    note(d, 56, 1); run(d, bar / 2, rec, &rn); note(d, 56, 0);   /* bank B pad 5: the 1/8-beat roll */
    run(d, bar * 1.5, rec, &rn);
    for (int i = 0; i <= 40; i++) { char v[16]; snprintf(v, sizeof v, "%g", -0.9 * i / 40); E->set_param(d, "filter", v); run(d, bar * 2 / 40, rec, &rn); }
    E->set_param(d, "filter", "0");
    E->set_param(d, "eq_low", "-1"); run(d, bar, rec, &rn); E->set_param(d, "eq_low", "0");
    note(d, 64, 1); note(d, 64, 0); run(d, bar * 2, rec, &rn);
    drwav_data_format f = { drwav_container_riff, DR_WAVE_FORMAT_PCM, 2, SR, 16 };
    drwav w; drwav_init_file_write(&w, "build/demo.wav", &f, NULL); drwav_write_pcm_frames(&w, rn, rec); drwav_uninit(&w);
    free(rec);
    E->destroy(d);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
