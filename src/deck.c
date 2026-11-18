/* mpSEEJAY (called Decks until 0.8, MPCjay in 0.9): one DJ deck as an MPC OS plugin (wrapper/engine.h contract: 44.1 kHz, int16 stereo, blocks of up to 128 frames).
 *
 * Plays WAV and MP3 files from a Tracks folder: play/pause, CDJ-style cue, 8 hot cues, pitch fader, tempo sync to the MPC,
 * beat-quantized loop and loop rolls (slip: the track keeps moving underneath and resumes where it would have been),
 * beat jumps, 3-band EQ and a one-knob DJ filter. Pads drive it by MIDI note (pad_base .. pad_base+15).
 *
 * Threads: a worker thread scans folders and decodes/analyses tracks. render() never blocks: it takes a finished track
 * with pthread_mutex_trylock and hands the old one back to the worker to free. */
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "engine.h"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO_WIDE
#include "vendor/dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "vendor/dr_mp3.h"

#define SR 44100
#define NCUES 8
#define XFADE 256                    /* frames of crossfade on every jump */
#ifdef MAX_SECONDS_OVERRIDE
#define MAX_SECONDS (MAX_SECONDS_OVERRIDE)   /* tests */
#else
#define MAX_SECONDS (15 * 60)        /* longest track loaded (int16 stereo: ~160 MB) */
#endif
#define MAX_TRACKS 2000
#define SCAN_DEPTH 4
#define WF_COLS 80            /* overview waveform columns (params wf_1 .. wf_80) */
#define WF_LEVELS 12          /* bar heights per column; the value is colour * 3 * WF_LEVELS + state * WF_LEVELS + level */

/* ---------- track list and loading (worker thread) ---------- */

typedef struct {
    int16_t *pcm;      /* interleaved stereo at 44.1 kHz */
    int64_t frames;
    double bpm;        /* detected (0 = unknown) */
    double anchor;     /* frame of a beat (grid origin) */
    char name[256];    /* file name: the project falls back to finding it by name */
    char path[1024];   /* full path, saved in the project */
    uint8_t wf[WF_COLS];   /* overview waveform: peak level per column, 0..WF_LEVELS-1 */
} track_t;

typedef struct {
    char **paths; char **names; int n;
} tlist_t;

typedef struct deck deck_t;

static void tlist_free(tlist_t *l) {
    if (!l) return;
    for (int i = 0; i < l->n; i++) { free(l->paths[i]); free(l->names[i]); }
    free(l->paths); free(l->names); free(l);
}

static int is_audio(const char *n) {
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".wav") || !strcasecmp(dot, ".mp3"));
}

typedef struct { char *path; char *name; int depth; } entry_t;
static int entry_cmp(const void *a, const void *b) {
    return strcasecmp(((const entry_t *)a)->path, ((const entry_t *)b)->path);
}

static void scan_dir(const char *dir, int depth, entry_t **ents, int *n, int *cap) {
    if (depth > SCAN_DEPTH || *n >= MAX_TRACKS) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *n < MAX_TRACKS) {
        if (e->d_name[0] == '.') continue;   /* ., .., hidden and macOS ._ files */
        char p[1024];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (lstat(p, &st)) continue;         /* lstat: never follow symlinks */
        if (S_ISDIR(st.st_mode)) scan_dir(p, depth + 1, ents, n, cap);
        else if (S_ISREG(st.st_mode) && is_audio(e->d_name)) {
            if (*n == *cap) { *cap = *cap ? *cap * 2 : 64; *ents = realloc(*ents, sizeof(entry_t) * *cap); }
            (*ents)[*n].path = strdup(p);
            (*ents)[*n].name = strdup(e->d_name);
            (*n)++;
        }
    }
    closedir(d);
}

/* Folders scanned, in order: the plugin's own Tracks folder (data_dir), $DECKS_DIR (desktop tests),
 * and a "Decks" folder at the top of any card or drive under /media. */
static tlist_t *scan_all(const char *data_dir) {
    entry_t *ents = NULL; int n = 0, cap = 0;
    if (data_dir) scan_dir(data_dir, 0, &ents, &n, &cap);
    const char *env = getenv("DECKS_DIR");
    if (env && *env) scan_dir(env, 0, &ents, &n, &cap);
    DIR *m = opendir("/media");
    if (m) {
        struct dirent *e;
        while ((e = readdir(m))) {
            if (e->d_name[0] == '.') continue;
            char p[512];
            snprintf(p, sizeof p, "/media/%s/Decks", e->d_name);
            scan_dir(p, 0, &ents, &n, &cap);
        }
        closedir(m);
    }
    qsort(ents, n, sizeof *ents, entry_cmp);   /* stable order: a Q-Link position always means the same file */
    tlist_t *l = calloc(1, sizeof *l);
    l->n = n;
    l->paths = calloc(n ? n : 1, sizeof(char *));
    l->names = calloc(n ? n : 1, sizeof(char *));
    for (int i = 0; i < n; i++) { l->paths[i] = ents[i].path; l->names[i] = ents[i].name; }
    free(ents);
    return l;
}

/* ---------- the track browser: one folder's listing (worker thread) ---------- */

#define BR_ROWS 6   /* rows on screen (0.9.8: 6 full-width rows, 40 px names, readable in a dark club) */
#define BR_MAX 5000

typedef struct {
    int n;
    char **name, **path; char *isdir;
    char dir[1024];      /* "" = the top level (drives) */
    char parent[1024];   /* where UP goes */
} blist_t;

static void blist_free(blist_t *b) {
    if (!b) return;
    for (int i = 0; i < b->n; i++) { free(b->name[i]); free(b->path[i]); }
    free(b->name); free(b->path); free(b->isdir); free(b);
}

static void blist_add(blist_t *b, int *cap, const char *name, const char *path, int isdir) {
    if (b->n >= BR_MAX) return;
    if (b->n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        b->name = realloc(b->name, sizeof(char *) * *cap);
        b->path = realloc(b->path, sizeof(char *) * *cap);
        b->isdir = realloc(b->isdir, *cap);
    }
    b->name[b->n] = strdup(name); b->path[b->n] = strdup(path); b->isdir[b->n] = (char)isdir;
    b->n++;
}

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

/* The top level: the plugin's own Tracks folder, $DECKS_DIR (desktop tests) and every drive under /media. */
static void root_entries(const char *data_dir, blist_t *b, int *cap) {
    if (data_dir && data_dir[0] && is_dir(data_dir)) blist_add(b, cap, "Decks Tracks folder", data_dir, 1);
    const char *env = getenv("DECKS_DIR");
    if (env && *env && is_dir(env)) {
        const char *base = strrchr(env, '/');
        blist_add(b, cap, base && base[1] ? base + 1 : env, env, 1);
    }
    DIR *m = opendir("/media");
    if (!m) return;
    char *names[64]; int nn = 0;
    struct dirent *e;
    while ((e = readdir(m)) && nn < 64) {
        if (e->d_name[0] == '.' || !strncmp(e->d_name, "acvs", 4)) continue;   /* hidden, and MPC's own system mounts */
        names[nn++] = strdup(e->d_name);
    }
    closedir(m);
    for (int i = 0; i < nn; i++) for (int j = i + 1; j < nn; j++) if (strcasecmp(names[j], names[i]) < 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
    for (int i = 0; i < nn; i++) {
        char p[512]; snprintf(p, sizeof p, "/media/%s", names[i]);
        if (is_dir(p)) blist_add(b, cap, names[i], p, 1);
        free(names[i]);
    }
}

typedef struct { char *name; int isdir; } bent_t;
static int bent_cmp(const void *a, const void *b) {
    const bent_t *x = a, *y = b;
    if (x->isdir != y->isdir) return y->isdir - x->isdir;   /* folders first */
    return strcasecmp(x->name, y->name);
}

static blist_t *list_dir(const char *data_dir, const char *dir) {
    blist_t *b = calloc(1, sizeof *b);
    int cap = 0;
    snprintf(b->dir, sizeof b->dir, "%s", dir);
    if (!dir[0]) { root_entries(data_dir, b, &cap); return b; }
    /* UP from a top-level folder goes back to the drives */
    blist_t *top = calloc(1, sizeof *top); int tc = 0;
    root_entries(data_dir, top, &tc);
    int at_top = 0;
    for (int i = 0; i < top->n; i++) if (!strcmp(top->path[i], dir)) at_top = 1;
    blist_free(top);
    if (!at_top) {
        snprintf(b->parent, sizeof b->parent, "%s", dir);
        char *sl = strrchr(b->parent, '/');
        if (sl && sl != b->parent) *sl = 0; else b->parent[0] = 0;
    }
    DIR *dd = opendir(dir);
    if (!dd) return b;
    bent_t *ents = NULL; int n = 0, ecap = 0;
    struct dirent *e;
    while ((e = readdir(dd)) && n < BR_MAX) {
        if (e->d_name[0] == '.') continue;   /* ., .., hidden and macOS ._ files */
        char p[1024];
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (lstat(p, &st)) continue;         /* never follow symlinks */
        int isd = S_ISDIR(st.st_mode);
        if (!isd && !(S_ISREG(st.st_mode) && is_audio(e->d_name))) continue;
        if (n == ecap) { ecap = ecap ? ecap * 2 : 64; ents = realloc(ents, sizeof *ents * ecap); }
        ents[n].name = strdup(e->d_name); ents[n].isdir = isd; n++;
    }
    closedir(dd);
    qsort(ents, n, sizeof *ents, bent_cmp);
    for (int i = 0; i < n; i++) {
        char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, ents[i].name);
        blist_add(b, &cap, ents[i].name, p, ents[i].isdir);
        free(ents[i].name);
    }
    free(ents);
    return b;
}

/* Decode to 44.1 kHz int16 stereo, at most MAX_SECONDS: the file is read only that far, so a long mix never needs more
 * memory than a 15-minute track (decoding a whole hour first would want ~600 MB). *cut is set when the file is longer. */
static int16_t *decode(const char *path, int64_t *frames_out, int *cut) {
    unsigned ch = 0, rate = 0; uint64_t fr = 0;
    int16_t *raw = NULL;
    *cut = 0;
    const char *dot = strrchr(path, '.');
    if (dot && !strcasecmp(dot, ".mp3")) {
        drmp3 mp3;
        if (!drmp3_init_file(&mp3, path, NULL)) return NULL;
        ch = mp3.channels; rate = mp3.sampleRate;
        if (ch && rate) {
            uint64_t maxin = (uint64_t)MAX_SECONDS * rate, cap = 0;
            for (;;) {   /* MP3 has no reliable length up front: grow in 30 s steps */
                if (fr == cap) {
                    if (cap >= maxin) { int16_t probe[2 * 8]; if (drmp3_read_pcm_frames_s16(&mp3, 1, probe)) *cut = 1; break; }
                    uint64_t nc = cap + (uint64_t)rate * 30; if (nc > maxin) nc = maxin;
                    int16_t *nr = realloc(raw, (size_t)nc * ch * sizeof(int16_t));
                    if (!nr) break;
                    raw = nr; cap = nc;
                }
                uint64_t got = drmp3_read_pcm_frames_s16(&mp3, cap - fr, raw + fr * ch);
                if (!got) break;
                fr += got;
            }
        }
        drmp3_uninit(&mp3);
    } else {
        drwav wav;
        if (!drwav_init_file(&wav, path, NULL)) return NULL;
        ch = wav.channels; rate = wav.sampleRate;
        if (ch && rate) {
            uint64_t want = wav.totalPCMFrameCount, maxin = (uint64_t)MAX_SECONDS * rate;
            if (want > maxin) { want = maxin; *cut = 1; }
            if (want) raw = malloc((size_t)want * ch * sizeof(int16_t));
            if (raw) fr = drwav_read_pcm_frames_s16(&wav, want, raw);
        }
        drwav_uninit(&wav);
    }
    if (!raw || !ch || !rate || !fr) { free(raw); return NULL; }
    int64_t out_n = (int64_t)((double)fr * SR / rate);
    int16_t *out = malloc((size_t)out_n * 4 + 4);
    if (out) {
        double step = (double)rate / SR;
        for (int64_t i = 0; i < out_n; i++) {
            double sp = i * step;
            int64_t a = (int64_t)sp; double f = sp - a;
            if (a >= (int64_t)fr) a = (int64_t)fr - 1;
            int64_t b = a + 1 < (int64_t)fr ? a + 1 : a;
            for (int c = 0; c < 2; c++) {
                unsigned sc = c < (int)ch ? (unsigned)c : 0;   /* mono -> both sides */
                double v = raw[a * ch + sc] * (1 - f) + raw[b * ch + sc] * f;
                out[i * 2 + c] = (int16_t)lrint(v);
            }
        }
        *frames_out = out_n;
    }
    free(raw);
    return out;
}

/* Tempo and beat grid. An onset curve at 100 Hz (rises in low-band and full-band energy), a coarse tempo from its
 * autocorrelation (70-180 BPM, a mild preference for ~120), then a fine search: for each candidate tempo, fold the whole
 * curve onto one beat and keep the tempo whose fold has the sharpest peak. The peak's phase is the grid anchor. */
#define HOP 441
void deck_analyse(const int16_t *pcm, int64_t frames, double *bpm_out, double *anchor_out) {
    *bpm_out = 0; *anchor_out = 0;
    int64_t nh = frames / HOP;
    if (nh < 400) return;   /* under 4 seconds */
    float *on = calloc(nh, sizeof(float));
    float lp = 0, prev_lo = 0, prev_all = 0;
    const float a = 1.0f - expf(-2.0f * (float)M_PI * 150.0f / SR);
    for (int64_t h = 0; h < nh; h++) {
        double elo = 0, eall = 0;
        for (int i = 0; i < HOP; i++) {
            const int16_t *s = pcm + (h * HOP + i) * 2;
            float x = (s[0] + s[1]) * (1.0f / 65536.0f);
            lp += a * (x - lp);
            elo += lp * lp; eall += x * x;
        }
        float llo = logf(1e-9f + (float)elo), lall = logf(1e-9f + (float)eall);
        float d = (llo - prev_lo > 0 ? llo - prev_lo : 0) + 0.5f * (lall - prev_all > 0 ? lall - prev_all : 0);
        on[h] = h ? d : 0;
        prev_lo = llo; prev_all = lall;
    }
    /* remove the local mean (1 s) so loud passages don't dominate */
    float *o = calloc(nh, sizeof(float));
    double acc = 0;
    for (int64_t h = 0; h < nh; h++) {
        acc += on[h]; if (h >= 100) acc -= on[h - 100];
        float m = (float)(acc / (h < 100 ? h + 1 : 100));
        o[h] = on[h] > m ? on[h] - m : 0;
    }
    free(on);
    /* coarse: autocorrelation on up to 90 s from the middle */
    int64_t seg = nh < 9000 ? nh : 9000, s0 = (nh - seg) / 2;
    double best = -1, best_bpm = 120;
    for (double bpm = 70; bpm <= 180; bpm += 0.25) {
        double P = 6000.0 / bpm, sc = 0;
        for (int m = 1; m <= 4; m *= 2) {
            double lag = P * m; int64_t L = (int64_t)lag; double f = lag - L;
            double s = 0;
            for (int64_t h = s0; h + L + 1 < s0 + seg; h++)
                s += o[h] * (o[h + L] * (1 - f) + o[h + L + 1] * f);
            sc += s / m;
        }
        double oct = log2(bpm / 122.0);
        sc *= exp(-0.5 * oct * oct / (0.6 * 0.6));
        if (sc > best) { best = sc; best_bpm = bpm; }
    }
    /* fine: sharpest fold over the whole track, +-1 BPM in 0.01 steps */
    enum { NB = 64 };
    double best_sharp = -1, fine_bpm = best_bpm, fine_phase = 0;
    for (double bpm = best_bpm - 1.0; bpm <= best_bpm + 1.0; bpm += 0.01) {
        double P = 6000.0 / bpm;
        double bins[NB] = {0};
        for (int64_t h = 0; h < nh; h++) {
            if (o[h] <= 0) continue;
            double ph = fmod(h / P, 1.0);
            bins[(int)(ph * NB) % NB] += o[h];
        }
        double tot = 0, mx = 0; int mi = 0;
        for (int i = 0; i < NB; i++) {   /* peak over three neighbouring bins */
            double v = bins[i] + 0.5 * (bins[(i + 1) % NB] + bins[(i + NB - 1) % NB]);
            tot += bins[i];
            if (v > mx) { mx = v; mi = i; }
        }
        double sharp = tot > 0 ? mx / tot : 0;
        if (sharp > best_sharp) { best_sharp = sharp; fine_bpm = bpm; fine_phase = (mi + 0.5) / NB; }
    }
    free(o);
    double beat = 60.0 * SR / fine_bpm;
    *bpm_out = round(fine_bpm * 100) / 100;
    *anchor_out = fmod(fine_phase * beat + HOP * 0.5, beat);
}

/* Overview waveform: the peak of each of WF_COLS slices, scaled to the track's loudest slice, on a square-root curve
 * so quiet passages still show. */
