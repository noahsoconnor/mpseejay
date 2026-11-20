/* Reading a rekordbox USB export: the playlists in PIONEER/rekordbox/export.pdb, the database CDJs read.
 *
 * The file is DeviceSQL pages (format: Deep Symmetry's crate-digger notes, rekordbox_pdb.ksy). Every read is bounds-
 * checked, so a damaged or unexpected file gives fewer or no playlists, never a crash. Read-only: nothing on the stick is
 * ever written. Only what the browser needs is read: the playlist tree, the playlist entries, and each track's title,
 * artist, file path and BPM.
 *
 *   pdb_open / pdb_close            the whole file in memory (a few hundred KB for a few thousand tracks)
 *   rb_nodes                        every folder and playlist: id, parent, sort order, folder or not, name
 *   rb_entries                      a playlist's tracks, in order
 *   rb_tracks                       title, artist, path and BPM for a list of track ids
 */
#include <stdint.h>

#define RB_TRACKS 0
#define RB_ARTISTS 2
#define RB_TREE 7
#define RB_ENTRIES 8
#define PDB_MAX_BYTES (64u << 20)

typedef struct { unsigned char *b; size_t n; uint32_t plen, ntab; } pdb_t;

static uint32_t pu4(const pdb_t *p, size_t o) {
    if (o > p->n || p->n - o < 4) return 0;
    const unsigned char *b = p->b + o;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint32_t pu2(const pdb_t *p, size_t o) { return o > p->n || p->n - o < 2 ? 0 : (uint32_t)p->b[o] | (uint32_t)p->b[o + 1] << 8; }
static uint32_t pu1(const pdb_t *p, size_t o) { return o < p->n ? p->b[o] : 0; }

static void pdb_close(pdb_t *p) { free(p->b); memset(p, 0, sizeof *p); }

/* 0 when it looks like an export.pdb */
static int pdb_open(pdb_t *p, const char *path) {
    memset(p, 0, sizeof *p);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 64 || (unsigned long)n > PDB_MAX_BYTES || fseek(f, 0, SEEK_SET)) { fclose(f); return -1; }
    p->b = malloc((size_t)n);
    if (!p->b || fread(p->b, 1, (size_t)n, f) != (size_t)n) { fclose(f); pdb_close(p); return -1; }
    fclose(f);
    p->n = (size_t)n;
    p->plen = pu4(p, 4); p->ntab = pu4(p, 8);
    if (pu4(p, 0) != 0 || p->plen < 512 || p->plen > 65536 || (p->plen & (p->plen - 1)) || p->ntab == 0 || p->ntab > 64 || 28 + 16 * (size_t)p->ntab > p->n) {
        pdb_close(p); return -1;
    }
    return 0;
}

/* append the UTF-8 for code point c, if it fits */
static void u8put(char *out, int on, int *w, uint32_t c) {
    char t[4]; int k;
    if (c < 0x20) return;   /* no control characters on screen */
    if (c < 0x80) { t[0] = (char)c; k = 1; }
    else if (c < 0x800) { t[0] = (char)(0xc0 | c >> 6); t[1] = (char)(0x80 | (c & 0x3f)); k = 2; }
    else if (c < 0x10000) { t[0] = (char)(0xe0 | c >> 12); t[1] = (char)(0x80 | (c >> 6 & 0x3f)); t[2] = (char)(0x80 | (c & 0x3f)); k = 3; }
    else { t[0] = (char)(0xf0 | c >> 18); t[1] = (char)(0x80 | (c >> 12 & 0x3f)); t[2] = (char)(0x80 | (c >> 6 & 0x3f)); t[3] = (char)(0x80 | (c & 0x3f)); k = 4; }
    if (*w + k >= on) return;
    memcpy(out + *w, t, k); *w += k; out[*w] = 0;
}

/* A DeviceSQL string at o, as UTF-8 in out. Short ASCII (odd first byte: length << 1 | 1), or long: kind byte 0x40 (ASCII)
 * or 0x90 (UTF-16LE), a 16-bit length including the 4-byte header, a pad byte, then the text. */
static int pdb_str(const pdb_t *p, size_t o, char *out, int on) {
    int w = 0;
    if (on < 1) return 0;
    out[0] = 0;
    if (o >= p->n) return 0;
    uint32_t k = p->b[o];
    if (k & 1) {
        uint32_t len = k >> 1;
        for (uint32_t i = 1; i < len && o + i < p->n; i++) u8put(out, on, &w, p->b[o + i]);
        return w;
    }
    uint32_t len = pu2(p, o + 1);
    if (len < 4 || o + 4 > p->n) return 0;
    len -= 4;
    if (len > p->n - o - 4) len = (uint32_t)(p->n - o - 4);
    const unsigned char *s = p->b + o + 4;
    if (k == 0x90) {
        for (uint32_t i = 0; i + 1 < len; i += 2) {
            uint32_t c = s[i] | s[i + 1] << 8;
            if (c >= 0xd800 && c < 0xdc00 && i + 3 < len) {   /* a surrogate pair */
                uint32_t c2 = s[i + 2] | s[i + 3] << 8;
                if (c2 >= 0xdc00 && c2 < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00); i += 2; }
            }
            if (c >= 0xd800 && c < 0xe000) continue;
            u8put(out, on, &w, c);
        }
    } else {
        for (uint32_t i = 0; i < len; i++) u8put(out, on, &w, s[i]);
    }
    return w;
}