static void wave_overview(const int16_t *pcm, int64_t frames, uint8_t *wf) {
    int pk[WF_COLS] = {0}, mx = 1;
    for (int c = 0; c < WF_COLS; c++) {
        int64_t a = frames * c / WF_COLS, b = frames * (c + 1) / WF_COLS;
        int m = 0;
        for (int64_t i = a; i < b; i += 4) {   /* every 4th frame is plenty for a bar */
            int v = abs(pcm[i * 2] + pcm[i * 2 + 1]) / 2;
            if (v > m) m = v;
        }
        pk[c] = m; if (m > mx) mx = m;
    }
    for (int c = 0; c < WF_COLS; c++) {
        int l = (int)lround(sqrt((double)pk[c] / mx) * (WF_LEVELS - 1));
        wf[c] = (uint8_t)(pk[c] > 0 && l < 1 ? 1 : l);
    }
}

/* ---------- the deck ---------- */

enum { JOB_NONE = 0, JOB_SCAN = 1, JOB_LOAD = 2, JOB_LIST = 4, JOB_EJECT = 8 };

struct deck {
    char data_dir[1024];
    /* worker */
    pthread_t th; pthread_mutex_t mu; pthread_cond_t cv; int quit;
    int jobs; char load_path[1024], load_name[256];
    char eject_drive[256];   /* JOB_EJECT: the drive under /media to flush */
    track_t *ready;          /* decoded, waiting for render() to take it (under mu) */
    track_t *garbage;        /* handed back by render() to free (under mu) */
    tlist_t *list;           /* current track list (under mu) */
    char status[96];         /* under mu */
    int loading;
    /* restore (state chunk): cues to apply when this track arrives */
    char want_name[256]; double want_cues[NCUES]; double want_cue; double want_bpm; double want_anchor; int want_valid;
    int want_scanned;        /* the saved path failed and a search by name has been started */
    /* browser (all under mu) */
    blist_t *bl;             /* the folder on screen */
    char br_req[1024], br_req_sel[256];   /* JOB_LIST: folder to read, entry to select in it */
    int sel, top;            /* selected entry, first row shown */
    double last_tap;         /* row taps: MPC sends two presses per tap */
    int browse;              /* the browser panel is open */

    /* audio thread */
    track_t *t;
    double pos;              /* playhead, in track frames */
    double slip;             /* where the track would be without the roll */
    int playing, previewing;
    double cue;              /* main cue point */
    double cues[NCUES];      /* hot cues, -1 = empty */
    double bpm, anchor;      /* track's grid (bpm editable) */
    /* jump crossfade */
    double fpos; int fleft;
    double park;             /* where the playhead goes once a stop has faded out (-1 = none) */
    /* play/stop gain ramp */
    float gain, gain_target;
    /* loops */
    int loop_on; double loop_a, loop_b;          /* normal loop */
    int roll_note; double roll_a, roll_b;        /* held roll (slip), roll_note = -1 when none */
    /* params */
    float vol, pitch, filt, eq[3];
    int range, sync, loop_beats, pad_base, pad_mode;
    /* host tempo */
    double host_bpm, host_ppq; int host_playing;
    double sync_trim;        /* phase-lock correction */
    int last_note;
    /* DSP state */
    float svf_ic1[2], svf_ic2[2];
    float bq[3][5]; float bz[3][2][2];  /* biquads: coeffs b0 b1 b2 a1 a2; state per channel */
    float eq_set[3];
    unsigned rev;            /* display_rev */
    /* display: last values reported, so display_rev only moves when one changes */
    int wf_col, beat_shown, phase_shown;
    /* MPC transport and nudging */
    int follow;              /* follow the MPC's Play / Play Start / Stop */
    int was_host_playing;
    double bend; int bend_left;   /* temporary rate offset from nudge / jog, frames left */
    double jog_last, scroll_last;  /* the last value MPC sent JOG / SCROLL (it sends a running total, see rel_delta) */
    int hold_dir, hold_frames;    /* a NUDGE pad held down: direction (-1/+1, 0 none) and frames held */
    int pad_set;                  /* unused since 0.9.4 (bank A pads 9-11 used to switch to rolls); kept for old projects */
    unsigned pad_held;            /* pads held down right now (bit k = pad k+1) */
    unsigned pad_browse;          /* held pads that were pressed as browser pads (their release goes to the browser) */
    int brep_dir, brep_frames;    /* a held UP / DOWN pad in the browser: direction and frames held (auto-repeat) */
    int sel_kind;                 /* last br_sel_kind answer */
    int guard_to; double guard_t; /* a load refused because that deck is playing: which deck, and when (press again to load) */
    char warn[64]; double warn_until;   /* a short message in the browser's path line */
    int bank;                     /* the pad bank last played (0 A: deck, 1 B: hot cues): the tiles and pad lights show it */
    int64_t play_frames; int played_marked;   /* frames this track has played; whether it went on the played list */
    int shift_latch;              /* SHIFT tapped on the screen: the next pad tap gets its shift function */
    int end_warn;                 /* last 30 s of a playing track: 0 off, 1/2 the two halves of the platter ring's flash */
    int pad_lights;               /* light the MPC's own pads in the tile colours (SETUP, experimental) */
    unsigned rt_max_us, rt_n;     /* longest audio block (microseconds) and blocks since the last heartbeat */
    double dbg_beat;              /* time of the last log heartbeat */
    int pad_flash[16];            /* frames left of a screen tap's light */
    uint8_t pad_shown[16];        /* the pad tiles' last values (function * 3 + light) */
    uint8_t note_to[128];         /* deck letter + 1 each held note went to, so its release follows it after a CONTROL switch */
    /* decks: letter, and the sync reference worked out once per block */
    int deck_id;             /* 0..1 = A..B */
    int target;              /* the deck this screen and these pads control (0..1); == deck_id: this deck */
    /* controls sent here by another deck's screen: applied at the start of the next audio block */
    pthread_mutex_t qmu;
    struct { char key[24]; char val[48]; } q[64]; int qn;
    uint8_t mq[64][3]; int mqn;
    /* what this screen last sent to a remote deck, read back until that deck has applied it */
    struct { char key[24]; char val[48]; double t; } pend[8]; int pendi;
    track_t *grave; double grave_t;   /* an old track kept a moment before freeing (another deck's screen may be reading it) */
    unsigned seen_leader_rev;
    double ref_bpm, ref_beat; int ref_phase_ok, ref_deck;   /* ref_deck: leader's letter, -1 = the MPC, -2 = none */
};

/* ---------- decks sharing one process: letters, the leader deck, and its published tempo and beat ----------
 * Every instance of the plugin lives in one MPC process with one copy of these statics (Open MPC, NOTES 2026-09-30),
 * so a deck on another track can follow the leader. The leader publishes once per audio block under a sequence
 * counter; followers read it without blocking. */
#define MAX_DECKS 8
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static deck_t *g_decks[MAX_DECKS];
static deck_t *g_leader;          /* read with __atomic_load_n */
/* The note each MPC pad sends: [0..15] bank A pads 1-16, [16..31] bank B. Shared by every deck, since it is the
 * hardware's: set from Pad Base (chromatic) or learned by hitting the pads in order. g_learn: 0 off, 1..32 = the pad to
 * hit next. */
#define NPADS 32
static int g_padnote[NPADS] = {36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
                               52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67};
static int g_learn;
static unsigned g_learn_rev;
/* the log and the pad lights (defined further down, beside set_param) */
#ifndef DECKS_VERSION
#define DECKS_VERSION 1000
#endif
static int g_debug;
static void dbg_sync(int sync, const char *fmt, ...);
static deck_t *g_light_owner;
static void lights_off(void);
static unsigned g_leader_rev;     /* moves whenever the leader changes */
static struct { unsigned seq; double bpm, beat; int playing, valid, id; } g_pub;

static void set_leader(deck_t *d) {
    pthread_mutex_lock(&g_mu);
    __atomic_store_n(&g_leader, d, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_leader_rev, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_mu);
}

static void publish(int id, double bpm, double beat, int playing, int valid) {
    unsigned s = __atomic_load_n(&g_pub.seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pub.seq, s + 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    g_pub.bpm = bpm; g_pub.beat = beat; g_pub.playing = playing; g_pub.valid = valid; g_pub.id = id;
    __atomic_store_n(&g_pub.seq, s + 2, __ATOMIC_RELEASE);
}

static int read_pub(double *bpm, double *beat, int *playing, int *id) {
    for (int tries = 0; tries < 4; tries++) {
        unsigned s1 = __atomic_load_n(&g_pub.seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) continue;
        double b = g_pub.bpm, bt = g_pub.beat; int pl = g_pub.playing, v = g_pub.valid, i = g_pub.id;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_pub.seq, __ATOMIC_ACQUIRE) == s1) { *bpm = b; *beat = bt; *playing = pl; *id = i; return v; }
    }
    return 0;
}

static void set_status(deck_t *d, const char *s) {
    snprintf(d->status, sizeof d->status, "%s", s);
    d->rev++;
}

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }

static void *worker(void *arg) {
    deck_t *d = arg;
    pthread_mutex_lock(&d->mu);
    while (!d->quit) {
        if (d->garbage) {   /* an old track: kept 2 s in the grave first, since another deck's screen may be reading it */
            track_t *g = d->garbage, *old = d->grave; d->garbage = NULL;
            d->grave = g; d->grave_t = now_s();
            if (old) { pthread_mutex_unlock(&d->mu); free(old->pcm); free(old); pthread_mutex_lock(&d->mu); }
            continue;
        }
        if (d->grave && now_s() - d->grave_t > 2.0) {
            track_t *g = d->grave; d->grave = NULL;
            pthread_mutex_unlock(&d->mu); free(g->pcm); free(g); pthread_mutex_lock(&d->mu);
            continue;
        }
        if (d->jobs & JOB_SCAN) {
            d->jobs &= ~JOB_SCAN;
            pthread_mutex_unlock(&d->mu);
            tlist_t *l = scan_all(d->data_dir[0] ? d->data_dir : NULL);
            pthread_mutex_lock(&d->mu);
            tlist_free(d->list); d->list = l;
            if (!l->n) set_status(d, "No tracks: add WAV/MP3 to Tracks");
            else { char s[64]; snprintf(s, sizeof s, "%d tracks", l->n); set_status(d, s); }
            continue;
        }
        if (d->jobs & JOB_LIST) {
            d->jobs &= ~JOB_LIST;
            char dir[1024], want[256];
            snprintf(dir, sizeof dir, "%s", d->br_req);
            snprintf(want, sizeof want, "%s", d->br_req_sel);
            pthread_mutex_unlock(&d->mu);
            blist_t *b = list_dir(d->data_dir, dir), *old;
            int s = 0;
            for (int i = 0; want[0] && i < b->n; i++) if (!strcmp(b->name[i], want) || !strcmp(b->path[i], want)) { s = i; break; }
            pthread_mutex_lock(&d->mu);
            old = d->bl; d->bl = b;
            d->sel = s; d->top = s >= BR_ROWS ? s - BR_ROWS / 2 : 0;
            if (d->top > b->n - BR_ROWS) d->top = b->n - BR_ROWS > 0 ? b->n - BR_ROWS : 0;
            d->rev++;
            pthread_mutex_unlock(&d->mu);
            blist_free(old);
            pthread_mutex_lock(&d->mu);
            continue;
        }
        if (d->jobs & JOB_EJECT) {   /* sync() can take seconds on a stick: never under the plugin's lock */
            d->jobs &= ~JOB_EJECT;
            char drive[256], root[300];
            snprintf(drive, sizeof drive, "%s", d->eject_drive);
            snprintf(root, sizeof root, "/media/%s/", drive);
            pthread_mutex_unlock(&d->mu);
            sync();
            int busy = 0;
            pthread_mutex_lock(&g_mu);
            for (int k = 0; k < MAX_DECKS; k++)
                if (g_decks[k] && g_decks[k]->loading && !strncmp(g_decks[k]->load_path, root, strlen(root))) busy = 1;
            pthread_mutex_unlock(&g_mu);
            pthread_mutex_lock(&d->mu);
            snprintf(d->warn, sizeof d->warn, busy ? "WAIT: A TRACK IS STILL LOADING FROM %.30s" : "SAFE TO REMOVE %.40s", drive);
            d->warn_until = now_s() + 4.0; d->rev++;
            continue;
        }
        if (d->jobs & JOB_LOAD) {
            d->jobs &= ~JOB_LOAD;
            char path[1024], name[256];
            snprintf(path, sizeof path, "%s", d->load_path);
            snprintf(name, sizeof name, "%s", d->load_name);
            d->loading = 1;
            set_status(d, "Loading...");
            pthread_mutex_unlock(&d->mu);
            int64_t fr = 0; int cut = 0;
            int16_t *pcm = decode(path, &fr, &cut);
            track_t *t = NULL;
            if (pcm) {
                t = calloc(1, sizeof *t);
                t->pcm = pcm; t->frames = fr;
                snprintf(t->name, sizeof t->name, "%s", name);
                snprintf(t->path, sizeof t->path, "%s", path);
                deck_analyse(pcm, fr, &t->bpm, &t->anchor);
                wave_overview(pcm, fr, t->wf);
            }
            pthread_mutex_lock(&d->mu);
            d->loading = 0;
            if (!t) {
                if (d->want_valid && !strcmp(name, d->want_name) && !d->want_scanned) {
                    /* a saved track that moved: look for it by name on every drive */
                    char s[96]; snprintf(s, sizeof s, "Looking for %.70s", name); set_status(d, s);
                    d->want_scanned = 1; d->jobs |= JOB_SCAN;
                } else {
                    if (d->want_valid && !strcmp(name, d->want_name)) d->want_valid = 0;
                    char s[96]; snprintf(s, sizeof s, "Can't read %.60s", name); set_status(d, s);
                }
                continue;
            }
            if (d->jobs & JOB_LOAD) { free(t->pcm); free(t); continue; }   /* a newer load is queued */
            if (d->ready) { free(d->ready->pcm); free(d->ready); }
            d->ready = t;
            set_status(d, cut ? "Loaded the first 15 minutes" : "Loaded");
            continue;
        }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 1;
        pthread_cond_timedwait(&d->cv, &d->mu, &ts);
    }
    pthread_mutex_unlock(&d->mu);
    return NULL;
}

static void post(deck_t *d, int job) { d->jobs |= job; pthread_cond_signal(&d->cv); }

/* Queue a load by path (caller holds mu). */
static void load_path(deck_t *d, const char *path) {
    const char *base = strrchr(path, '/');
    snprintf(d->load_path, sizeof d->load_path, "%s", path);
    snprintf(d->load_name, sizeof d->load_name, "%s", base ? base + 1 : path);
    post(d, JOB_LOAD);
}

/* Queue a load by index in the search-by-name list (caller holds mu). */
static void load_index(deck_t *d, int i) {
    if (!d->list || i < 0 || i >= d->list->n) return;
    load_path(d, d->list->paths[i]);
}

/* JOG and SCROLL read back 0, but MPC sends a running total while they turn: -0.02, -0.04, ... -0.40 (seen on a
 * Live II, 3.10), and starts again from 0 when it reads back. The step is the change from the last value sent; a small
 * value that isn't next to the last one is a fresh start from 0. */
static double rel_delta(double *last, double v) {
    double dv;
    if (*last != 0 && fabs(v - *last) <= 0.03) dv = v - *last;
    else if (fabs(v) <= 0.025) dv = v;
    else dv = v - *last;
    *last = v;
    if (dv > 0.2) dv = 0.2; else if (dv < -0.2) dv = -0.2;
    return dv;
}

/* Browser (caller holds mu). */
static void br_open_dir(deck_t *d, const char *dir, const char *select) {
    snprintf(d->br_req, sizeof d->br_req, "%s", dir);
    snprintf(d->br_req_sel, sizeof d->br_req_sel, "%s", select ? select : "");
    post(d, JOB_LIST);
}

static void br_select(deck_t *d, int i) {
    int n = d->bl ? d->bl->n : 0;
    if (i < 0) i = 0;
    if (n > 0 && i > n - 1) i = n - 1;   /* an empty folder keeps the knob's own value */
    d->sel = i;
    if (d->sel < d->top) d->top = d->sel;
    if (d->sel >= d->top + BR_ROWS) d->top = d->sel - BR_ROWS + 1;
    d->rev++;
}

/* LOAD, or a tap on the selected row: open a folder, or load a file and close the browser. */
static int load_guard(deck_t *d, int to, int playing);
static void br_activate(deck_t *d) {
    if (!d->bl || d->sel < 0 || d->sel >= d->bl->n) return;
    if (d->bl->isdir[d->sel]) br_open_dir(d, d->bl->path[d->sel], NULL);
    else if (load_guard(d, d->deck_id, d->playing && !d->previewing)) { load_path(d, d->bl->path[d->sel]); d->browse = 0; d->rev++; }
}

/* Browser moves, for the screen's controls and the pads (these take mu themselves). */
static void br_move(deck_t *d, int n) {
    pthread_mutex_lock(&d->mu);
    br_select(d, d->sel + n);
    if (!d->browse) d->browse = 1;
    pthread_mutex_unlock(&d->mu);
}

static void br_page(deck_t *d, int dn) {
    pthread_mutex_lock(&d->mu);
    d->top += dn ? BR_ROWS : -BR_ROWS;
    int n = d->bl ? d->bl->n : 0;
    if (d->top > n - BR_ROWS) d->top = n - BR_ROWS;
    if (d->top < 0) d->top = 0;
    br_select(d, dn ? (d->sel + BR_ROWS) : (d->sel - BR_ROWS));
    pthread_mutex_unlock(&d->mu);
}

static void br_back(deck_t *d) {
    pthread_mutex_lock(&d->mu);
    if (d->bl && d->bl->dir[0]) br_open_dir(d, d->bl->parent, d->bl->dir);
    pthread_mutex_unlock(&d->mu);
}

static void br_open_sel(deck_t *d) { pthread_mutex_lock(&d->mu); br_activate(d); pthread_mutex_unlock(&d->mu); }

/* the selection: 1 a file, 2 a folder, 0 nothing (caller need not hold mu; a stale answer only dims a pad) */
static int br_sel_kind(deck_t *d) {
    int r = 0;
    if (pthread_mutex_trylock(&d->mu)) return d->sel_kind;
    if (d->bl && d->sel >= 0 && d->sel < d->bl->n) r = d->bl->isdir[d->sel] ? 2 : 1;
    pthread_mutex_unlock(&d->mu);
    return d->sel_kind = r;
}

/* A deck that is playing only takes a new track on a second press within 3 s, so a slip in the browser can't cut off the
 * track the crowd is hearing. Returns 1 when the load may go ahead. */
static int load_guard(deck_t *d, int to, int playing) {
    double t = now_s();
    if (!playing || (d->guard_to == to && t - d->guard_t < 3.0)) { d->guard_to = -1; return 1; }
    d->guard_to = to; d->guard_t = t;
    snprintf(d->warn, sizeof d->warn, "DECK %c IS PLAYING: PRESS LOAD %c AGAIN TO REPLACE IT", 'A' + to, 'A' + to);
    d->warn_until = t + 3.0; d->rev++;
    return 0;
}

/* LOAD A / LOAD B: the selection onto that deck (a folder opens instead), and the browser closes */
static void br_load_to(deck_t *d, int to) {
    char path[1024] = "";
    pthread_mutex_lock(&d->mu);
    if (d->bl && d->sel >= 0 && d->sel < d->bl->n) {
        if (d->bl->isdir[d->sel]) br_open_dir(d, d->bl->path[d->sel], NULL);
        else snprintf(path, sizeof path, "%s", d->bl->path[d->sel]);
    }
    pthread_mutex_unlock(&d->mu);
    if (!path[0]) return;
    deck_t *o = d->deck_id == to ? d : NULL;
    pthread_mutex_lock(&g_mu);
    if (!o) for (int i = 0; i < MAX_DECKS; i++) if (g_decks[i] && g_decks[i]->deck_id == to) o = g_decks[i];
    int ok = o && load_guard(d, to, o->playing && !o->previewing);
    if (ok) { pthread_mutex_lock(&o->mu); load_path(o, path); pthread_mutex_unlock(&o->mu); }
    pthread_mutex_unlock(&g_mu);
    if (ok) { d->browse = 0; d->rev++; }
    else if (!o) {
        snprintf(d->warn, sizeof d->warn, "NO DECK %c: ADD A SECOND MPSEEJAY TRACK", 'A' + to);
        d->warn_until = now_s() + 3.0; d->rev++;
    }
}

/* ---------- the browser's rows: on a deck, played, selected ----------
 * Tracks that have played 30 s on either deck go on a played list for as long as the MPC app runs; the browser greys them
 * out. A track on a deck is shaded in that deck's colour. */
#define PLAYED_MAX 1024
static __thread int t_gmu_held;   /* this thread already holds g_mu (reading another deck for its screen) */
static char *g_played[PLAYED_MAX]; static int g_nplayed;   /* under g_mu */

static void played_add(const char *path) {   /* caller holds g_mu */
    for (int i = 0; i < g_nplayed; i++) if (!strcmp(g_played[i], path)) return;
    if (g_nplayed == PLAYED_MAX) { free(g_played[0]); memmove(g_played, g_played + 1, sizeof g_played[0] * (PLAYED_MAX - 1)); g_nplayed--; }
    g_played[g_nplayed++] = strdup(path);
}

/* Row i (0-5) of d's browser: 0 empty, 1 folder, 2 file; *sel when selected; *on = deck letter + 1 holding it;
 * *played. Locks d->mu, then (released) g_mu, so it keeps the lock order g_mu -> mu used elsewhere. */
static int br_row(deck_t *d, int i, int *sel, int *on, int *played, char *name, int nn) {
    char path[1024] = "";
    int kind = 0;
    *sel = *on = *played = 0;
    pthread_mutex_lock(&d->mu);
    int r = d->top + i;
    if (d->bl && r < d->bl->n) {
        kind = d->bl->isdir[r] ? 1 : 2;
        *sel = r == d->sel;
        snprintf(path, sizeof path, "%s", d->bl->path[r]);
        if (name) snprintf(name, nn, "%s", d->bl->name[r]);
    }
    pthread_mutex_unlock(&d->mu);
    if (kind != 2) return kind;
    if (!t_gmu_held) pthread_mutex_lock(&g_mu);
    for (int k = 0; k < MAX_DECKS; k++) {
        deck_t *o = g_decks[k];
        if (!o) continue;
        const char *lp = o->loading ? o->load_path : o->t ? o->t->path : "";
        if (!strcmp(lp, path)) *on = o->deck_id + 1;
    }
    for (int k = 0; k < g_nplayed && !*played; k++) if (!strcmp(g_played[k], path)) *played = 1;
    if (!t_gmu_held) pthread_mutex_unlock(&g_mu);
    return kind;
}

/* EJECT: flush anything written to the drive on screen (or selected at the top level) and say whether it is safe to pull.
 * The MPC's own disk service unmounts drives (its browser's eject); a plugin can't, but mpSEEJAY runs from the internal
 * drive and keeps loaded tracks in memory, so once nothing is loading from the stick it can go. */
static void br_eject(deck_t *d) {
    char path[1024] = "";
    pthread_mutex_lock(&d->mu);
    if (d->bl && d->bl->dir[0]) snprintf(path, sizeof path, "%s", d->bl->dir);
    else if (d->bl && d->sel < d->bl->n) snprintf(path, sizeof path, "%s", d->bl->path[d->sel]);
    if (strncmp(path, "/media/", 7)) {
        snprintf(d->warn, sizeof d->warn, "PICK A DRIVE (OR A FOLDER ON IT) TO EJECT");
    } else {
        const char *a = path + 7, *e = strchr(a, '/');
        snprintf(d->eject_drive, sizeof d->eject_drive, "%.*s", (int)(e ? e - a : (long)strlen(a)), a);
        snprintf(d->warn, sizeof d->warn, "EJECTING %.40s...", d->eject_drive);
        post(d, JOB_EJECT);   /* the worker flushes it and says when it's safe */
    }
    d->warn_until = now_s() + 4.0; d->rev++;
    pthread_mutex_unlock(&d->mu);
}

static void *create(const char *data_dir) {
    deck_t *d = calloc(1, sizeof *d);
    if (data_dir) snprintf(d->data_dir, sizeof d->data_dir, "%s", data_dir);
    pthread_mutex_init(&d->mu, NULL); pthread_cond_init(&d->cv, NULL);
    d->vol = 1.0f; d->range = 0; d->loop_beats = 4; d->pad_base = 36; d->roll_note = -1; d->last_note = -1; d->park = -1;
    d->follow = 1; d->pad_lights = 1; d->guard_to = -1; d->wf_col = -1; d->was_host_playing = -1; d->ref_deck = -2;
    pthread_mutex_init(&d->qmu, NULL);
    pthread_mutex_lock(&g_mu);
    int used = 0, slot = -1;
    for (int i = 0; i < MAX_DECKS; i++) { if (g_decks[i]) used |= 1 << g_decks[i]->deck_id; else if (slot < 0) slot = i; }
    while (d->deck_id < 1 && (used >> d->deck_id & 1)) d->deck_id++;   /* the first free letter */
    if (slot >= 0) g_decks[slot] = d;
    d->target = d->deck_id;
    pthread_mutex_unlock(&g_mu);
    d->gain = 0; d->gain_target = 0;
    for (int i = 0; i < NCUES; i++) d->cues[i] = -1;
    for (int i = 0; i < 3; i++) d->eq_set[i] = 99;   /* force coefficient computation */
    set_status(d, "Tap BROWSE to pick a track");
    dbg_sync(1, "%c started (Decks %d)", 'A' + d->deck_id, DECKS_VERSION);
    br_open_dir(d, "", NULL);
    pthread_create(&d->th, NULL, worker, d);
    return d;
}

static void destroy(void *inst) {
    deck_t *d = inst;
    if (__atomic_load_n(&g_leader, __ATOMIC_ACQUIRE) == d) { publish(0, 0, 0, 0, 0); set_leader(NULL); }
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_DECKS; i++) if (g_decks[i] == d) g_decks[i] = NULL;
    if (g_light_owner == d) g_light_owner = NULL;
    pthread_mutex_unlock(&g_mu);
    dbg_sync(1, "%c closed", 'A' + d->deck_id);
    pthread_mutex_lock(&d->mu); d->quit = 1; pthread_cond_signal(&d->cv); pthread_mutex_unlock(&d->mu);
    pthread_join(d->th, NULL);
    if (d->t) { free(d->t->pcm); free(d->t); }
    if (d->ready) { free(d->ready->pcm); free(d->ready); }
    if (d->garbage) { free(d->garbage->pcm); free(d->garbage); }
    if (d->grave) { free(d->grave->pcm); free(d->grave); }
    pthread_mutex_destroy(&d->qmu);
    tlist_free(d->list);
    blist_free(d->bl);
    pthread_mutex_destroy(&d->mu); pthread_cond_destroy(&d->cv);
    free(d);
}

/* ---------- grid helpers ---------- */

static double beat_len(const deck_t *d) { return d->bpm > 0 ? 60.0 * SR / d->bpm : SR / 2.0; }
static double beat_at(const deck_t *d, double frame) { return (frame - d->anchor) / beat_len(d); }
static double frame_of_beat(const deck_t *d, double b) { return d->anchor + b * beat_len(d); }

/* the steady playback rate: the pitch fader, or the sync reference (no phase correction or nudge) */
static double steady_rate(const deck_t *d) {
    static const float ranges[3] = {0.08f, 0.16f, 0.50f};
    return d->sync && d->ref_bpm > 0 && d->bpm > 0 ? d->ref_bpm / d->bpm : 1.0 + d->pitch * ranges[d->range];
}

static double rate_now(const deck_t *d) {
    static const float ranges[3] = {0.08f, 0.16f, 0.50f};
    if (d->sync && d->ref_bpm > 0 && d->bpm > 0) return d->ref_bpm / d->bpm * (1.0 + d->sync_trim + d->bend);
    return 1.0 + d->pitch * ranges[d->range] + d->bend;
}

/* Nudge, like pushing or dragging a record's edge: speed up or slow down briefly. Repeated nudges add up (to +-15%). */
static void nudge(deck_t *d, double amount, double secs) {
    d->bend += amount;
    if (d->bend > 0.15) d->bend = 0.15; else if (d->bend < -0.15) d->bend = -0.15;
    int f = (int)(secs * SR);
    if (f > d->bend_left) d->bend_left = f;
}

static void jump(deck_t *d, double to) {
    if (!d->t) return;
    if (to < 0) to = 0;
    if (to > d->t->frames - 1) to = (double)(d->t->frames - 1);
    if (d->gain > 0.001f) { d->fpos = d->pos; d->fleft = XFADE; }
    d->pos = to;
}

static const double ROLL_BEATS[4] = {0.125, 0.25, 0.5, 1.0};
static const double LOOP_BEATS[8] = {0.25, 0.5, 1, 2, 4, 8, 16, 32};

static void loop_set(deck_t *d, int on) {
    if (!d->t) { d->loop_on = 0; return; }
    if (on) {
        double len = LOOP_BEATS[d->loop_beats];
        double b0 = floor(beat_at(d, d->pos) / (len < 1 ? len : 1)) * (len < 1 ? len : 1);
        d->loop_a = frame_of_beat(d, b0);
        d->loop_b = d->loop_a + len * beat_len(d);
    }
    d->loop_on = on;
    d->rev++;
}

static void set_cue_main(deck_t *d) { d->cue = d->pos; }

static void cue_press(deck_t *d) {
    if (!d->t) return;
    if (d->playing && !d->previewing) {          /* back to cue and stop */
        d->playing = 0; d->gain_target = 0; d->park = d->cue;
    } else if (fabs(d->pos - d->cue) < 1.0) {    /* at the cue: preview while held */
        d->playing = 1; d->previewing = 1; d->gain_target = 1;
    } else {                                     /* paused elsewhere: set the cue here */
        set_cue_main(d);
    }
    d->rev++;
}

static void cue_release(deck_t *d) {
    if (d->previewing) { d->previewing = 0; d->playing = 0; d->gain_target = 0; d->park = d->cue; d->rev++; }
}

static void play_toggle(deck_t *d, int on) {
    if (!d->t) { d->playing = 0; return; }
    if (d->previewing) { d->previewing = 0; on = 1; }   /* play while previewing keeps playing (CDJ) */
    if (on && d->park >= 0) { jump(d, d->park); d->park = -1; }
    if (on && !__atomic_load_n(&g_leader, __ATOMIC_ACQUIRE)) set_leader(d);   /* the first deck to play leads */
    d->playing = on; d->gain_target = on ? 1.0f : 0.0f;
    d->rev++;
}

static void hot_cue(deck_t *d, int i) {
    if (!d->t || i < 0 || i >= NCUES) return;
    if (d->pad_mode == 2) { d->cues[i] = -1; d->rev++; return; }          /* Clear */
    if (d->pad_mode == 1 || d->cues[i] < 0) { d->cues[i] = d->pos; d->rev++; return; }   /* Set (or empty) */
    d->park = -1;
    jump(d, d->cues[i]);
    if (d->roll_note >= 0) d->slip = d->pos;
    if (!d->playing) { d->playing = 1; d->gain_target = 1; d->rev++; }
}

static void roll_on(deck_t *d, int note, double beats) {
    if (!d->t) return;
    double b = beat_at(d, d->pos);
    double b0 = floor(b / beats) * beats;
    if (d->roll_note < 0) d->slip = d->pos;
    d->roll_note = note;
    d->roll_a = frame_of_beat(d, b0);
    d->roll_b = d->roll_a + beats * beat_len(d);
}

static void roll_off(deck_t *d, int note) {
    if (d->roll_note != note) return;
    d->roll_note = -1;
    jump(d, d->slip);
}

static void beat_jump(deck_t *d, double beats) {
    if (!d->t) return;
    double dist = beats * beat_len(d);
    jump(d, d->pos + dist);
    if (d->loop_on) { d->loop_a += dist; d->loop_b += dist; }
}

/* Pads, laid out like a player (pad 1 is bottom left on MPC):
 *   13-16  hot cues 1-4
 *    9-12  (see pad_func_mode for the layout since 0.8)
 *    5-8   SYNC, beat jump back, beat jump forward (by Loop Length), LOOP on/off
 *    1-4   CUE (hold to preview), PLAY/PAUSE, NUDGE - and NUDGE + (hold: bends while held; paused: steps 1/32 beat) */
/* BPM + / -: move the pitch so the tempo lands on the next step (0.1 or 1 BPM); nothing while SYNC sets the tempo */
static void tempo_step(deck_t *d, double step) {
    static const float ranges[3] = {0.08f, 0.16f, 0.50f};
    if (d->bpm <= 0 || d->sync) return;
    double bpm = d->bpm * (1.0 + d->pitch * ranges[d->range]);
    double q = fabs(step) >= 1 ? 1 : 10;
    bpm = round(bpm * q) / q + step;
    double p = (bpm / d->bpm - 1.0) / ranges[d->range];
    d->pitch = (float)(p > 1 ? 1 : p < -1 ? -1 : p); d->rev++;
}

static void hold_nudge(deck_t *d, int dir) {
    d->hold_dir = dir; d->hold_frames = 0;
    if (dir && d->t && !d->playing) jump(d, d->pos + dir * beat_len(d) / 32);
    if (!dir) { d->bend = 0; d->bend_left = 0; }
}

/* pad functions, in the order of the tile filmstrip (art/make_pads.py): value = function * 3 + light (0 dim, 1 on, 2 pressed) */
enum { PF_CUE, PF_PLAY, PF_NDN, PF_NUP, PF_SYNC, PF_JBACK, PF_JFWD, PF_LOOP, PF_HC1, PF_ROLL1 = PF_HC1 + 8,
       /* while the browser is open the pads drive it */
       PF_B_PGUP = PF_ROLL1 + 4, PF_B_UP, PF_B_CLOSE, PF_B_BACK, PF_B_DOWN, PF_B_OPEN, PF_B_PGDN, PF_B_LOADA, PF_B_LOADB,
       PF_B_NONE,
       /* 0.8: deck switch, tempo and SHIFT pads, and what SHIFT turns pads into */
       /* PF_BAR1: bank A pads 9-11, loops of 2, 4 and 8 bars (these three frames were DEL 1-3 before 0.9.4) */
       PF_DECKA, PF_DECKB, PF_BPMUP, PF_BPMDN, PF_SHIFT, PF_BAR1, PF_HALF = PF_BAR1 + 3, PF_DOUBLE, PF_MKLEAD, PF_BPMUP1,
       PF_BPMDN1,
       /* bank B: auto loops of 1, 2, 4 and 8 beats */
       PF_ALOOP1, PF_N = PF_ALOOP1 + 4 };