/* Calls fn for every row of table `type`: follows the table's page chain, data pages only, rows marked present. */
typedef void (*pdb_row_fn)(const pdb_t *p, size_t row, size_t page_end, void *ctx);
static void pdb_rows(const pdb_t *p, uint32_t type, pdb_row_fn fn, void *ctx) {
    uint32_t first = 0, last = 0; int found = 0;
    for (uint32_t i = 0; i < p->ntab; i++) {
        size_t o = 28 + 16 * (size_t)i;
        if (pu4(p, o) == type) { first = pu4(p, o + 8); last = pu4(p, o + 12); found = 1; break; }
    }
    if (!found) return;
    size_t npages = p->n / p->plen;
    uint32_t page = first;
    for (size_t guard = 0; guard <= npages; guard++) {   /* a looping chain can't hang the browser */
        if (page >= npages) break;
        size_t base = (size_t)page * p->plen;
        if (pu4(p, base + 8) == type && !(pu1(p, base + 27) & 0x40)) {
            uint32_t small = pu1(p, base + 24), large = pu2(p, base + 34);
            uint32_t nrows = (large > small && large != 0x1fff) ? large : small;
            uint32_t groups = nrows ? (nrows - 1) / 16 + 1 : 0;
            for (uint32_t g = 0; g < groups; g++) {
                size_t gb = base + p->plen - (size_t)g * 0x24;
                if (gb < base + 0x28 + 0x24) break;
                uint32_t flags = pu2(p, gb - 4);
                for (uint32_t j = 0; j < 16 && g * 16 + j < nrows; j++) {
                    if (!(flags >> j & 1)) continue;
                    size_t row = base + 0x28 + pu2(p, gb - 6 - 2 * j);
                    if (row < base + p->plen) fn(p, row, base + p->plen, ctx);
                }
            }
        }
        if (page == last) break;
        page = pu4(p, base + 12);
    }
}

/* ---- the playlist tree ---- */
typedef struct { uint32_t id, parent, sort; int folder; char name[200]; } rb_node_t;
typedef struct { rb_node_t *v; int n, cap; } rb_nodes_ctx;

static void rb_node_row(const pdb_t *p, size_t r, size_t end, void *c) {
    rb_nodes_ctx *x = c;
    if (r + 20 >= end || x->n >= 20000) return;
    if (x->n == x->cap) {
        int nc = x->cap ? x->cap * 2 : 64;
        rb_node_t *nv = realloc(x->v, sizeof *nv * nc);
        if (!nv) return;
        x->v = nv; x->cap = nc;
    }
    rb_node_t *e = &x->v[x->n];
    e->parent = pu4(p, r); e->sort = pu4(p, r + 8); e->id = pu4(p, r + 12); e->folder = pu4(p, r + 16) != 0;
    pdb_str(p, r + 20, e->name, sizeof e->name);
    if (!e->id) return;   /* 0 is the root */
    if (!e->name[0]) snprintf(e->name, sizeof e->name, "%s %u", e->folder ? "Folder" : "Playlist", e->id);
    x->n++;
}

/* every folder and playlist; *out is malloc'd (free it) */
static int rb_nodes(const pdb_t *p, rb_node_t **out) {
    rb_nodes_ctx x = {0};
    pdb_rows(p, RB_TREE, rb_node_row, &x);
    *out = x.v;
    return x.n;
}

/* ---- a playlist's entries ---- */
typedef struct { uint32_t idx, track; } rb_entry_t;
typedef struct { rb_entry_t *v; int n, cap; uint32_t list; } rb_entries_ctx;