/* browser pads (pad 1 bottom left):  13 PAGE UP   14 UP     15 -      16 CLOSE
 *                                      9 BACK      10 DOWN   11 OPEN   12 -
 *                                      5 PAGE DN    6 -       7 -       8 -
 *                                      1 LOAD A     2 LOAD B  3 -       4 -       UP / DOWN repeat while held */
static const int BROWSE_PADS[16] = {PF_B_LOADA, PF_B_LOADB, PF_B_NONE, PF_B_NONE, PF_B_PGDN, PF_B_NONE, PF_B_NONE,
                                    PF_B_NONE, PF_B_BACK, PF_B_DOWN, PF_B_OPEN, PF_B_NONE, PF_B_PGUP, PF_B_UP, PF_B_NONE,
                                    PF_B_CLOSE};

/* Deck pads (pad 1 bottom left), and with SHIFT (pad 8) held:
 *   13 DECK A   14 DECK B   15 LOOP     16 BPM +      SHIFT: 16 BPM +1
 *    9 2 BARS   10 4 BARS   11 8 BARS   12 BPM -      SHIFT: 12 BPM -1      (9-11: loops; again ends it)
 *    5 SYNC      6 JUMP -    7 JUMP +    8 SHIFT       SHIFT: 5 LEADER, 6 halve / 7 double the loop and jump length
 *    1 CUE       2 PLAY      3 NUDGE -   4 NUDGE +
 * (The hot cues are on bank B.) */
static int pad_shift(const deck_t *d) {
    return ((d->pad_held >> 7 & 1) && !(d->pad_browse >> 7 & 1)) || d->shift_latch;
}

/* Bank B (pad 1 bottom left): 13-16 hot cues 1-4, 9-12 hot cues 5-8, 5-8 loop rolls 1/8, 1/4, 1/2, 1 beat (held),
 * 1-4 loops of 1, 2, 4, 8 beats (on; the same pad again turns it off) */
static const int BANK_B[16] = {PF_ALOOP1, PF_ALOOP1 + 1, PF_ALOOP1 + 2, PF_ALOOP1 + 3, PF_ROLL1, PF_ROLL1 + 1, PF_ROLL1 + 2,
                               PF_ROLL1 + 3, PF_HC1 + 4, PF_HC1 + 5, PF_HC1 + 6, PF_HC1 + 7, PF_HC1, PF_HC1 + 1, PF_HC1 + 2,
                               PF_HC1 + 3};
static const int ALOOP_IDX[4] = {2, 3, 4, 5};   /* LOOP_BEATS index of 1, 2, 4, 8 beats */
static const int BAR_IDX[3] = {5, 6, 7};         /* ... of 2, 4, 8 bars (8, 16, 32 beats) */

/* k: 0-15 bank A, 16-31 bank B */
static int pad_func_mode(const deck_t *d, int k, int browse) {
    if (browse) return BROWSE_PADS[k & 15];
    if (k >= 16) return BANK_B[k - 16];
    static const int base[16] = {PF_CUE, PF_PLAY, PF_NDN, PF_NUP, PF_SYNC, PF_JBACK, PF_JFWD, PF_SHIFT,
                                 PF_BAR1, PF_BAR1 + 1, PF_BAR1 + 2, PF_BPMDN, PF_DECKA, PF_DECKB, PF_LOOP, PF_BPMUP};
    int f = base[k];
    if (pad_shift(d)) switch (f) {
        case PF_JBACK: f = PF_HALF; break;
        case PF_JFWD: f = PF_DOUBLE; break;
        case PF_SYNC: f = PF_MKLEAD; break;
        case PF_BPMUP: f = PF_BPMUP1; break;
        case PF_BPMDN: f = PF_BPMDN1; break;
    }
    return f;
}
static int pad_func(const deck_t *d, int k) { return pad_func_mode(d, k, d->browse); }

static int pad_lit(deck_t *d, int f) {
    switch (f) {
    case PF_B_LOADA: case PF_B_LOADB: return br_sel_kind(d) == 1;
    case PF_B_OPEN: return br_sel_kind(d) == 2;
    case PF_B_NONE: return 0;
    case PF_DECKA: return d->deck_id == 0;   /* the deck the screen and pads are on */
    case PF_DECKB: return d->deck_id == 1;
    case PF_SHIFT: return d->shift_latch;
    case PF_MKLEAD: return __atomic_load_n(&g_leader, __ATOMIC_ACQUIRE) == d;
    case PF_BAR1: case PF_BAR1 + 1: case PF_BAR1 + 2: return d->loop_on && d->loop_beats == BAR_IDX[f - PF_BAR1];
    case PF_ALOOP1: case PF_ALOOP1 + 1: case PF_ALOOP1 + 2: case PF_ALOOP1 + 3:
        return d->loop_on && d->loop_beats == ALOOP_IDX[f - PF_ALOOP1];
    case PF_CUE: return d->t && !d->playing && fabs(d->pos - d->cue) < 1;
    case PF_PLAY: return d->playing && !d->previewing;
    case PF_NDN: return d->bend < 0;
    case PF_NUP: return d->bend > 0;
    case PF_SYNC: return d->sync;
    case PF_LOOP: return d->loop_on;
    default: return f >= PF_HC1 && f < PF_HC1 + 8 && d->cues[f - PF_HC1] >= 0;   /* other browser pads: 0 */
    }
}

static int pad_value(deck_t *d, int k16) {   /* the tile for pad k16 of the bank last played, or the browser pad */
    int k = d->browse ? k16 : k16 + 16 * d->bank;
    int f = pad_func(d, k);
    int held = (d->pad_held >> k & 1) && !(d->pad_browse >> k & 1) == !d->browse;   /* held in this mode */
    int light = f == PF_B_NONE ? 0 : held || d->pad_flash[k16] > 0 || (f == PF_SHIFT && pad_shift(d)) ? 2 : pad_lit(d, f);
    return f * 3 + light;
}

/* Pads that take the deck's colour (A blue, B red), so a glance says which deck they act on: CUE, pads 5-8 and every
 * hot cue (and what SHIFT turns them into). The same list is in art/make_pads.py. */
static int pad_deck_coloured(int f) {
    return f == PF_CUE || f == PF_SYNC || f == PF_JBACK || f == PF_JFWD || f == PF_SHIFT || (f >= PF_HC1 && f < PF_HC1 + 8)
        || f == PF_HALF || f == PF_DOUBLE || f == PF_MKLEAD;
}
#define PAD_BLANK (PF_N * 3)   /* an empty frame: the big tiles while the browser is open */

static int pad_of_note(int note) {
    for (int k = 0; k < NPADS; k++) if (g_padnote[k] == note) return k;
    return -1;
}

static void pad_browse(deck_t *d, int f, int on) {
    if (!on) { if ((f == PF_B_UP || f == PF_B_DOWN) && d->brep_dir == (f == PF_B_UP ? -1 : 1)) d->brep_dir = 0; return; }
    switch (f) {
    case PF_B_UP: case PF_B_DOWN: d->brep_dir = f == PF_B_UP ? -1 : 1; d->brep_frames = 0; br_move(d, d->brep_dir); break;
    case PF_B_PGUP: case PF_B_PGDN: br_page(d, f == PF_B_PGDN); break;
    case PF_B_BACK: br_back(d); break;
    case PF_B_OPEN: br_open_sel(d); break;
    case PF_B_LOADA: case PF_B_LOADB: br_load_to(d, f == PF_B_LOADB); break;
    case PF_B_CLOSE: d->browse = 0; break;
    }
}

static void pad_press(deck_t *d, int k, int on, int note) {
    int browse = on ? d->browse : (int)(d->pad_browse >> k & 1);   /* a release goes where its press went */
    if (on) { d->pad_held |= 1u << k; if (browse) d->pad_browse |= 1u << k; else d->pad_browse &= ~(1u << k); }
    else { d->pad_held &= ~(1u << k); d->pad_browse &= ~(1u << k); }
    if (browse) { pad_browse(d, pad_func_mode(d, k, 1), on); d->rev++; return; }
    int fk = pad_func_mode(d, k, 0);
    if (on && fk != PF_SHIFT && d->shift_latch) d->shift_latch = 0;   /* a latched SHIFT lasts one pad */
    switch (fk) {
    case PF_CUE: if (on) cue_press(d); else cue_release(d); break;
    case PF_PLAY: if (on) play_toggle(d, !d->playing || d->previewing); break;
    case PF_NDN: case PF_NUP: {
        int dir = pad_func_mode(d, k, 0) == PF_NDN ? -1 : 1;
        if (on) hold_nudge(d, dir); else if (d->hold_dir == dir) hold_nudge(d, 0);
        break;
    }
    case PF_SYNC: if (on) { d->sync = !d->sync; d->sync_trim = 0; } break;
    case PF_JBACK: case PF_JFWD: if (on) beat_jump(d, (pad_func_mode(d, k, 0) == PF_JBACK ? -1 : 1) * LOOP_BEATS[d->loop_beats]); break;
    case PF_LOOP: if (on) loop_set(d, !d->loop_on); break;
    case PF_BPMUP: case PF_BPMDN: case PF_BPMUP1: case PF_BPMDN1: {
        int f = pad_func_mode(d, k, 0);
        if (on) tempo_step(d, f == PF_BPMUP ? 0.1 : f == PF_BPMDN ? -0.1 : f == PF_BPMUP1 ? 1.0 : -1.0);
        break;
    }
    case PF_SHIFT: case PF_DECKA: case PF_DECKB: break;   /* SHIFT is its held bit; the deck pads act in midi() */
    case PF_BAR1: case PF_BAR1 + 1: case PF_BAR1 + 2:
        if (on) {
            int idx = BAR_IDX[fk - PF_BAR1];
            if (d->loop_on && d->loop_beats == idx) loop_set(d, 0);
            else { d->loop_beats = idx; loop_set(d, 1); }
        }
        break;
    case PF_HALF: case PF_DOUBLE:
        if (on) {
            int lb = d->loop_beats + (pad_func_mode(d, k, 0) == PF_HALF ? -1 : 1);
            d->loop_beats = lb < 0 ? 0 : lb > 7 ? 7 : lb;
            if (d->loop_on) loop_set(d, 1);
        }
        break;
    case PF_MKLEAD: if (on && __atomic_load_n(&g_leader, __ATOMIC_ACQUIRE) != d) set_leader(d); break;
    case PF_ALOOP1: case PF_ALOOP1 + 1: case PF_ALOOP1 + 2: case PF_ALOOP1 + 3:
        if (on) {
            int idx = ALOOP_IDX[fk - PF_ALOOP1];
            if (d->loop_on && d->loop_beats == idx) loop_set(d, 0);
            else { d->loop_beats = idx; loop_set(d, 1); }
        }
        break;
    default: {
        int f = pad_func_mode(d, k, 0);
        if (f >= PF_ROLL1 && f < PF_ROLL1 + 4) { if (on) roll_on(d, note, ROLL_BEATS[f - PF_ROLL1]); else roll_off(d, note); }
        else if (on) hot_cue(d, f - PF_HC1);
    }
    }
    d->rev++;
}

/* A tap on a pad tile on the screen: a press and release in one (the screen sends no release), lit for a moment.
 * Hold-type pads act as a tap: NUDGE nudges briefly, CUE works like the CUE button, a roll pad does nothing. */
static void pad_tap(deck_t *d, int k16) {
    int k = d->browse ? k16 : k16 + 16 * d->bank;
    int f = pad_func(d, k);
    d->pad_flash[k16] = (int)(SR * 0.15);
    if (f == PF_SHIFT) { d->shift_latch = !d->shift_latch; d->rev++; return; }   /* the screen can't hold: SHIFT latches */
    if (f == PF_NDN || f == PF_NUP) {
        double dir = f == PF_NDN ? -1 : 1;
        if (d->playing) nudge(d, dir * 0.04, 0.15); else if (d->t) jump(d, d->pos + dir * beat_len(d) / 32);
    } else if (f == PF_CUE) cue_press(d);
    else if (f < PF_ROLL1 || f >= PF_ROLL1 + 4) { pad_press(d, k, 1, -1); pad_press(d, k, 0, -1); }
    d->rev++;
}

/* Pads, laid out like a player (pad 1 is bottom left on MPC):
 *   13-16  hot cues 1-4
 *    9-12  (see pad_func_mode for the layout since 0.8)
 *    5-8   SYNC, beat jump back, beat jump forward (by Loop Length), LOOP on/off
 *    1-4   CUE (hold to preview), PLAY/PAUSE, NUDGE - and NUDGE + (hold: bends while held; paused: steps 1/32 beat)
 * A pad is found by its note in g_padnote, so any pad mode works once learned. */
static void midi_local(void *inst, const uint8_t *msg, int len) {
    deck_t *d = inst;
    if (len < 3) return;
    int st = msg[0] & 0xf0, note = msg[1], vel = msg[2];
    int on = st == 0x90 && vel > 0, off = st == 0x80 || (st == 0x90 && vel == 0);
    if (!on && !off) return;
    if (on) { d->last_note = note; d->rev++; }
    int k = pad_of_note(note);
    if (k < 0) return;
    if (off && !(d->pad_held >> k & 1)) return;   /* a release whose press was learned or went elsewhere */
    if (on && (k >= 16) != d->bank) { d->bank = k >= 16; d->rev++; }   /* the tiles follow the bank in use */
    pad_press(d, k, on, note);
}

/* ---------- parameters ---------- */

static void apply_restore(deck_t *d) {
    if (!d->want_valid || !d->t || strcmp(d->t->name, d->want_name)) return;
    for (int i = 0; i < NCUES; i++) d->cues[i] = d->want_cues[i];
    d->cue = d->want_cue;
    if (d->want_bpm > 0) { d->bpm = d->want_bpm; d->anchor = d->want_anchor; }
    d->pos = d->cue;
    d->want_valid = 0;
}

/* Settings kept in the project's state chunk (the wrapper saves only this chunk). */
static const char *SAVED[] = {"volume", "pitch", "range", "sync", "filter", "eq_low", "eq_mid", "eq_high", "loop_len",
                              "pad_mode", "pad_base", "follow", "deck_id", "pad_set", "pad_lights", NULL};
static void set_local(void *inst, const char *key, const char *val);

static void restore_state(deck_t *d, const char *val) {
    if (strncmp(val, "decks1\n", 7)) return;
    char *buf = strdup(val), *p = buf, *line;
    char name[256] = "", path[1024] = "", dir[1024] = "";
    int have_dir = 0;
    double bpm = 0, anchor = 0, cue = 0, cues[NCUES];
    for (int i = 0; i < NCUES; i++) cues[i] = -1;
    while ((line = strsep(&p, "\n"))) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "name")) snprintf(name, sizeof name, "%s", v);
        else if (!strcmp(k, "path")) snprintf(path, sizeof path, "%s", v);
        else if (!strcmp(k, "dir")) { snprintf(dir, sizeof dir, "%s", v); have_dir = 1; }
        else if (!strcmp(k, "bpm")) bpm = atof(v);
        else if (!strcmp(k, "anchor")) anchor = atof(v);
        else if (!strcmp(k, "cue")) cue = atof(v);
        else if (k[0] == 'c' && k[1] >= '0' && k[1] < '0' + NCUES && !k[2]) cues[k[1] - '0'] = atof(v);
        else if (!strcmp(k, "pmap")) set_local(d, k, v);
        else for (int i = 0; SAVED[i]; i++) if (!strcmp(k, SAVED[i])) { set_local(d, k, v); break; }
    }
    free(buf);
    if (have_dir) { pthread_mutex_lock(&d->mu); br_open_dir(d, dir, path[0] ? path : NULL); pthread_mutex_unlock(&d->mu); }
    if (!name[0]) return;
    pthread_mutex_lock(&d->mu);
    snprintf(d->want_name, sizeof d->want_name, "%s", name);
    d->want_bpm = bpm; d->want_anchor = anchor; d->want_cue = cue;
    memcpy(d->want_cues, cues, sizeof cues);
    d->want_valid = 1; d->want_scanned = 0;
    if (path[0]) load_path(d, path);   /* a failure starts a search by name (worker) */
    else { d->want_scanned = 1; post(d, JOB_SCAN); }   /* older state: find it by name */
    pthread_mutex_unlock(&d->mu);
    /* the cues go on when the (re)loaded track arrives (take_ready), not onto the track playing now: a deck that
     * already had this track would get them and then lose them to the reload */
}

static void set_local(void *inst, const char *key, const char *val) {
    deck_t *d = inst;
    double v = atof(val);
    if (!strcmp(key, "volume")) d->vol = (float)v;
    else if (!strcmp(key, "track")) {   /* the Browse knob: moves the selection, and pops the browser up */
        pthread_mutex_lock(&d->mu);
        int was = d->sel;
        br_select(d, (int)lround(v));
        if (d->sel != was && !d->browse) d->browse = 1;
        pthread_mutex_unlock(&d->mu);
    }
    else if (!strcmp(key, "br_scroll")) {
        /* the browser's wheel: relative like JOG (reads back 0), so each wheel click or Q-Link event moves one row,
         * a fast turn more */
        double dv = rel_delta(&d->scroll_last, v);
        if (fabs(dv) > 1e-6) {
            int steps = (int)lround(dv * 64);
            if (!steps) steps = dv > 0 ? 1 : -1;
            br_move(d, steps);
        }
    }
    else if (!strcmp(key, "load_a") || !strcmp(key, "load_b")) { if (v >= 0.5) br_load_to(d, key[5] - 'a'); }
    else if (!strcmp(key, "load")) { if (v >= 0.5) br_open_sel(d); }
    else if (!strcmp(key, "browse")) { d->browse = (int)v ? 1 : 0; d->rev++; }
    else if (!strcmp(key, "br_open")) { if (v >= 0.5) { d->browse = 1; d->rev++; } }
    else if (!strcmp(key, "br_close")) { if (v >= 0.5) { d->browse = 0; d->rev++; } }
    else if (!strcmp(key, "br_up")) { if (v >= 0.5) br_back(d); }
    else if (!strcmp(key, "br_pgup") || !strcmp(key, "br_pgdn")) { if (v >= 0.5) br_page(d, key[6] == 'd'); }
    else if (!strcmp(key, "br_eject")) { if (v >= 0.5) br_eject(d); }
    else if ((!strncmp(key, "br_", 3) && key[3] >= '1' && key[3] <= '9') || (!strncmp(key, "brt_", 4) && v >= 0.5)) {
        /* a row tap. brt_N is a momentary: only its press counts, so a long press's release can't act as a second tap */
        int k = atoi(key + (key[2] == 't' ? 4 : 3));
        double t = now_s();
        if (k >= 1 && k <= BR_ROWS && t - d->last_tap > 0.25) {   /* one tap arrives as two presses 30-140 ms apart */
            d->last_tap = t;
            pthread_mutex_lock(&d->mu);
            int i = d->top + k - 1;
            if (d->bl && i < d->bl->n) { if (i == d->sel) br_activate(d); else br_select(d, i); }
            pthread_mutex_unlock(&d->mu);
        }
    }
    else if (!strcmp(key, "play")) { if ((int)v != d->playing || d->previewing) play_toggle(d, (int)v); }
    else if (!strcmp(key, "cue")) { if (v >= 0.5) cue_press(d); }
    else if (!strcmp(key, "pitch")) { if ((float)v != d->pitch) { d->pitch = (float)v; d->rev++; } }
    else if (!strcmp(key, "range")) { d->range = (int)v < 0 ? 0 : (int)v > 2 ? 2 : (int)v; d->rev++; }
    else if (!strcmp(key, "sync")) { d->sync = (int)v; d->sync_trim = 0; d->rev++; }
    else if (!strcmp(key, "filter")) d->filt = (float)v;
    else if (!strcmp(key, "eq_low")) d->eq[0] = (float)v;
    else if (!strcmp(key, "eq_mid")) d->eq[1] = (float)v;
    else if (!strcmp(key, "eq_high")) d->eq[2] = (float)v;
    else if (!strcmp(key, "loop_len")) { d->loop_beats = (int)v < 0 ? 0 : (int)v > 7 ? 7 : (int)v; if (d->loop_on) loop_set(d, 1); }
    else if (!strcmp(key, "loop")) loop_set(d, (int)v);
    else if (!strcmp(key, "bpm")) { if (v >= 40 && v <= 250) { d->bpm = v; d->rev++; } }
    else if (!strcmp(key, "pad_mode")) d->pad_mode = (int)v < 0 ? 0 : (int)v > 2 ? 2 : (int)v;
    else if (!strcmp(key, "pad_lights")) {
        int on = (int)v ? 1 : 0;
        if (d->pad_lights && !on && g_light_owner == d) lights_off();
        d->pad_lights = on; d->rev++;
    }
    else if (!strcmp(key, "pad_set")) { d->pad_set = (int)v ? 1 : 0; if (d->roll_note >= 0) roll_off(d, d->roll_note); }
    else if (!strcmp(key, "pad_base")) {   /* chromatic pads from this note (replaces a learned map) */
        d->pad_base = (int)v < 0 ? 0 : (int)v > 96 ? 96 : (int)v;
        for (int k = 0; k < NPADS; k++) g_padnote[k] = d->pad_base + k;
        g_learn_rev++;
    }
    else if (!strcmp(key, "pmap")) {   /* a learned pad map: 32 notes (bank A then B), or 16 from before 0.8 */
        int m[NPADS], i = 0; const char *q = val;
        while (i < NPADS && *q) { m[i++] = atoi(q); q = strchr(q, ','); if (!q) break; q++; }
        if (i == 16 || i == NPADS) { memcpy(g_padnote, m, sizeof m[0] * i); g_learn_rev++; }
    }
    else if (!strncmp(key, "tp_", 3)) { int k = atoi(key + 3) - 1; if (v >= 0.5 && k >= 0 && k < 16) pad_tap(d, k); }
    else if (!strcmp(key, "rescan")) {   /* re-read the folder on screen */
        if (v >= 0.5) {
            pthread_mutex_lock(&d->mu);
            if (d->bl) br_open_dir(d, d->bl->dir, d->sel < d->bl->n ? d->bl->path[d->sel] : NULL);
            pthread_mutex_unlock(&d->mu);
        }
    }
    else if (!strcmp(key, "jump_back")) { if (v >= 0.5) beat_jump(d, -LOOP_BEATS[d->loop_beats]); }
    else if (!strcmp(key, "jump_fwd")) { if (v >= 0.5) beat_jump(d, LOOP_BEATS[d->loop_beats]); }
    else if (!strcmp(key, "follow")) d->follow = (int)v ? 1 : 0;
    else if (!strcmp(key, "deck_id")) {
        /* The letters stay unique: taking a letter another deck has swaps the two. Two decks that both loaded as A
         * left DECK B pointing at nothing (Live II, 2026-11-18). */
        int was = d->deck_id, want = (int)v < 0 ? 0 : (int)v > 1 ? 1 : (int)v;
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < MAX_DECKS; i++)
            if (g_decks[i] && g_decks[i] != d && g_decks[i]->deck_id == want && want != was) {
                deck_t *o = g_decks[i];
                if (o->target == o->deck_id) o->target = was;
                o->deck_id = was; o->rev++;
            }
        d->deck_id = want; d->rev++;
        pthread_mutex_unlock(&g_mu);
        if (d->target == was) d->target = d->deck_id;   /* a deck controlling itself keeps doing so */
    }
    else if (!strcmp(key, "leader")) {
        deck_t *l = __atomic_load_n(&g_leader, __ATOMIC_ACQUIRE);
        if ((int)v && l != d) set_leader(d);
        else if (!(int)v && l == d) { publish(0, 0, 0, 0, 0); set_leader(NULL); }
    }
    else if (!strcmp(key, "nudge_dn")) { if (v >= 0.5) nudge(d, -0.04, 0.15); }
    else if (!strcmp(key, "nudge_up")) { if (v >= 0.5) nudge(d, 0.04, 0.15); }
    else if (!strcmp(key, "tempo_up") || !strcmp(key, "tempo_dn")) {   /* BPM +/-: 0.1 BPM per tap, by moving the pitch */
        static const float ranges[3] = {0.08f, 0.16f, 0.50f};
        (void)ranges;
        if (v >= 0.5) tempo_step(d, key[6] == 'u' ? 0.1 : -0.1);
    }
    else if (!strcmp(key, "pitch_reset")) { if (v >= 0.5) { d->pitch = 0; d->rev++; } }
    else if (!strcmp(key, "jog")) {
        /* a relative control: each wheel click, +/- press or Q-Link event moves it a little (see rel_delta) */
        double dv = rel_delta(&d->jog_last, v);
        if (fabs(dv) > 1e-6 && d->t) {
            double steps = dv / (1.0 / 64);   /* a Q-Link event is 1/128 of the -1..1 range, a wheel click 0.02 */
            if (d->playing) nudge(d, steps * 0.02, 0.12);
            else {   /* paused: scrub by 1/32 beat per click */
                long st = lround(steps); if (!st) st = steps > 0 ? 1 : -1;
                jump(d, d->pos + st * beat_len(d) / 32);
            }
        }
    }
    else if (!strcmp(key, "state")) restore_state(d, val);
}

static void fmt_time(char *b, int n, double frames) {
    int s = frames > 0 ? (int)(frames / SR) : 0;
    snprintf(b, n, "%d:%02d", s / 60, s % 60);
}

static int get_local(void *inst, const char *key, char *buf, int n) {
    deck_t *d = inst;
    static const char *RANGE[3] = {"8%", "16%", "50%"};
    if (!strcmp(key, "volume")) return snprintf(buf, n, "%g", d->vol);
    if (!strcmp(key, "track")) { pthread_mutex_lock(&d->mu); int r = snprintf(buf, n, "%d", d->sel); pthread_mutex_unlock(&d->mu); return r; }
    if (!strcmp(key, "track_display")) {   /* the selected entry's name, as the knob's value text */
        pthread_mutex_lock(&d->mu);
        int r = d->bl && d->sel < d->bl->n ? snprintf(buf, n, "%s%s", d->bl->name[d->sel], d->bl->isdir[d->sel] ? "/" : "") : snprintf(buf, n, "-");
        pthread_mutex_unlock(&d->mu);
        return r;
    }
    if (!strcmp(key, "browse")) return snprintf(buf, n, "%d", d->browse);
    if (!strcmp(key, "br_path")) {
        pthread_mutex_lock(&d->mu);
        int r;
        if (now_s() < d->warn_until) r = snprintf(buf, n, "%s", d->warn);
        else if (!d->bl || !d->bl->dir[0]) r = snprintf(buf, n, "DRIVES   (%d)", d->bl ? d->bl->n : 0);
        else {
            /* "USB DRIVE > House   (154)": the drive's name, not /media/..., and the last folders that fit */
            char p2[1100]; const char *dd = d->bl->dir;
            if (!strncmp(dd, "/media/", 7)) dd += 7;
            else if (d->data_dir[0] && !strncmp(dd, d->data_dir, strlen(d->data_dir))) { snprintf(p2, sizeof p2, "mpSEEJAY Tracks%s", dd + strlen(d->data_dir)); dd = p2; }
            char out[1100]; int o = 0;
            for (const char *c = dd; *c && o < (int)sizeof out - 4; c++) {
                if (*c == '/') { out[o++] = ' '; out[o++] = '>'; out[o++] = ' '; } else out[o++] = *c;
            }
            out[o] = 0;
            size_t l = strlen(out);
            r = snprintf(buf, n, "%s%s   (%d)", l > 52 ? "..." : "", l > 52 ? out + l - 52 : out, d->bl->n);
        }
        pthread_mutex_unlock(&d->mu);
        return r;
    }
    if (!strncmp(key, "brs_", 4) || !strncmp(key, "brd_", 4)) {
        /* brs_N: the row's background (0 normal, 1 selected, 2/3 on deck A/B, 4/5 the same selected, 6/7 folder, 8 empty);
         * brd_N: 1 lays the grey veil over a played track */
        int i = atoi(key + 4) - 1, sel, on, played;
        if (i < 0 || i >= BR_ROWS) return 0;
        int kind = br_row(d, i, &sel, &on, &played, NULL, 0);
        if (key[2] == 'd') return snprintf(buf, n, "%d", kind == 2 && played && !on);
        int f = kind == 0 ? 8 : kind == 1 ? 6 + sel : on ? (on == 1 ? 2 : 3) + 2 * sel : sel;
        return snprintf(buf, n, "%d", f);
    }
    if (!strncmp(key, "brt_", 4) || !strcmp(key, "br_eject")) return snprintf(buf, n, "0");
    if (!strncmp(key, "br_", 3) && key[3] >= '1' && key[3] <= '0' + BR_ROWS && !key[4]) {   /* the row's text, with what it is */
        char name[256]; int sel, on, played;
        int kind = br_row(d, atoi(key + 3) - 1, &sel, &on, &played, name, sizeof name);
        if (!kind) return snprintf(buf, n, " ");
        if (kind == 1) return snprintf(buf, n, "%s  >", name);   /* a folder: tap twice (or OPEN) to go in */
        char *dot = strrchr(name, '.');
        if (dot && dot != name && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".wav"))) *dot = 0;   /* no extension */
        return snprintf(buf, n, "%s", name);   /* the name alone: the row's colour says on a deck / played */
    }
    if (!strncmp(key, "br_", 3) && key[3] >= '1' && key[3] <= '9') {   /* row text, and "<row>_on" when it is the selection */
        char *us = strchr(key + 3, '_');
        int k = atoi(key + 3), on = us && !strcmp(us, "_on");
        if (us && !on) return 0;
        pthread_mutex_lock(&d->mu);
        int i = d->top + k - 1, r;
        if (on) r = snprintf(buf, n, "%d", d->bl && i == d->sel && i < d->bl->n);
        else if (d->bl && i < d->bl->n) r = snprintf(buf, n, "%s%s", d->bl->name[i], d->bl->isdir[i] ? "/" : "");
        else r = snprintf(buf, n, " ");
        pthread_mutex_unlock(&d->mu);
        return r;
    }
    if (!strcmp(key, "load") || !strcmp(key, "cue") || !strcmp(key, "rescan") || !strcmp(key, "jump_back") || !strcmp(key, "jump_fwd")
        || !strcmp(key, "br_open") || !strcmp(key, "br_close") || !strcmp(key, "br_up") || !strcmp(key, "br_pgup") || !strcmp(key, "br_pgdn"))
        return snprintf(buf, n, "0");
    if (!strcmp(key, "play")) return snprintf(buf, n, "%d", d->playing && !d->previewing);
    if (!strcmp(key, "pitch")) return snprintf(buf, n, "%g", d->pitch);
    if (!strcmp(key, "pitch_display")) {
        static const float ranges[3] = {8, 16, 50};
        return snprintf(buf, n, "%+.2f%%", d->pitch * ranges[d->range]);
    }
    if (!strcmp(key, "range")) return snprintf(buf, n, "%d", d->range);
    if (!strcmp(key, "range_display")) return snprintf(buf, n, "%s", RANGE[d->range]);
    if (!strcmp(key, "sync")) return snprintf(buf, n, "%d", d->sync);
    if (!strcmp(key, "filter")) return snprintf(buf, n, "%g", d->filt);
    if (!strcmp(key, "eq_low")) return snprintf(buf, n, "%g", d->eq[0]);
    if (!strcmp(key, "eq_mid")) return snprintf(buf, n, "%g", d->eq[1]);
    if (!strcmp(key, "eq_high")) return snprintf(buf, n, "%g", d->eq[2]);
    if (!strcmp(key, "loop_len")) return snprintf(buf, n, "%d", d->loop_beats);
    if (!strcmp(key, "loop")) return snprintf(buf, n, "%d", d->loop_on);
    if (!strcmp(key, "bpm")) return snprintf(buf, n, "%.2f", d->bpm > 0 ? d->bpm : 120.0);
    if (!strcmp(key, "pad_mode")) return snprintf(buf, n, "%d", d->pad_mode);
    if (!strcmp(key, "pad_set")) return snprintf(buf, n, "%d", d->pad_set);
    if (!strcmp(key, "pad_lights")) return snprintf(buf, n, "%d", d->pad_lights);
    if (!strcmp(key, "pad_base")) return snprintf(buf, n, "%d", d->pad_base);
    if (!strcmp(key, "display_rev")) return snprintf(buf, n, "%u", d->rev);
    if (!strcmp(key, "title")) {
        if (now_s() < d->warn_until) return snprintf(buf, n, "%s", d->warn);
        if (d->loading) { pthread_mutex_lock(&d->mu); int r = snprintf(buf, n, "Loading %s", d->load_name); pthread_mutex_unlock(&d->mu); return r; }
        return snprintf(buf, n, "%s", d->t ? d->t->name : "No track loaded");
    }
    if (!strcmp(key, "status")) {
        int r = 0;
        pthread_mutex_lock(&d->mu); r = snprintf(buf, n, "%s", d->status); pthread_mutex_unlock(&d->mu);
        return r;
    }
    if (!strcmp(key, "time")) {
        if (!d->t) return snprintf(buf, n, "--:-- / --:--");
        char a[16], b[16]; fmt_time(a, sizeof a, d->pos); fmt_time(b, sizeof b, (double)d->t->frames - d->pos);
        return snprintf(buf, n, "%s  -%s", a, b);
    }
    if (!strcmp(key, "tempo")) {
        if (!d->t) return snprintf(buf, n, "--- BPM");
        static const float ranges[3] = {0.08f, 0.16f, 0.50f};   /* the steady tempo: no phase correction or nudge */
        double r = d->sync && d->ref_bpm > 0 && d->bpm > 0 ? d->ref_bpm / d->bpm : 1.0 + d->pitch * ranges[d->range];
        char src[16] = "";
        if (d->sync) {
            if (d->ref_deck >= 0) snprintf(src, sizeof src, "  SYNC %c", 'A' + d->ref_deck);
            else if (d->ref_deck == -1) snprintf(src, sizeof src, "  SYNC MPC");
            else snprintf(src, sizeof src, "  SYNC --");
        }
        return snprintf(buf, n, "%.1f BPM%s%s", d->bpm * r, src, d->bpm > 0 ? "" : "?");
    }
    if (!strncmp(key, "pad_", 4) && key[4] >= '1' && key[4] <= '9') {   /* the big tiles on PLAY: empty in the browser */
        int k = atoi(key + 4) - 1;
        return k >= 0 && k < 16 ? snprintf(buf, n, "%d", d->browse ? PAD_BLANK : pad_value(d, k)) : 0;
    }
    if (!strncmp(key, "bpad_", 5)) {   /* the small pad map beside the browser */
        int k = atoi(key + 5) - 1;
        return k >= 0 && k < 16 ? snprintf(buf, n, "%d", pad_value(d, k)) : 0;
    }
    if (!strncmp(key, "tp_", 3)) return snprintf(buf, n, "0");
    if (!strcmp(key, "bank_label")) return snprintf(buf, n, d->browse ? "PADS: BROWSER" : d->bank ? "PADS: BANK B  HOT CUES" : "PADS: BANK A  DECK");
    if (!strcmp(key, "deck_color")) return snprintf(buf, n, "%d", d->end_warn == 2 ? 2 + d->deck_id : d->deck_id);   /* platter ring: deck colour, amber flash near the end */
    if (!strcmp(key, "bpm_big")) return d->t && d->bpm > 0 ? snprintf(buf, n, "%.1f", d->bpm * steady_rate(d)) : snprintf(buf, n, "---.-");
    if (!strcmp(key, "tempo_sub")) {   /* under the BPM: pitch, range and what sync follows */
        static const char *rn[3] = {"8", "16", "50"};
        double pct = (steady_rate(d) - 1) * 100;
        if (fabs(pct) < 0.005) pct = 0;
        char src[16] = "";
        if (d->sync) snprintf(src, sizeof src, d->ref_deck >= 0 ? "   SYNC %c" : d->ref_deck == -1 ? "   SYNC MPC" : "   SYNC --",
                              'A' + d->ref_deck);
        return snprintf(buf, n, "%+.2f%%   \xc2\xb1%s%%%s", pct, rn[d->range], src);
    }
    if (!strcmp(key, "tempo_up") || !strcmp(key, "tempo_dn")) return snprintf(buf, n, "0");
    if (!strcmp(key, "pitch_pct")) {   /* the pitch as a player shows it: percent and the fader's range */
        static const float ranges[3] = {0.08f, 0.16f, 0.50f};
        static const char *rn[3] = {"8", "16", "50"};
        double r = d->sync && d->ref_bpm > 0 && d->bpm > 0 ? d->ref_bpm / d->bpm : 1.0 + d->pitch * ranges[d->range];
        double pct = (r - 1) * 100;
        if (fabs(pct) < 0.005) pct = 0;
        return snprintf(buf, n, "%+.2f%%   \xc2\xb1%s", pct, rn[d->range]);
    }
    if (!strcmp(key, "follow")) return snprintf(buf, n, "%d", d->follow);
    if (!strcmp(key, "deck_id")) return snprintf(buf, n, "%d", d->deck_id);
    if (!strcmp(key, "leader")) return snprintf(buf, n, "%d", __atomic_load_n(&g_leader, __ATOMIC_ACQUIRE) == d);
    if (!strcmp(key, "deck_label")) {
        int lead = __atomic_load_n(&g_leader, __ATOMIC_ACQUIRE) == d;
        return snprintf(buf, n, "DECK %c%s", 'A' + d->deck_id, lead ? "  LEADER" : "");
    }
    if (!strcmp(key, "jog") || !strcmp(key, "br_scroll") || !strcmp(key, "load_a") || !strcmp(key, "load_b")) return snprintf(buf, n, "0");
    if (!strcmp(key, "nudge_dn") || !strcmp(key, "nudge_up") || !strcmp(key, "pitch_reset")) return snprintf(buf, n, "0");
    /* meters carry the deck's colour: each strip holds one set of frames per deck letter */
    if (!strcmp(key, "beat")) return snprintf(buf, n, "%d", d->deck_id * 5 + (d->t ? d->beat_shown : 0));
    if (!strcmp(key, "phase")) return snprintf(buf, n, "%d", d->deck_id * 34 + (d->t ? d->phase_shown : 0));
    if (!strncmp(key, "wf_", 3)) {
        int c = atoi(key + 3) - 1;
        if (c < 0 || c >= WF_COLS) return 0;
        int base = d->deck_id * 3 * WF_LEVELS;
        if (!d->t) return snprintf(buf, n, "%d", base);
        int st = c < d->wf_col ? 1 : c == d->wf_col ? 2 : 0;   /* 0 still to play, 1 played, 2 playhead */
        return snprintf(buf, n, "%d", base + st * WF_LEVELS + d->t->wf[c]);
    }
    if (!strcmp(key, "pads")) {   /* hot cues set, and the last pad note seen (to find pad_base) */
        char c[NCUES + 1];
        for (int i = 0; i < NCUES; i++) c[i] = d->cues[i] >= 0 ? '1' + i : '-';
        c[NCUES] = 0;
        if (d->last_note >= 0) return snprintf(buf, n, "%s  note %d", c, d->last_note);
        return snprintf(buf, n, "%s", c);
    }
    if (!strcmp(key, "state")) {
        /* one "key=value" per line; the track is saved by file name, not list position (the list changes) */
        char v[64];
        pthread_mutex_lock(&d->mu);
        char dir[1024]; snprintf(dir, sizeof dir, "%s", d->bl ? d->bl->dir : "");
        pthread_mutex_unlock(&d->mu);
        int w = snprintf(buf, n, "decks1\nname=%s\npath=%s\ndir=%s\nbpm=%.3f\nanchor=%.1f\ncue=%.1f\n",
                         d->t ? d->t->name : "", d->t ? d->t->path : "", dir, d->bpm, d->anchor, d->cue);
        for (int i = 0; i < NCUES && w < n; i++) w += snprintf(buf + w, n - w, "c%d=%.1f\n", i, d->cues[i]);
        for (int i = 0; SAVED[i] && w < n; i++) {
            if (get_local(d, SAVED[i], v, sizeof v) > 0) w += snprintf(buf + w, n - w, "%s=%s\n", SAVED[i], v);
        }
        if (w < n) w += snprintf(buf + w, n - w, "pmap=");   /* after pad_base, which would reset it */
        for (int k = 0; k < NPADS && w < n; k++) w += snprintf(buf + w, n - w, k < NPADS - 1 ? "%d," : "%d\n", g_padnote[k]);
        return w < n ? w : 0;
    }
    return 0;
}