static void rb_entry_row(const pdb_t *p, size_t r, size_t end, void *c) {
    rb_entries_ctx *x = c;
    if (r + 12 > end || pu4(p, r + 8) != x->list || x->n >= 20000) return;
    if (x->n == x->cap) {
        int nc = x->cap ? x->cap * 2 : 64;
        rb_entry_t *nv = realloc(x->v, sizeof *nv * nc);
        if (!nv) return;
        x->v = nv; x->cap = nc;
    }
    x->v[x->n].idx = pu4(p, r); x->v[x->n].track = pu4(p, r + 4); x->n++;
}
static int rb_entry_cmp(const void *a, const void *b) {
    const rb_entry_t *x = a, *y = b;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

/* playlist `list`'s tracks in playlist order; *out is malloc'd (free it) */
static int rb_entries(const pdb_t *p, uint32_t list, rb_entry_t **out) {
    rb_entries_ctx x = {0};
    x.list = list;
    pdb_rows(p, RB_ENTRIES, rb_entry_row, &x);
    if (x.n > 1) qsort(x.v, x.n, sizeof *x.v, rb_entry_cmp);
    *out = x.v;
    return x.n;
}

/* ---- tracks ---- */
typedef struct { uint32_t id; char title[200], artist[200], path[1024]; float bpm; int rating; } rb_track_t;   /* rating 0-5 */
typedef struct { uint32_t id; int at; } rb_want_t;
typedef struct { rb_want_t *want; int nwant; rb_track_t *out; uint32_t *artist_id; } rb_tracks_ctx;
typedef struct { uint32_t id; char name[200]; } rb_artist_t;
typedef struct { rb_artist_t *v; int n, cap; } rb_artists_ctx;

static int rb_want_cmp(const void *a, const void *b) {
    const rb_want_t *x = a, *y = b;
    return x->id < y->id ? -1 : x->id > y->id;
}

static void rb_track_row(const pdb_t *p, size_t r, size_t end, void *c) {
    rb_tracks_ctx *x = c;
    if (r + 0x5E + 2 * 21 > end) return;
    rb_want_t key = { pu4(p, r + 0x48), 0 };
    rb_want_t *hit = bsearch(&key, x->want, x->nwant, sizeof key, rb_want_cmp);
    if (!hit) return;
    /* the same id can be wanted twice (a track twice in a playlist): fill every slot */
    while (hit > x->want && hit[-1].id == key.id) hit--;
    for (; hit < x->want + x->nwant && hit->id == key.id; hit++) {
        rb_track_t *t = &x->out[hit->at];
        t->id = key.id;
        t->bpm = pu4(p, r + 0x38) / 100.0f;
        t->rating = (int)pu1(p, r + 0x59);   /* rekordbox's stars, 0-5 */
        if (t->rating > 5) t->rating = 0;
        x->artist_id[hit->at] = pu4(p, r + 0x44);
        size_t s17 = r + pu2(p, r + 0x5E + 2 * 17), s20 = r + pu2(p, r + 0x5E + 2 * 20);
        if (s17 < end) pdb_str(p, s17, t->title, sizeof t->title);
        if (s20 < end) pdb_str(p, s20, t->path, sizeof t->path);
    }
}

static void rb_artist_row(const pdb_t *p, size_t r, size_t end, void *c) {
    rb_artists_ctx *x = c;
    if (r + 10 > end || x->n >= 100000) return;
    uint32_t sub = pu2(p, r), ofs = sub == 0x64 ? pu2(p, r + 0x0A) : pu1(p, r + 9);
    if (r + ofs >= end) return;
    if (x->n == x->cap) {
        int nc = x->cap ? x->cap * 2 : 256;
        rb_artist_t *nv = realloc(x->v, sizeof *nv * nc);
        if (!nv) return;
        x->v = nv; x->cap = nc;
    }
    x->v[x->n].id = pu4(p, r + 4);
    pdb_str(p, r + ofs, x->v[x->n].name, sizeof x->v[x->n].name);
    x->n++;
}
static int rb_artist_cmp(const void *a, const void *b) {
    const rb_artist_t *x = a, *y = b;
    return x->id < y->id ? -1 : x->id > y->id;
}

/* out[i] for ids[i]: title, artist, path (from the drive's root, starting with /) and BPM; a track not found keeps an
 * empty path. Returns how many were found. */
static int rb_tracks(const pdb_t *p, const uint32_t *ids, int n, rb_track_t *out) {
    if (n <= 0) return 0;
    rb_tracks_ctx x = { malloc(sizeof(rb_want_t) * n), n, out, calloc(n, sizeof(uint32_t)) };
    if (!x.want || !x.artist_id) { free(x.want); free(x.artist_id); return 0; }
    for (int i = 0; i < n; i++) { x.want[i].id = ids[i]; x.want[i].at = i; memset(&out[i], 0, sizeof out[i]); }
    qsort(x.want, n, sizeof *x.want, rb_want_cmp);
    pdb_rows(p, RB_TRACKS, rb_track_row, &x);
    rb_artists_ctx a = {0};
    pdb_rows(p, RB_ARTISTS, rb_artist_row, &a);
    if (a.n > 1) qsort(a.v, a.n, sizeof *a.v, rb_artist_cmp);
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (!out[i].path[0]) continue;
        found++;
        rb_artist_t key = { x.artist_id[i], "" };
        rb_artist_t *ar = x.artist_id[i] && a.n ? bsearch(&key, a.v, a.n, sizeof key, rb_artist_cmp) : NULL;
        if (ar) snprintf(out[i].artist, sizeof out[i].artist, "%s", ar->name);
    }
    free(a.v); free(x.want); free(x.artist_id);
    return found;
}