/* ---------- audio ---------- */

static inline float hermite(float xm1, float x0, float x1, float x2, float t) {
    float c = (x1 - xm1) * 0.5f, v = x0 - x1, w = c + v, a = w + v + (x2 - x0) * 0.5f, b = w + a;
    return ((a * t - b) * t + c) * t + x0;
}

static inline void read_at(const track_t *t, double p, float *l, float *r) {
    int64_t i = (int64_t)floor(p);
    float f = (float)(p - (double)i);
    if (i < 1 || i + 2 >= t->frames) { *l = *r = 0; return; }
    const int16_t *s = t->pcm + (i - 1) * 2;
    *l = hermite(s[0], s[2], s[4], s[6], f) * (1.0f / 32768.0f);
    *r = hermite(s[1], s[3], s[5], s[7], f) * (1.0f / 32768.0f);
}

/* RBJ biquads: 0 low shelf 250 Hz, 1 peak 1.2 kHz, 2 high shelf 3.5 kHz. Knob -1..1: -1 kills (-30 dB), +1 = +6 dB. */
static void eq_coeffs(deck_t *d) {
    static const float FREQ[3] = {250, 1200, 3500};
    for (int b = 0; b < 3; b++) {
        if (d->eq[b] == d->eq_set[b]) continue;
        d->eq_set[b] = d->eq[b];
        float db = d->eq[b] >= 0 ? d->eq[b] * 6.0f : d->eq[b] * 30.0f;
        float A = powf(10, db / 40), w = 2 * (float)M_PI * FREQ[b] / SR, cw = cosf(w), sw = sinf(w);
        float b0, b1, b2, a0, a1, a2;
        if (b == 1) {
            float al = sw / (2 * 0.7f);
            b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A;
        } else {
            float al = sw / 2 * sqrtf(2), sq = 2 * sqrtf(A) * al;
            if (b == 0) {
                b0 = A * ((A + 1) - (A - 1) * cw + sq); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sq);
                a0 = (A + 1) + (A - 1) * cw + sq; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sq;
            } else {
                b0 = A * ((A + 1) + (A - 1) * cw + sq); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sq);
                a0 = (A + 1) - (A - 1) * cw + sq; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sq;
            }
        }
        float *c = d->bq[b];
        c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
    }
}

static inline float biquad(const float *c, float *z, float x) {   /* transposed direct form II */
    float y = c[0] * x + z[0];
    z[0] = c[1] * x - c[3] * y + z[1];
    z[1] = c[2] * x - c[4] * y;
    return y;
}

static void take_ready(deck_t *d) {
    if (pthread_mutex_trylock(&d->mu)) return;
    if (d->ready && !d->garbage) {
        track_t *old = d->t;
        d->t = d->ready; d->ready = NULL;
        d->garbage = old;
        if (old) pthread_cond_signal(&d->cv);
        d->bpm = d->t->bpm; d->anchor = d->t->anchor;
        d->pos = 0; d->cue = 0; d->fleft = 0; d->park = -1; d->playing = 0; d->previewing = 0;
        d->gain = d->gain_target = 0;
        d->loop_on = 0; d->roll_note = -1; d->sync_trim = 0; d->hold_dir = 0;
        for (int i = 0; i < NCUES; i++) d->cues[i] = -1;
        d->play_frames = 0; d->played_marked = 0;
        if (d->bpm > 0) d->cue = d->pos = d->anchor;   /* start on the first beat */
        d->rev++;
        apply_restore(d);
    }
    /* a restored track that wasn't in the list yet: load it once the scan has it */
    if (d->want_valid && !d->loading && !(d->jobs & JOB_LOAD) && !d->ready && d->list && !(d->jobs & JOB_SCAN)
        && (!d->t || strcmp(d->t->name, d->want_name))) {
        int found = -1;
        for (int i = 0; i < d->list->n; i++) if (!strcmp(d->list->names[i], d->want_name)) { found = i; break; }
        if (found >= 0) load_index(d, found);
        else { d->want_valid = 0; snprintf(d->status, sizeof d->status, "Missing: %.70s", d->want_name); d->rev++; }
    }
    pthread_mutex_unlock(&d->mu);
}

static void song_pos(void *inst, double ppq, double bpm, int playing, int frames) {
    deck_t *d = inst; (void)frames;
    d->host_bpm = bpm; d->host_ppq = ppq; d->host_playing = playing;
    /* Follow the MPC transport: PLAY plays from here, PLAY START (the song position back at the start) plays from the
     * cue point, STOP pauses. Only the moment it starts or stops counts, so the MPC looping its sequence changes nothing. */
    if (d->was_host_playing >= 0 && playing != d->was_host_playing && d->follow && d->t) {
        if (playing) {
            if (ppq >= 0 && ppq < 0.02) { d->park = -1; d->previewing = 0; jump(d, d->cue); }
            if (!d->playing) play_toggle(d, 1);
        } else if (d->playing) play_toggle(d, 0);
    }
    d->was_host_playing = playing;
}

/* Phase lock: while synced, playing and the host is playing, keep the deck's beat phase on the host's. An offset over
 * 0.08 beat (sync just switched on, play just pressed) snaps the playhead over (with the jump crossfade); smaller drift
 * is pulled in by nudging the rate, at most +-2%. Rolls keep their own phase, so it pauses during them. */
static void sync_phase(deck_t *d) {
    if (!d->sync || !d->playing || !d->ref_phase_ok || d->bpm <= 0 || d->roll_note >= 0) { d->sync_trim = 0; return; }
    double deck_ph = beat_at(d, d->pos), host_ph = d->ref_beat;
    double err = (host_ph - floor(host_ph)) - (deck_ph - floor(deck_ph));   /* in beats */
    if (err > 0.5) err -= 1; else if (err < -0.5) err += 1;
    if (fabs(err) > 0.08 && d->fleft == 0) {
        jump(d, d->pos + err * beat_len(d));
        if (d->loop_on && (d->pos < d->loop_a || d->pos >= d->loop_b)) d->pos = d->pos < d->loop_a ? d->pos + (d->loop_b - d->loop_a) : d->pos - (d->loop_b - d->loop_a);
        d->sync_trim = 0;
        return;
    }
    double t = err * 0.3;
    d->sync_trim = t > 0.02 ? 0.02 : t < -0.02 ? -0.02 : t;
}

static void render_block(deck_t *d, int16_t *out, int frames);
static void render(void *inst, int16_t *out, int frames) {   /* times each block for the log's heartbeat */
    deck_t *d = inst;
    if (g_debug != 1) { render_block(d, out, frames); return; }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    render_block(d, out, frames);
    clock_gettime(CLOCK_MONOTONIC, &b);
    unsigned us = (unsigned)((b.tv_sec - a.tv_sec) * 1000000 + (b.tv_nsec - a.tv_nsec) / 1000);
    if (us > d->rt_max_us) d->rt_max_us = us;
    d->rt_n++;
}

static void render_block(deck_t *d, int16_t *out, int frames) {
    if (d->qn || d->mqn) {   /* controls another deck's screen sent here */
        static __thread struct { char key[24]; char val[48]; } q[64];
        static __thread uint8_t mq[64][3];
        int qn = 0, mqn = 0;
        if (pthread_mutex_trylock(&d->qmu) == 0) {
            qn = d->qn; mqn = d->mqn;
            memcpy(q, d->q, sizeof q[0] * qn); memcpy(mq, d->mq, 3 * mqn);
            d->qn = d->mqn = 0;
            pthread_mutex_unlock(&d->qmu);
        }
        for (int i = 0; i < qn; i++) set_local(d, q[i].key, q[i].val);
        for (int i = 0; i < mqn; i++) midi_local(d, mq[i], 3);
    }
    take_ready(d);
    eq_coeffs(d);
    const track_t *t = d->t;
    deck_t *lead = __atomic_load_n(&g_leader, __ATOMIC_ACQUIRE);
    unsigned lr = __atomic_load_n(&g_leader_rev, __ATOMIC_ACQUIRE);
    if (lr != d->seen_leader_rev) { d->seen_leader_rev = lr; d->rev++; }
    if (!t) { memset(out, 0, (size_t)frames * 4); if (lead == d) publish(d->deck_id, 0, 0, 0, 0); return; }
    {   /* sync reference: the leader deck (when it isn't this one), else the MPC's clock */
        double lb, lbeat; int lpl, lid;
        d->ref_bpm = 0; d->ref_phase_ok = 0; d->ref_deck = -2;
        if (lead && lead != d && read_pub(&lb, &lbeat, &lpl, &lid) && lb > 0) {
            d->ref_bpm = lb; d->ref_beat = lbeat; d->ref_phase_ok = lpl; d->ref_deck = lid;
        } else if (d->host_bpm > 0) {
            d->ref_bpm = d->host_bpm; d->ref_beat = d->host_ppq; d->ref_phase_ok = d->host_playing && d->host_ppq >= 0; d->ref_deck = -1;
        }
    }
    sync_phase(d);
    const double rate = rate_now(d);
    if (d->brep_dir) {   /* a held UP / DOWN browser pad: repeats after 0.4 s, every 80 ms */
        if (!d->browse) d->brep_dir = 0;
        else {
            int was = d->brep_frames; d->brep_frames += frames;
            int start = (int)(SR * 0.4), every = (int)(SR * 0.08);
            if (d->brep_frames > start && (d->brep_frames - start) / every != (was > start ? (was - start) / every : -1))
                br_move(d, d->brep_dir);
        }
    }
    for (int k = 0; k < 16; k++) {   /* pad tiles: screen-tap lights run out; any change redraws */
        if (d->pad_flash[k] > 0) d->pad_flash[k] -= frames;
        int v = pad_value(d, k);
        if (v != d->pad_shown[k]) { d->pad_shown[k] = (uint8_t)v; d->rev++; }
    }
    if (d->hold_dir) {   /* a NUDGE pad held: bend 2% at first, growing to 8% over a second; paused: step every 60 ms */
        d->hold_frames += frames;
        if (d->playing) {
            double held = d->hold_frames / (double)SR;
            d->bend = d->hold_dir * (0.02 + 0.06 * (held > 1 ? 1 : held));
            d->bend_left = frames + 1;
        } else if (d->hold_frames > SR * 0.35) {
            int every = (int)(SR * 0.06), before = (d->hold_frames - frames) / every, now = d->hold_frames / every;
            if (now > before) jump(d, d->pos + d->hold_dir * beat_len(d) / 32);
        }
    } else if (d->bend_left > 0 && (d->bend_left -= frames) <= 0) { d->bend_left = 0; d->bend = 0; }
    {   /* display values that move by themselves: the waveform's playhead column, the beat in the bar, the phase */
        int col = (int)(d->pos * WF_COLS / (double)t->frames);
        int beat = d->bpm > 0 ? ((int)floor(beat_at(d, d->pos)) % 4 + 4) % 4 + 1 : 0;
        int ph = 0;
        if (d->ref_phase_ok && d->bpm > 0) {
            double db = beat_at(d, d->pos), err = (d->ref_beat - floor(d->ref_beat)) - (db - floor(db));
            if (err > 0.5) err -= 1; else if (err < -0.5) err += 1;
            ph = 1 + (int)lround((err + 0.5) * 32);   /* 1..33, 17 = in phase */
        }
        /* the last 30 s of a playing track: the platter ring flashes amber, twice a second */
        int ew = d->playing && !d->loop_on && t->frames - d->pos < 30.0 * SR ? 1 + ((int)(d->pos / (SR * 0.25)) & 1) : 0;
        if (col != d->wf_col || beat != d->beat_shown || ph != d->phase_shown || ew != d->end_warn) {
            d->wf_col = col; d->beat_shown = beat; d->phase_shown = ph; d->end_warn = ew; d->rev++;
        }
    }
    /* filter: knob < 0 low-pass 20 kHz -> 60 Hz, > 0 high-pass 20 Hz -> 10 kHz, centre dead zone = bypass */
    float fk = d->filt, mode = 0, fc = 0;
    if (fk < -0.03f) { mode = -1; fc = 20000.0f * powf(60.0f / 20000.0f, (-fk - 0.03f) / 0.97f); }
    else if (fk > 0.03f) { mode = 1; fc = 20.0f * powf(10000.0f / 20.0f, (fk - 0.03f) / 0.97f); }
    const float g = tanf((float)M_PI * (fc < 20000 ? fc : 20000) / SR), k = 1.0f / 0.9f;   /* Q 0.9: a little bite */
    const float a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float gstep = 1.0f / 220;   /* 5 ms play/stop ramp */
    const int fx = d->eq[0] != 0 || d->eq[1] != 0 || d->eq[2] != 0 || mode != 0;

    for (int i = 0; i < frames; i++) {
        /* playhead and loops */
        if (d->gain > 0 || d->gain_target > 0) {
            if (d->roll_note >= 0 && d->pos >= d->roll_b) jump(d, d->roll_a + (d->pos - d->roll_b));
            else if (d->roll_note < 0 && d->loop_on && d->pos >= d->loop_b) jump(d, d->loop_a + (d->pos - d->loop_b));
        }
        float l, r;
        read_at(t, d->pos, &l, &r);
        if (d->fleft > 0) {
            float fl, fr, w = (float)d->fleft / XFADE;
            read_at(t, d->fpos, &fl, &fr);
            l = l * (1 - w) + fl * w; r = r * (1 - w) + fr * w;
            d->fpos += rate; d->fleft--;
        }
        /* gain ramp */
        if (d->gain < d->gain_target) { d->gain += gstep; if (d->gain > d->gain_target) d->gain = d->gain_target; }
        else if (d->gain > d->gain_target) { d->gain -= gstep; if (d->gain < d->gain_target) d->gain = d->gain_target; }
        if (d->gain == 0 && d->park >= 0) { d->pos = d->park; d->park = -1; d->fleft = 0; }
        if (d->gain > 0) {
            d->pos += rate; d->play_frames++;
            if (d->roll_note >= 0) d->slip += rate;
            if (d->pos >= t->frames - 3) { d->pos = (double)(t->frames - 3); d->playing = 0; d->gain_target = 0; d->rev++; }
        }
        if (fx) {   /* EQ and filter: kept for old projects, off the screen since 0.3 (the DJ mixer does this) */
        l = biquad(d->bq[0], d->bz[0][0], l); r = biquad(d->bq[0], d->bz[0][1], r);
        l = biquad(d->bq[1], d->bz[1][0], l); r = biquad(d->bq[1], d->bz[1][1], r);
        l = biquad(d->bq[2], d->bz[2][0], l); r = biquad(d->bq[2], d->bz[2][1], r);
        /* filter (TPT state-variable) */
        float x[2] = {l, r};
        for (int c = 0; c < 2; c++) {
            float v3 = x[c] - d->svf_ic2[c];
            float v1 = a1 * d->svf_ic1[c] + a2 * v3;
            float v2 = d->svf_ic2[c] + a2 * d->svf_ic1[c] + a3 * v3;
            d->svf_ic1[c] = 2 * v1 - d->svf_ic1[c];
            d->svf_ic2[c] = 2 * v2 - d->svf_ic2[c];
            if (mode < 0) x[c] = v2;
            else if (mode > 0) x[c] = x[c] - k * v1 - v2;
        }
        l = x[0]; r = x[1];
        }
        float gl = d->gain * d->vol;
        float ol = l * gl * 32767.0f, orr = r * gl * 32767.0f;
        out[i * 2] = (int16_t)(ol > 32767 ? 32767 : ol < -32768 ? -32768 : ol);
        out[i * 2 + 1] = (int16_t)(orr > 32767 ? 32767 : orr < -32768 ? -32768 : orr);
    }
    if (lead == d) publish(d->deck_id, d->bpm > 0 ? d->bpm * rate : 0, beat_at(d, d->pos), d->playing && d->gain > 0, d->bpm > 0);
}

/* ---------- controlling another deck from this screen ----------
 * CONTROL picks the deck this screen and these pads drive. Controls for another deck go into that deck's queue and are
 * applied at the start of its next audio block (its own thread, under its own lock); readouts are read from it while
 * the deck list is locked, so it can't close underneath. Its audio still leaves through its own track and output. */
static int is_local_key(const char *k) {   /* this deck's own identity and plumbing: never sent to another deck */
    return !strcmp(k, "target") || !strcmp(k, "state") || !strcmp(k, "deck_id") || !strcmp(k, "display_rev")
        || !strcmp(k, "target_label") || !strcmp(k, "pad_learn") || !strcmp(k, "learn_label") || !strcmp(k, "pad_lights")
        /* volume is parameter 0, which MPC toggles on every STOP (NOTES: "MPC's host toggles parameter 0"): sent on to
         * the deck this screen controls, that toggle left deck B silent (Live II, 2026-11-18). Each deck keeps its own. */
        || !strcmp(k, "volume") || !strcmp(k, "show_a") || !strcmp(k, "show_b");
}
static int is_trigger_key(const char *k) {   /* reads back 0 whatever was sent */
    static const char *T[] = {"load", "cue", "rescan", "jump_back", "jump_fwd", "br_open", "br_close", "br_up", "br_pgup",
                              "br_pgdn", "nudge_dn", "nudge_up", "pitch_reset", "jog", "br_scroll", "load_a", "load_b",
                              "tempo_up", "tempo_dn", NULL};
    for (int i = 0; T[i]; i++) if (!strcmp(k, T[i])) return 1;
    if (!strncmp(k, "tp_", 3) || !strncmp(k, "brt_", 4) || !strcmp(k, "br_eject")) return 1;
    return !strncmp(k, "br_", 3) && k[3] >= '1' && k[3] <= '9';
}

/* the deck this one controls, with g_mu held (caller unlocks); NULL = this deck */
static deck_t *remote_lock(deck_t *d) {
    if (d->target == d->deck_id) return NULL;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_DECKS; i++) if (g_decks[i] && g_decks[i] != d && g_decks[i]->deck_id == d->target) return g_decks[i];
    pthread_mutex_unlock(&g_mu);
    /* no such deck: show this one, and say so, rather than a header for one deck over another's readouts */
    snprintf(d->warn, sizeof d->warn, "NO DECK %c: ADD A SECOND MPSEEJAY TRACK (SETUP: THIS PLUGIN IS DECK)", 'A' + d->target);
    d->warn_until = now_s() + 4.0;
    d->target = d->deck_id; d->rev++;
    return NULL;
}

/* Log for finding out what the MPC sends and what happened before a crash. On when /data/decks-debug (or
 * /tmp/decks-debug) exists on the MPC, checked when the MPC app starts. Goes to /data/decks.log on the internal drive, so
 * it survives a forced restart: every parameter set, every pad note, and a heartbeat every 2 s per deck (playing,
 * browser, longest audio block). Never synced from here: these run inside the plugin's lock, so a slow fsync would stall
 * the audio (the monitor script syncs every 10 s instead). Over 2 MB it moves to decks.log.1. Notes come in on
 * the audio thread, so they wait in a ring until the screen's next poll writes them. */
#define DBG_LOG "/data/decks.log"
static int g_debug = -1;
static char g_dbg_ring[64][24];
static unsigned g_dbg_w, g_dbg_r;
static pthread_mutex_t g_dbg_mu = PTHREAD_MUTEX_INITIALIZER;
static int dbg_on(void) {
    if (g_debug < 0) g_debug = access("/data/decks-debug", F_OK) == 0 || access("/tmp/decks-debug", F_OK) == 0;
    return g_debug;
}
static void dbg_sync(int sync, const char *fmt, ...) {
    if (!dbg_on()) return;
    pthread_mutex_lock(&g_dbg_mu);
    FILE *f = fopen(DBG_LOG, "a");
    if (f) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm; localtime_r(&ts.tv_sec, &tm);
        va_list ap; va_start(ap, fmt);
        fprintf(f, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
        vfprintf(f, fmt, ap); fputc('\n', f);
        va_end(ap);
        long size = ftell(f);
        fflush(f);
        if (sync) fsync(fileno(f));
        fclose(f);
        if (size > 2 * 1024 * 1024) rename(DBG_LOG, DBG_LOG ".1");
    }
    pthread_mutex_unlock(&g_dbg_mu);
}
#define dbg(...) dbg_sync(0, __VA_ARGS__)
static void dbg_note(int deck, const uint8_t *m) {
    if (g_debug != 1) return;
    unsigned w = __atomic_load_n(&g_dbg_w, __ATOMIC_RELAXED);
    snprintf(g_dbg_ring[w % 64], 24, "%c midi %02x %d %d", 'A' + deck, m[0], m[1], m[2]);
    __atomic_store_n(&g_dbg_w, w + 1, __ATOMIC_RELEASE);
}
static void dbg_flush(deck_t *d) {
    if (g_debug != 1) return;
    /* Every deck's screen thread drains the one ring, so take the read position under a lock and stop at a snapshot
     * of the write position, at most 64 lines a poll. Without the lock two threads raced on g_dbg_r: one passed the
     * other's snapshot and that one looped for ever, holding the plugin's lock and freezing MPC (Live II, 2026-11-18,
     * a burst of pad pressure messages). */
    static pthread_mutex_t rmu = PTHREAD_MUTEX_INITIALIZER;
    char lines[64][24]; int nl = 0;
    pthread_mutex_lock(&rmu);
    unsigned w = __atomic_load_n(&g_dbg_w, __ATOMIC_ACQUIRE);
    if ((int)(w - g_dbg_r) > 64) g_dbg_r = w - 64;
    while ((int)(w - g_dbg_r) > 0 && nl < 64) { memcpy(lines[nl++], g_dbg_ring[g_dbg_r % 64], 24); g_dbg_r++; }
    pthread_mutex_unlock(&rmu);
    for (int i = 0; i < nl; i++) dbg("%s", lines[i]);
    double t = now_s();
    if (t - d->dbg_beat >= 2.0) {   /* heartbeat */
        d->dbg_beat = t;
        unsigned mx = __atomic_exchange_n(&d->rt_max_us, 0, __ATOMIC_RELAXED), n = __atomic_exchange_n(&d->rt_n, 0, __ATOMIC_RELAXED);
        dbg_sync(0, "%c beat: target %c playing %d browse %d learn %d pos %.1fs rate %.3f blocks %u max %uus lights %d",
                 'A' + d->deck_id, 'A' + d->target, d->playing, d->browse, g_learn, d->pos / SR, d->t ? rate_now(d) : 0.0,
                 n, mx, d->pad_lights);
    }
}

/* ---------- the MPC's own pad lights (experimental) ----------
 * MPC drives the pad LEDs with sysex to the control surface's "Public" port (TheKikGen's MPC-LiveXplore wiki, "MPC
 * internal controller - Midi aspects"): F0 47 7F <product> 65 00 04 <pad> <r> <g> <b> F7, colours 0-127, product 47 for
 * a Live II (3B Live, 46 One, 3A X). MPC holds that port, so nobody outside can open it; a plugin runs inside MPC, so it
 * writes through MPC's own open handle, found in /proc/self/fd and checked with the rawmidi info ioctl. On a Live II
 * (3.10, 2026-11-18) headers 47 7F 47, 47 00 47 and 47 47 47 lit the pads; 3B, 46 and 4A did not. Pad order unverified
 * (assumed: index 0 = pad 1, bottom left, rows upward). */
struct dk_rawmidi_info {   /* struct snd_rawmidi_info from <sound/asound.h>, spelled out so the build needs no kernel headers */
    unsigned int device, subdevice; int stream, card; unsigned int flags;
    unsigned char id[64], name[80], subname[32];
    unsigned int subdevices_count, subdevices_avail;
    unsigned char reserved[64];
};
#define DK_RAWMIDI_IOCTL_INFO _IOR('W', 0x01, struct dk_rawmidi_info)
static int g_light_fd = -2;          /* -2 not looked yet, -1 none found */
struct dk_rawmidi_status {   /* struct snd_rawmidi_status */
    int stream; struct timespec tstamp; size_t avail; size_t xruns; unsigned char reserved[16];
};
#define DK_RAWMIDI_IOCTL_STATUS _IOWR('W', 0x20, struct dk_rawmidi_status)

/* room in MPC's output buffer to the pads, in bytes (-1: unknown). The fd is non-blocking, so a write into a nearly full
 * buffer could be cut short and leave half a message in MPC's stream to the pads. */
static long light_room(void) {
#ifdef __linux__
    struct dk_rawmidi_status st; memset(&st, 0, sizeof st);
    st.stream = 0;
    if (ioctl(g_light_fd, DK_RAWMIDI_IOCTL_STATUS, &st) < 0) return -1;
    return (long)st.avail;
#else
    return -1;
#endif
}
static deck_t *g_light_owner;        /* the deck whose track has the pads (the last to get a pad note or a screen touch) */

static int light_find_fd(void) {
#ifdef __linux__
    char path[64], link[128];
    int best = -1;
    for (int fd = 3; fd < 1024; fd++) {
        snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, link, sizeof link - 1);
        if (n <= 0) continue;
        link[n] = 0;
        if (strncmp(link, "/dev/snd/midiC", 14)) continue;
        int fl = fcntl(fd, F_GETFL);
        if (fl < 0 || (fl & O_ACCMODE) != O_WRONLY) continue;
        struct dk_rawmidi_info info; memset(&info, 0, sizeof info);
        info.stream = 0;   /* output */
        if (ioctl(fd, DK_RAWMIDI_IOCTL_INFO, &info) < 0) { dbg("lights: fd %d %s: no info", fd, link); continue; }
        dbg("lights: fd %d %s card %d sub %u name '%s' subname '%s'", fd, link, info.card, info.subdevice,
            (char *)info.name, (char *)info.subname);
        if (strstr((char *)info.subname, "Public") || strstr((char *)info.name, "Public")) best = fd;
        else if (best < 0 && info.subdevice == 0) best = fd;
    }
    dbg("lights: using fd %d", best);
    return best;
#else
    return -1;
#endif
}

static const uint8_t PAD_RGB[PF_N][3] = {   /* the tile colours (art/make_pads.py), 0-255 */
    {255, 154, 31}, {54, 217, 127}, {201, 209, 219}, {201, 209, 219}, {34, 211, 238}, {255, 210, 63}, {255, 210, 63},
    {255, 107, 213}, {255, 77, 77}, {255, 140, 26}, {255, 210, 63}, {61, 220, 132}, {34, 211, 238}, {59, 130, 246},
    {168, 85, 247}, {236, 72, 153}, {176, 124, 255}, {176, 124, 255}, {176, 124, 255}, {176, 124, 255},
    {201, 209, 219}, {232, 237, 242}, {138, 148, 163}, {255, 176, 77}, {232, 237, 242}, {54, 217, 127},
    {201, 209, 219}, {47, 155, 255}, {255, 77, 106}, {0, 0, 0},
    /* DECK A, DECK B, BPM +, BPM -, SHIFT, LOOP 2 / 4 / 8 BARS, HALF, DOUBLE, LEADER, BPM +1, BPM -1 */
    {47, 155, 255}, {255, 77, 106}, {232, 237, 242}, {232, 237, 242}, {255, 176, 77}, {255, 107, 213}, {255, 107, 213},
    {255, 107, 213}, {255, 210, 63}, {255, 210, 63}, {255, 176, 77}, {232, 237, 242}, {232, 237, 242},
    /* LOOP 1, 2, 4, 8 */
    {255, 107, 213}, {255, 107, 213}, {255, 107, 213}, {255, 107, 213}};
_Static_assert(sizeof PAD_RGB / sizeof PAD_RGB[0] == PF_N, "a colour for every pad function");
static uint8_t g_light_sent[16][3];
static double g_light_full, g_light_touch;

/* Light test (while /data/decks-lighttest exists): every pad one colour per candidate header, 2.5 s each, so a look at the
 * pads tells which header the Live II takes. The PLAY page names the candidate running. */
static const uint8_t LT_HDR[8][3] = {{0x47, 0x7F, 0x3B}, {0x47, 0x7F, 0x47}, {0x47, 0x00, 0x3B}, {0x47, 0x00, 0x47},
                                     {0x47, 0x47, 0x3B}, {0x47, 0x47, 0x47}, {0x47, 0x7F, 0x46}, {0x47, 0x47, 0x4A}};
static const uint8_t LT_RGB[8][3] = {{127, 0, 0}, {0, 127, 0}, {0, 0, 127}, {127, 127, 127}, {127, 127, 0}, {0, 127, 127},
                                     {127, 0, 127}, {127, 50, 0}};
static const char *LT_NAME[8] = {"RED", "GREEN", "BLUE", "WHITE", "YELLOW", "CYAN", "PURPLE", "ORANGE"};
static int g_lighttest = -1, g_lt_cur = -1;
static uint8_t g_hdr[3] = {0x47, 0x7F, 0x47};   /* device 7F, product 47: lit the pads on a Live II (2026-11-18) */

static void light_send(int k, const uint8_t rgb[3]) {
    uint8_t m[12] = {0xF0, g_hdr[0], g_hdr[1], g_hdr[2], 0x65, 0x00, 0x04, (uint8_t)k, rgb[0], rgb[1], rgb[2], 0xF7};
    if (write(g_light_fd, m, sizeof m) != (ssize_t)sizeof m) dbg("lights: write failed");
    memcpy(g_light_sent[k], rgb, 3);
}

/* called from the screen poll (not the audio thread): bring the pads up to date with the tiles of what d shows */
static void lights_update(deck_t *d, deck_t *shown) {
    if (g_light_owner != d || !d->pad_lights) return;
    if (g_light_fd == -2) g_light_fd = light_find_fd();
    if (g_light_fd < 0) return;
    double t = now_s();
    if (g_lighttest < 0) g_lighttest = access("/data/decks-lighttest", F_OK) == 0;
    if (g_lighttest) {
        int c = (int)(t / 2.5) % 8;
        if (c != g_lt_cur) {
            g_lt_cur = c; g_learn_rev++;
            memcpy(g_hdr, LT_HDR[c], 3);
            dbg("lights: test %d %s header %02x %02x %02x", c + 1, LT_NAME[c], g_hdr[0], g_hdr[1], g_hdr[2]);
            for (int k = 0; k < 16; k++) light_send(k, LT_RGB[c]);
        }
        return;
    }
    /* only while Decks is in use (a deck playing, or a pad or screen touch in the last 30 s), so another track's pads
     * get MPC's own colours back once you move on */
    int busy = t - g_light_touch < 30;
    for (int i = 0; i < MAX_DECKS && !busy; i++) if (g_decks[i] && g_decks[i]->playing) busy = 1;
    if (!busy) { g_light_full = 0; return; }
    /* MPC paints a pad itself when it's hit (green, then a yellow flash, then off), so go over them all 5 times a second */
    int full = t - g_light_full > 0.2;
    long room = light_room();
    if (room >= 0 && room < 16 * 12 + 64) return;   /* MPC's own traffic first: try again on the next poll */
    if (full) g_light_full = t;
    for (int k = 0; k < 16; k++) {
        int v = shown->pad_shown[k], f = v / 3, l = v % 3;
        if (f < 0 || f >= PF_N) continue;
        static const float LV[3] = {0.30f, 0.70f, 1.0f};   /* dim, on, pressed: the LEDs need more than the screen */
        /* DECK A / DECK B and PLAY say which deck you're on and whether it plays: nearly dark when not, full when so */
        static const float LV_STATE[3] = {0.06f, 1.0f, 1.0f};
        const float *lv = f == PF_DECKA || f == PF_DECKB || f == PF_PLAY ? LV_STATE : LV;
        static const uint8_t DECK_RGB[2][3] = {{47, 155, 255}, {255, 77, 106}};
        const uint8_t *base = pad_deck_coloured(f) && !shown->browse ? DECK_RGB[shown->deck_id & 1] : PAD_RGB[f];
        uint8_t rgb[3];
        for (int c = 0; c < 3; c++) rgb[c] = (uint8_t)(base[c] * lv[l] / 2.0f);   /* 0-255 -> 0-127 */
        if (full || memcmp(rgb, g_light_sent[k], 3)) light_send(k, rgb);
    }
}

static void lights_off(void) {
    static const uint8_t black[3] = {0, 0, 0};
    if (g_light_fd < 0) return;
    long room = light_room();
    if (room >= 0 && room < 16 * 12) return;
    for (int k = 0; k < 16; k++) light_send(k, black);
}

/* Pads 13 and 14 pick the deck this screen and these pads drive. They belong to this instance, not to the deck it is
 * showing, so they act here before anything is sent on; in the browser (on the deck shown) they are browser pads. */
static int deck_pad(deck_t *d, int k, int tile) {   /* tile: a screen tap, which hits the bank shown */
    if (k != 12 && k != 13) return 0;
    int browsing, bank;
    if (d->target == d->deck_id) { browsing = d->browse; bank = d->bank; }
    else {
        deck_t *o = remote_lock(d);
        browsing = o ? o->browse : d->browse; bank = o ? o->bank : d->bank;
        if (o) pthread_mutex_unlock(&g_mu);
    }
    if (browsing || (tile && bank)) return 0;
    d->target = k == 12 ? 0 : 1;
    d->rev++;
    /* a bank A pad was just played: the deck now shown shows bank A's tiles */
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_DECKS; i++) if (g_decks[i] && g_decks[i]->deck_id == d->target && g_decks[i]->bank) { g_decks[i]->bank = 0; g_decks[i]->rev++; }
    pthread_mutex_unlock(&g_mu);
    return 1;
}

static void set_param(void *inst, const char *key, const char *val) {
    deck_t *d = inst;
    dbg("%c set %s = %s", 'A' + d->deck_id, key, val);
    if (strcmp(key, "display_rev") && strcmp(key, "state")) { g_light_owner = d; g_light_touch = now_s(); }   /* a screen touch */
    if (!strcmp(key, "pad_learn")) {   /* start (or cancel) learning the pads: hit pads 1-16 in order */
        if (atof(val) >= 0.5) { g_learn = g_learn ? 0 : 1; g_learn_rev++; }
        return;
    }
    if (!strncmp(key, "tp_", 3) && atof(val) >= 0.5 && deck_pad(d, atoi(key + 3) - 1, 1)) return;   /* DECK A / B tiles */
    if (!strcmp(key, "show_a") || !strcmp(key, "show_b")) {   /* the big A / B switch at the top of PLAY */
        if (atof(val) >= 0.5) { d->target = key[5] - 'a'; d->rev++; }
        return;
    }
    if (!strcmp(key, "target")) { int v = atoi(val); d->target = v < 0 ? 0 : v > 1 ? 1 : v; d->rev++; return; }
    if (!strcmp(key, "deck_id")) {
        int was = d->deck_id;
        set_local(d, key, val);
        if (d->target == was) d->target = d->deck_id;   /* still controlling itself */
        return;
    }
    deck_t *o = is_local_key(key) ? NULL : remote_lock(d);
    if (!o) { set_local(d, key, val); return; }
    pthread_mutex_lock(&o->qmu);
    if (o->qn < 64) { snprintf(o->q[o->qn].key, sizeof o->q[0].key, "%s", key); snprintf(o->q[o->qn].val, sizeof o->q[0].val, "%s", val); o->qn++; }
    pthread_mutex_unlock(&o->qmu);
    pthread_mutex_unlock(&g_mu);
    if (!is_trigger_key(key)) {   /* read it back as sent until the other deck has applied it */
        int i = d->pendi++ % 8;
        snprintf(d->pend[i].key, sizeof d->pend[0].key, "%s", key); snprintf(d->pend[i].val, sizeof d->pend[0].val, "%s", val);
        d->pend[i].t = now_s();
    }
}

static int get_param(void *inst, const char *key, char *buf, int n) {
    deck_t *d = inst;
    if (!strcmp(key, "display_rev")) {
        dbg_flush(d);
        if (d->t && !d->played_marked && d->play_frames > 30 * SR) {   /* 30 s played: on the played list */
            d->played_marked = 1;
            pthread_mutex_lock(&g_mu); played_add(d->t->path); pthread_mutex_unlock(&g_mu);
            d->rev++;
        }
        if (d->pad_lights && g_light_owner == d) {
            deck_t *o = remote_lock(d);
            lights_update(d, o ? o : d);
            if (o) pthread_mutex_unlock(&g_mu);
        }
    }
    if (!strcmp(key, "target")) return snprintf(buf, n, "%d", d->target);
    if (!strcmp(key, "target_label")) {   /* which deck this screen shows */
        deck_t *o = remote_lock(d);
        int r;
        if (o) { r = snprintf(buf, n, "DECK %c  (FROM %c)", 'A' + o->deck_id, 'A' + d->deck_id); pthread_mutex_unlock(&g_mu); }
        else if (d->target != d->deck_id) r = snprintf(buf, n, "NO DECK %c", 'A' + d->target);
        else r = snprintf(buf, n, "DECK %c", 'A' + d->deck_id);
        return r;
    }
    if (!strcmp(key, "display_rev")) {   /* moves when this deck, or the deck it shows, changes */
        deck_t *o = remote_lock(d);
        unsigned r = d->rev * 31u + (unsigned)d->target + g_learn_rev * 104729u;
        if (o) { r += o->rev * 7919u; pthread_mutex_unlock(&g_mu); }
        pthread_mutex_lock(&g_mu);   /* the header shows both decks' tempo, so any deck's change counts */
        for (int i = 0; i < MAX_DECKS; i++) if (g_decks[i] && g_decks[i] != d && g_decks[i] != o) r += g_decks[i]->rev * 15485863u;
        pthread_mutex_unlock(&g_mu);
        return snprintf(buf, n, "%u", r);
    }
    if (!strcmp(key, "pad_learn") || !strcmp(key, "show_a") || !strcmp(key, "show_b")) return snprintf(buf, n, "0");
    if ((!strncmp(key, "wf_", 3) && key[3] >= '1' && key[3] <= '9') || !strncmp(key, "wfb_", 4)) {
        /* two waveform rows on every screen: wf_N is deck A's, wfb_N deck B's, whichever deck the screen is on */
        int letter = key[2] == 'b', r;
        char k2[16]; snprintf(k2, sizeof k2, "wf_%s", key + (letter ? 4 : 3));
        pthread_mutex_lock(&g_mu);
        deck_t *o = d->deck_id == letter ? d : NULL;
        if (!o) for (int i = 0; i < MAX_DECKS; i++) if (g_decks[i] && g_decks[i]->deck_id == letter) o = g_decks[i];
        r = o ? get_local(o, k2, buf, n) : snprintf(buf, n, "%d", letter * 3 * WF_LEVELS);   /* no such deck: a flat line */
        pthread_mutex_unlock(&g_mu);
        return r;
    }
    if (!strcmp(key, "bpm_a") || !strcmp(key, "bpm_b") || !strcmp(key, "sub_a") || !strcmp(key, "sub_b")) {
        /* both decks' tempo in the header, whichever deck the screen shows */
        int letter = key[4] - 'a', r = 0;
        pthread_mutex_lock(&g_mu);
        deck_t *o = d->deck_id == letter ? d : NULL;
        if (!o) for (int i = 0; i < MAX_DECKS; i++) if (g_decks[i] && g_decks[i]->deck_id == letter) o = g_decks[i];
        if (!o) r = snprintf(buf, n, key[0] == 'b' ? "---.-" : "NO DECK %c", 'A' + letter);
        else if (key[0] == 'b') r = get_local(o, "bpm_big", buf, n);
        else r = get_local(o, "tempo_sub", buf, n);
        pthread_mutex_unlock(&g_mu);
        return r;
    }
    if (!strcmp(key, "learn_label")) {
        if (g_lighttest == 1 && g_lt_cur >= 0) return snprintf(buf, n, "PAD LIGHT TEST %d: %s", g_lt_cur + 1, LT_NAME[g_lt_cur]);
        if (g_learn) return snprintf(buf, n, "LEARNING: BANK %c, HIT PAD %d   (TAP LEARN TO STOP)", g_learn > 16 ? 'B' : 'A',
                                     (g_learn - 1) % 16 + 1);
        int chrom = 1;
        for (int k = 1; k < NPADS; k++) if (g_padnote[k] != g_padnote[0] + k) chrom = 0;
        return chrom ? snprintf(buf, n, "PADS: NOTES %d-%d", g_padnote[0], g_padnote[NPADS - 1])
                     : snprintf(buf, n, "PADS: LEARNED (PAD 1 = NOTE %d)", g_padnote[0]);
    }
    if (g_learn && !strncmp(key, "pad_", 4) && key[4] >= '1' && key[4] <= '9') {   /* learning: light the pad to hit */
        int k = atoi(key + 4) - 1 + (g_learn > 16 ? 16 : 0);   /* the bank being learned */
        return snprintf(buf, n, "%d", k == g_learn - 1 ? 2 : k < g_learn - 1 ? 1 : 0);
    }
    if (is_local_key(key)) return get_local(d, key, buf, n);
    deck_t *o = remote_lock(d);
    if (!o) return get_local(d, key, buf, n);
    double t = now_s();
    for (int i = 0; i < 8; i++)
        if (d->pend[i].t > 0 && t - d->pend[i].t < 0.3 && !strcmp(d->pend[i].key, key)) {
            pthread_mutex_unlock(&g_mu);
            return snprintf(buf, n, "%s", d->pend[i].val);
        }
    t_gmu_held = 1;
    int r = get_local(o, key, buf, n);
    t_gmu_held = 0;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static void midi(void *inst, const uint8_t *msg, int len) {   /* the pads follow CONTROL too */
    deck_t *d = inst;
    /* Pads only: notes on and off. Everything else goes, above all pad pressure (dozens of messages a hit), which used to
     * take the deck lock on the audio thread and could fill the other deck's 64-slot queue, so a note-off was lost and a
     * held pad (nudge, CUE preview, roll) stayed held. */
    if (len < 3 || ((msg[0] & 0xf0) != 0x90 && (msg[0] & 0xf0) != 0x80)) return;
    dbg_note(d->deck_id, msg);
    if ((msg[0] & 0xf0) == 0x90) { g_light_owner = d; g_light_touch = now_s(); }
    if (g_learn && len >= 3) {   /* learning the pads: note-ons fill the map in pad order; nothing plays */
        if ((msg[0] & 0xf0) == 0x90 && msg[2] > 0) {
            int k = g_learn - 1, dup = 0;
            for (int i = 0; i < k; i++) if (g_padnote[i] == msg[1]) dup = 1;   /* the same pad twice: wait for the next */
            if (!dup) { g_padnote[k] = msg[1]; g_learn = k + 2 > NPADS ? 0 : k + 2; g_learn_rev++; }
        }
        return;
    }
    if (len < 3) return;
    int st = msg[0] & 0xf0, note = msg[1] & 0x7f, on = st == 0x90 && msg[2] > 0, off = st == 0x80 || (st == 0x90 && !msg[2]);
    if (off && d->note_to[note] == 0xFF) { d->note_to[note] = 0; return; }   /* release of a DECK A / B pad */
    if (on && deck_pad(d, pad_of_note(note), 0)) { d->note_to[note] = 0xFF; return; }
    int dest = d->target;
    if (on) d->note_to[note] = (uint8_t)(d->target + 1);
    else if (off && d->note_to[note]) { dest = d->note_to[note] - 1; d->note_to[note] = 0; }
    if (dest == d->deck_id) { midi_local(d, msg, len); return; }
    deck_t *o = NULL;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_DECKS; i++) if (g_decks[i] && g_decks[i] != d && g_decks[i]->deck_id == dest) o = g_decks[i];
    if (!o) { pthread_mutex_unlock(&g_mu); midi_local(d, msg, len); return; }   /* no such deck: play this one */
    pthread_mutex_lock(&o->qmu);
    if (o->mqn < 64) { memcpy(o->mq[o->mqn], msg, 3); o->mqn++; }
    pthread_mutex_unlock(&o->qmu);
    pthread_mutex_unlock(&g_mu);
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, NULL, song_pos };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }

/* The wrapper's optional hook (engine.h). Decks reports its own changes through display_rev instead; defining it as a
 * no-op also lets the desktop test link on macOS, whose linker doesn't accept an undefined weak symbol. */
int mpc_engine_changed(void *inst, char *key, int len) { (void)inst; (void)key; (void)len; return 0; }
