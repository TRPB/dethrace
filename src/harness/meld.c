// Meld feature: merge all games listed under [Games] into one unified campaign
// at runtime. Tracks, opponents, and assets are combined without manual file
// copying. See harness/meld.h for the public API.

#include "harness/meld.h"
#include "harness/config.h"
#include "harness/os.h"
#include "harness/trace.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define MELD_SEP "\\"
#define MELD_SEP_CH '\\'
#else
#define MELD_SEP "/"
#define MELD_SEP_CH '/'
#endif

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

int gMeld_active = 0;
int gMeld_both_starting_cars = 0;
int gMeld_total_race_count = 0;
int gMeld_primary_race_count = 0;

#define MELD_MAX_GAMES 10
#define MELD_MAX_RACES 200
#define MELD_MAX_RACE_LINES 80
#define MELD_MAX_OPPONENTS 60
#define MELD_MAX_OPPO_LINES 40
#define MELD_LINE_LEN 512
#define MELD_MAX_MUSIC 64

// Per-game decode method (1 = supported XOR, 2 = demo-only, skipped).
static int s_game_method[MELD_MAX_GAMES];
// Whether each game contributed at least one unique race (for PARTSHOP).
static int s_game_contributed[MELD_MAX_GAMES];

// Pre-built merged file contents (plain text).
static char* s_races_buf = NULL;
static size_t s_races_len = 0;
static char* s_oppo_buf = NULL;
static size_t s_oppo_len = 0;
static char* s_partshop_buf = NULL;
static size_t s_partshop_len = 0;

// Race slot -> source game index.
static int s_race_source_game[MELD_MAX_RACES];
// Opponent slot -> source game index.
static int s_opponent_source_game[MELD_MAX_OPPONENTS];

// Currently active game for VFS routing.
static int s_active_game = 0;

// Music pool (absolute paths).
static char s_music_paths[MELD_MAX_MUSIC][MAX_PATH];
static int s_music_count = 0;
static int s_music_index = 0;

// Forward declarations for helpers defined later.
static void meld_join(char* dest, size_t len, const char* a, const char* b);
static const char* meld_basename(const char* path);

// ---------------------------------------------------------------------------
// Asset conflict map (Phase 3): basenames that differ across game dirs
// ---------------------------------------------------------------------------

#define MELD_MAX_CONFLICTS 512
#define MELD_CONFLICT_NAMELEN 64

static char s_conflict_basenames[MELD_MAX_CONFLICTS][MELD_CONFLICT_NAMELEN];
static int s_conflict_count = 0;

// ---------------------------------------------------------------------------
// XOR decode (method 1 only; method 2 used only by CARDEMO which has no unique
// content, so those games are skipped)
// ---------------------------------------------------------------------------

static const unsigned char MELD_KEY[16] = {
    0x6c, 0x1b, 0x99, 0x5f, 0xb9, 0xcd, 0x5f, 0x13,
    0xcb, 0x04, 0x20, 0x0e, 0x5e, 0x1c, 0xa1, 0x0e
};

static void meld_decode_method1(char* buf) {
    int len = (int)strlen(buf);
    int seed;
    int i;

    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
        buf[--len] = 0;
    }
    seed = len % 16;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0x9f) {
            c = '\t';
        }
        c = ((MELD_KEY[seed] ^ (c - 32)) & 0x7F) + 32;
        if (c == 0x9f) {
            c = '\t';
        }
        buf[i] = (char)c;
        seed = (seed + 7) % 16;
    }
}

// Strip trailing CR/LF in place.
static void meld_strip_eol(char* buf) {
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
        buf[--len] = 0;
    }
}

// Strip leading whitespace by shifting in place.
static void meld_strip_leading_ws(char* buf) {
    char* p = buf;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (p != buf) {
        memmove(buf, p, strlen(p) + 1);
    }
}

// Returns 1 if this line should be treated as a comment / skipped (mirrors
// GetALineWithNoPossibleService: keep lines starting with alnum or one of
// - . ! & ( ' ").
static int meld_is_comment(const char* buf) {
    char c = buf[0];
    if (c == 0) {
        return 1;
    }
    if (isalnum((unsigned char)c)) {
        return 0;
    }
    switch (c) {
    case '-':
    case '.':
    case '!':
    case '&':
    case '(':
    case '\'':
    case '"':
        return 0;
    default:
        return 1;
    }
}

// Read one meaningful data line from f (decoded if needed, skipping
// comments/blanks). Returns 0 on EOF, 1 on success. method selects decode.
static int meld_readline_m(FILE* f, char* buf, int maxlen, int method) {
    while (fgets(buf, maxlen, f) != NULL) {
        if (buf[0] == '@') {
            // shift off the '@' and decode
            memmove(buf, buf + 1, strlen(buf));
            if (method == 1) {
                meld_decode_method1(buf);
            }
        } else {
            meld_strip_eol(buf);
        }
        meld_strip_leading_ws(buf);
        if (meld_is_comment(buf)) {
            continue;
        }
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Hashing (FNV-1a of file contents; not cryptographic)
// ---------------------------------------------------------------------------

static uint64_t meld_fnv1a(const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Hash a file's full contents. Returns 0 if the file could not be opened.
static uint64_t meld_hash_file(const char* path) {
    FILE* f = OS_fopen(path, "rb");
    uint64_t h = 1469598103934665603ULL;
    unsigned char buf[4096];
    size_t n;
    if (f == NULL) {
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t i;
        for (i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
    }
    fclose(f);
    return h;
}

// ---------------------------------------------------------------------------
// Conflict map helpers
// ---------------------------------------------------------------------------

static int meld_is_conflict(const char* basename) {
    int i;
    for (i = 0; i < s_conflict_count; i++) {
        if (strcasecmp(s_conflict_basenames[i], basename) == 0) {
            return 1;
        }
    }
    return 0;
}

static void meld_add_conflict(const char* basename) {
    if (meld_is_conflict(basename)) {
        return;
    }
    if (s_conflict_count < MELD_MAX_CONFLICTS) {
        strncpy(s_conflict_basenames[s_conflict_count], basename, MELD_CONFLICT_NAMELEN - 1);
        s_conflict_basenames[s_conflict_count][MELD_CONFLICT_NAMELEN - 1] = 0;
        s_conflict_count++;
    }
}

// Scan one subdir across all game dirs; any file whose hash differs across
// two games is added to the conflict set.
static void meld_scan_subdir_conflicts(const char* subdir) {
    char dir_a[MAX_PATH];
    char file_a[MAX_PATH];
    char dir_b[MAX_PATH];
    char file_b[MAX_PATH];
    const char* fname;
    int g, other;
    int gc = harness_game_config.game_dirs_count;

    if (gc < 2) {
        return;
    }

    for (g = 0; g < gc && g < MELD_MAX_GAMES; g++) {
        meld_join(dir_a, sizeof(dir_a), harness_game_config.game_dirs[g].directory, subdir);
        fname = OS_GetFirstFileInDirectory(dir_a);
        while (fname != NULL) {
            if (fname[0] != '.' && !meld_is_conflict(fname)) {
                meld_join(file_a, sizeof(file_a), dir_a, fname);
                uint64_t ha = meld_hash_file(file_a);
                if (ha != 0) {
                    for (other = 0; other < gc && other < MELD_MAX_GAMES; other++) {
                        if (other == g) {
                            continue;
                        }
                        meld_join(dir_b, sizeof(dir_b), harness_game_config.game_dirs[other].directory, subdir);
                        meld_join(file_b, sizeof(file_b), dir_b, fname);
                        uint64_t hb = meld_hash_file(file_b);
                        if (hb != 0 && hb != ha) {
                            meld_add_conflict(fname);
                            break;
                        }
                    }
                }
            }
            fname = OS_GetNextFileInDirectory();
        }
    }
}

static void meld_build_conflict_map(void) {
    static const char* asset_subdirs[] = {
        "DATA/CARS", "DATA/MODELS", "DATA/PIXELMAP",
        "DATA/MATERIAL", "DATA/ACTORS", "DATA/ANIM", NULL
    };
    int i;

    for (i = 0; asset_subdirs[i] != NULL; i++) {
        meld_scan_subdir_conflicts(asset_subdirs[i]);
    }

    if (s_conflict_count > 0) {
        LOG_INFO2("Meld conflict map: %d conflicting asset basenames", s_conflict_count);
    }
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

static void meld_join(char* dest, size_t len, const char* a, const char* b) {
    snprintf(dest, len, "%s%s%s", a, MELD_SEP, b);
}

// Open DATA/<name> under a specific game directory.
static FILE* meld_open_data(int game_idx, const char* name, const char* mode) {
    char path[MAX_PATH];
    char data[MAX_PATH];
    meld_join(data, sizeof(data), harness_game_config.game_dirs[game_idx].directory, "DATA");
    meld_join(path, sizeof(path), data, name);
    return OS_fopen(path, mode);
}

// Detect the encode method for a game by inspecting DATA/GENERAL.TXT.
static int meld_detect_method(int game_idx) {
    FILE* f = meld_open_data(game_idx, "GENERAL.TXT", "rb");
    char buf[MELD_LINE_LEN];
    if (f == NULL) {
        return 0;
    }
    if (fgets(buf, sizeof(buf), f) == NULL) {
        fclose(f);
        return 0;
    }
    fclose(f);
    if (buf[0] != '@') {
        // plaintext file (already decoded) — treat as method 1
        return 1;
    }
    memmove(buf, buf + 1, strlen(buf));
    meld_decode_method1(buf);
    // GENERAL.TXT starts with version "0.01" for method 1.
    if (strncmp(buf, "0.01", 4) == 0) {
        return 1;
    }
    return 2;
}

// ---------------------------------------------------------------------------
// Dynamic text buffer
// ---------------------------------------------------------------------------

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} tMeld_buf;

static void mbuf_init(tMeld_buf* b) {
    b->cap = 4096;
    b->len = 0;
    b->data = (char*)malloc(b->cap);
    if (b->data) {
        b->data[0] = 0;
    }
}

static void mbuf_append(tMeld_buf* b, const char* s) {
    size_t sl = strlen(s);
    if (b->data == NULL) {
        return;
    }
    if (b->len + sl + 1 > b->cap) {
        size_t newcap = b->cap * 2;
        while (b->len + sl + 1 > newcap) {
            newcap *= 2;
        }
        char* nd = (char*)realloc(b->data, newcap);
        if (nd == NULL) {
            return;
        }
        b->data = nd;
        b->cap = newcap;
    }
    memcpy(b->data + b->len, s, sl + 1);
    b->len += sl;
}

// ---------------------------------------------------------------------------
// RACES.TXT merge
// ---------------------------------------------------------------------------

typedef struct {
    char lines[MELD_MAX_RACE_LINES][MELD_LINE_LEN];
    int num_lines;
    int game_idx;
    int order; // stable sort tie-break
    float norm;
} tMeld_race;

static tMeld_race s_races[MELD_MAX_RACES];
static int s_race_count = 0;

// Read one full race record (starting at its name line) into r. Returns:
//   1 = read a race, 0 = hit "END", -1 = EOF/error.
static int meld_read_one_race(FILE* f, int method, tMeld_race* r) {
    char s[MELD_LINE_LEN];
    int chunk_count;
    int j;

    r->num_lines = 0;
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    if (strcmp(s, "END") == 0) {
        return 0;
    }
    // name
    if (r->num_lines < MELD_MAX_RACE_LINES) {
        strcpy(r->lines[r->num_lines++], s);
    }
    // FLI/MAP/INFO line
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    if (r->num_lines < MELD_MAX_RACE_LINES) {
        strcpy(r->lines[r->num_lines++], s);
    }
    // TRACK.TXT line
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    if (r->num_lines < MELD_MAX_RACE_LINES) {
        strcpy(r->lines[r->num_lines++], s);
    }
    // chunk count
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    chunk_count = atoi(s);
    if (r->num_lines < MELD_MAX_RACE_LINES) {
        snprintf(r->lines[r->num_lines++], MELD_LINE_LEN, "%d", chunk_count);
    }
    for (j = 0; j < chunk_count; j++) {
        int line_count;
        int k;
        // x/y line
        if (!meld_readline_m(f, s, sizeof(s), method)) {
            return -1;
        }
        if (r->num_lines < MELD_MAX_RACE_LINES) {
            strcpy(r->lines[r->num_lines++], s);
        }
        // frame/frame_end line
        if (!meld_readline_m(f, s, sizeof(s), method)) {
            return -1;
        }
        if (r->num_lines < MELD_MAX_RACE_LINES) {
            strcpy(r->lines[r->num_lines++], s);
        }
        // line count
        if (!meld_readline_m(f, s, sizeof(s), method)) {
            return -1;
        }
        line_count = atoi(s);
        if (r->num_lines < MELD_MAX_RACE_LINES) {
            snprintf(r->lines[r->num_lines++], MELD_LINE_LEN, "%d", line_count);
        }
        for (k = 0; k < line_count; k++) {
            if (!meld_readline_m(f, s, sizeof(s), method)) {
                return -1;
            }
            if (r->num_lines < MELD_MAX_RACE_LINES) {
                strcpy(r->lines[r->num_lines++], s);
            }
        }
    }
    return 1;
}

// Sort key: ascending norm, tie-break by original insertion order (stable).
static int meld_race_cmp(const void* a, const void* b) {
    const tMeld_race* ra = (const tMeld_race*)a;
    const tMeld_race* rb = (const tMeld_race*)b;
    if (ra->norm < rb->norm) {
        return -1;
    }
    if (ra->norm > rb->norm) {
        return 1;
    }
    if (ra->game_idx != rb->game_idx) {
        return ra->game_idx - rb->game_idx;
    }
    return ra->order - rb->order;
}

static void meld_build_races(void) {
    int g;
    int i;
    tMeld_buf buf;

    s_race_count = 0;

    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        FILE* f;
        int method = s_game_method[g];
        int start;
        int count;
        int idx;

        if (method != 1) {
            continue;
        }
        f = meld_open_data(g, "RACES.TXT", "rb");
        if (f == NULL) {
            continue;
        }
        start = s_race_count;
        for (;;) {
            int rc;
            if (s_race_count >= MELD_MAX_RACES) {
                break;
            }
            rc = meld_read_one_race(f, method, &s_races[s_race_count]);
            if (rc <= 0) {
                break;
            }
            s_races[s_race_count].game_idx = g;
            s_races[s_race_count].order = s_race_count;
            s_race_count++;
        }
        fclose(f);
        count = s_race_count - start;
        if (count <= 0) {
            continue;
        }
        s_game_contributed[g] = 1;
        if (gMeld_primary_race_count == 0) {
            gMeld_primary_race_count = count;
        }
        // Assign proportional norm within this game.
        for (idx = 0; idx < count; idx++) {
            if (count == 1) {
                s_races[start + idx].norm = 0.0f;
            } else {
                s_races[start + idx].norm = (float)idx / (float)(count - 1);
            }
        }
    }

    gMeld_total_race_count = s_race_count;

    // Stable sort by norm then game order.
    qsort(s_races, s_race_count, sizeof(tMeld_race), meld_race_cmp);

    // Record per-slot source game and emit plain-text merged file.
    mbuf_init(&buf);
    for (i = 0; i < s_race_count; i++) {
        int l;
        s_race_source_game[i] = s_races[i].game_idx;
        for (l = 0; l < s_races[i].num_lines; l++) {
            mbuf_append(&buf, s_races[i].lines[l]);
            mbuf_append(&buf, "\n");
        }
    }
    mbuf_append(&buf, "END\n");

    s_races_buf = buf.data;
    s_races_len = buf.len;
}

// ---------------------------------------------------------------------------
// OPPONENT.TXT merge
// ---------------------------------------------------------------------------

typedef struct {
    char lines[MELD_MAX_OPPO_LINES][MELD_LINE_LEN];
    int num_lines;
    int game_idx;
    char car_file[256];
    uint64_t car_hash;
    char name[256];
    int car_number;
} tMeld_opponent;

static tMeld_opponent s_oppos[MELD_MAX_OPPONENTS * MELD_MAX_GAMES];
static int s_oppo_raw_count = 0;

// Read one opponent record (its 9 header lines + text chunks) into o.
// Returns 1 = read, 0 = "END", -1 = EOF/error.
static int meld_read_one_opponent(FILE* f, int method, int game_idx, tMeld_opponent* o) {
    char s[MELD_LINE_LEN];
    int chunk_count;
    int j;
    char tmp[MELD_LINE_LEN];
    char* tok;

    o->num_lines = 0;
    // name
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    if (strcmp(s, "END") == 0) {
        return 0;
    }
    strcpy(o->name, s);
    strcpy(o->lines[o->num_lines++], s);
    // abbrev
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // car number
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    o->car_number = atoi(s);
    strcpy(o->lines[o->num_lines++], s);
    // strength
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // net avail
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // mug shot
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // car file name
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    strcpy(tmp, s);
    tok = strtok(tmp, "\t ,/");
    if (tok) {
        strcpy(o->car_file, tok);
    } else {
        o->car_file[0] = 0;
    }
    // stolen car flic
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // chunk count
    if (!meld_readline_m(f, s, sizeof(s), method)) {
        return -1;
    }
    chunk_count = atoi(s);
    snprintf(o->lines[o->num_lines++], MELD_LINE_LEN, "%d", chunk_count);
    for (j = 0; j < chunk_count; j++) {
        int line_count;
        int k;
        if (!meld_readline_m(f, s, sizeof(s), method)) {
            return -1;
        }
        if (o->num_lines < MELD_MAX_OPPO_LINES) {
            strcpy(o->lines[o->num_lines++], s);
        }
        if (!meld_readline_m(f, s, sizeof(s), method)) {
            return -1;
        }
        if (o->num_lines < MELD_MAX_OPPO_LINES) {
            strcpy(o->lines[o->num_lines++], s);
        }
        if (!meld_readline_m(f, s, sizeof(s), method)) {
            return -1;
        }
        line_count = atoi(s);
        if (o->num_lines < MELD_MAX_OPPO_LINES) {
            snprintf(o->lines[o->num_lines++], MELD_LINE_LEN, "%d", line_count);
        }
        for (k = 0; k < line_count; k++) {
            if (!meld_readline_m(f, s, sizeof(s), method)) {
                return -1;
            }
            if (o->num_lines < MELD_MAX_OPPO_LINES) {
                strcpy(o->lines[o->num_lines++], s);
            }
        }
    }
    o->game_idx = game_idx;
    // Hash the car TXT file for dedup.
    {
        char carpath[MAX_PATH];
        char cars[MAX_PATH];
        meld_join(cars, sizeof(cars), harness_game_config.game_dirs[game_idx].directory, "DATA" MELD_SEP "CARS");
        meld_join(carpath, sizeof(carpath), cars, o->car_file);
        o->car_hash = meld_hash_file(carpath);
    }
    return 1;
}

static void meld_build_opponents(void) {
    int g;
    int i;
    int j;
    int out_count = 0;
    tMeld_buf buf;
    char numbuf[64];

    s_oppo_raw_count = 0;

    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        FILE* f;
        int method = s_game_method[g];
        char header[MELD_LINE_LEN];
        int declared;

        if (method != 1) {
            continue;
        }
        f = meld_open_data(g, "OPPONENT.TXT", "rb");
        if (f == NULL) {
            continue;
        }
        // count header
        if (!meld_readline_m(f, header, sizeof(header), method)) {
            fclose(f);
            continue;
        }
        declared = atoi(header);
        for (i = 0; i < declared; i++) {
            int rc;
            if (s_oppo_raw_count >= (int)(sizeof(s_oppos) / sizeof(s_oppos[0]))) {
                break;
            }
            rc = meld_read_one_opponent(f, method, g, &s_oppos[s_oppo_raw_count]);
            if (rc <= 0) {
                break;
            }
            s_oppo_raw_count++;
        }
        fclose(f);
    }

    // Dedup: include one entry per (name, car_hash). For player cars
    // (car_number < 0) and cops (500/501) dedup by car_hash only.
    // Mark included entries.
    {
        static int included[MELD_MAX_OPPONENTS * MELD_MAX_GAMES];
        for (i = 0; i < s_oppo_raw_count; i++) {
            int dup = 0;
            int special = (s_oppos[i].car_number < 0 || s_oppos[i].car_number == 500 || s_oppos[i].car_number == 501);
            for (j = 0; j < i; j++) {
                if (!included[j]) {
                    continue;
                }
                if (special) {
                    int j_special = (s_oppos[j].car_number < 0 || s_oppos[j].car_number == 500 || s_oppos[j].car_number == 501);
                    if (j_special && s_oppos[j].car_hash == s_oppos[i].car_hash && s_oppos[i].car_hash != 0) {
                        dup = 1;
                        break;
                    }
                } else {
                    if (strcmp(s_oppos[j].name, s_oppos[i].name) == 0 &&
                        s_oppos[j].car_hash == s_oppos[i].car_hash) {
                        dup = 1;
                        break;
                    }
                }
            }
            // MeldBothStartingCars: keep player-car duplicates for the same
            // character so both starting cars are available.
            if (dup && gMeld_both_starting_cars && s_oppos[i].car_number < 0) {
                // only dedup if identical car hash AND same name; otherwise keep
                int true_dup = 0;
                for (j = 0; j < i; j++) {
                    if (included[j] && s_oppos[j].car_number < 0 &&
                        strcmp(s_oppos[j].name, s_oppos[i].name) == 0 &&
                        s_oppos[j].car_hash == s_oppos[i].car_hash) {
                        true_dup = 1;
                        break;
                    }
                }
                dup = true_dup;
            }
            included[i] = !dup;
            if (included[i]) {
                out_count++;
            }
        }

        mbuf_init(&buf);
        snprintf(numbuf, sizeof(numbuf), "%d\n", out_count);
        mbuf_append(&buf, numbuf);

        {
            int slot = 0;
            for (i = 0; i < s_oppo_raw_count; i++) {
                int l;
                if (!included[i]) {
                    continue;
                }
                if (slot < MELD_MAX_OPPONENTS) {
                    s_opponent_source_game[slot] = s_oppos[i].game_idx;
                }
                slot++;
                for (l = 0; l < s_oppos[i].num_lines; l++) {
                    mbuf_append(&buf, s_oppos[i].lines[l]);
                    mbuf_append(&buf, "\n");
                }
            }
        }
        mbuf_append(&buf, "END\n");
    }

    s_oppo_buf = buf.data;
    s_oppo_len = buf.len;
}

// ---------------------------------------------------------------------------
// PARTSHOP.TXT merge
// ---------------------------------------------------------------------------

#define MELD_PS_CATEGORIES 3
#define MELD_PS_PARTS 6
#define MELD_PS_PRICES 3

typedef struct {
    int rank_required;
    char part_name[128];
    int prices[MELD_PS_PRICES];
} tMeld_part;

static void meld_build_partshop(void) {
    int g;
    int cat;
    int p;
    int k;
    tMeld_part parts[MELD_PS_CATEGORIES][MELD_PS_PARTS];
    int have_base = 0;
    uint64_t seen_hash[MELD_MAX_GAMES];
    int seen_count = 0;
    tMeld_buf buf;
    char line[MELD_LINE_LEN];

    memset(parts, 0, sizeof(parts));

    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        FILE* f;
        int method = s_game_method[g];
        char path[MAX_PATH];
        char data[MAX_PATH];
        uint64_t h;
        int dup = 0;
        char s[MELD_LINE_LEN];

        if (method != 1 || !s_game_contributed[g]) {
            continue;
        }
        meld_join(data, sizeof(data), harness_game_config.game_dirs[g].directory, "DATA");
        meld_join(path, sizeof(path), data, "PARTSHOP.TXT");
        h = meld_hash_file(path);
        for (k = 0; k < seen_count; k++) {
            if (seen_hash[k] == h && h != 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (seen_count < MELD_MAX_GAMES) {
            seen_hash[seen_count++] = h;
        }

        f = meld_open_data(g, "PARTSHOP.TXT", "rb");
        if (f == NULL) {
            continue;
        }
        for (cat = 0; cat < MELD_PS_CATEGORIES; cat++) {
            int part_count;
            if (!meld_readline_m(f, s, sizeof(s), method)) {
                break;
            }
            part_count = atoi(s);
            for (p = 0; p < part_count; p++) {
                char* tok;
                int rank;
                if (!meld_readline_m(f, s, sizeof(s), method)) {
                    break;
                }
                if (p >= MELD_PS_PARTS) {
                    continue;
                }
                tok = strtok(s, "\t ,/");
                if (!tok) {
                    continue;
                }
                rank = atoi(tok);
                if (!have_base) {
                    parts[cat][p].rank_required = rank;
                    tok = strtok(NULL, "\t ,/");
                    if (tok) {
                        strncpy(parts[cat][p].part_name, tok, sizeof(parts[cat][p].part_name) - 1);
                    }
                    for (k = 0; k < MELD_PS_PRICES; k++) {
                        tok = strtok(NULL, "\t ,/");
                        parts[cat][p].prices[k] = tok ? atoi(tok) : 0;
                    }
                } else {
                    // sum prices onto base
                    tok = strtok(NULL, "\t ,/"); // part name, ignore
                    for (k = 0; k < MELD_PS_PRICES; k++) {
                        tok = strtok(NULL, "\t ,/");
                        if (tok) {
                            parts[cat][p].prices[k] += atoi(tok);
                        }
                    }
                }
            }
        }
        fclose(f);
        have_base = 1;
    }

    mbuf_init(&buf);
    if (!have_base) {
        // No partshop found; leave buffer empty (VFS will fall through).
        s_partshop_buf = buf.data;
        s_partshop_len = buf.len;
        return;
    }
    for (cat = 0; cat < MELD_PS_CATEGORIES; cat++) {
        snprintf(line, sizeof(line), "%d\n", MELD_PS_PARTS);
        mbuf_append(&buf, line);
        for (p = 0; p < MELD_PS_PARTS; p++) {
            snprintf(line, sizeof(line), "%d\t%s\t%d\t%d\t%d\n",
                parts[cat][p].rank_required,
                parts[cat][p].part_name[0] ? parts[cat][p].part_name : "NONE",
                parts[cat][p].prices[0],
                parts[cat][p].prices[1],
                parts[cat][p].prices[2]);
            mbuf_append(&buf, line);
        }
    }
    s_partshop_buf = buf.data;
    s_partshop_len = buf.len;
}

// ---------------------------------------------------------------------------
// Music pool
// ---------------------------------------------------------------------------

static void meld_build_music(void) {
    int g;
    int t;
    s_music_count = 0;
    s_music_index = 0;

    // Deduplicate by a lightweight hash of the first 4KB + a track number scan.
    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        if (s_game_method[g] != 1) {
            continue;
        }
        // Tracks are Track02..Track09.
        for (t = 2; t <= 9; t++) {
            char rel[64];
            char path[MAX_PATH];
            uint64_t h;
            int k;
            int dup = 0;
            FILE* f;
            unsigned char head[4096];
            size_t n;

            snprintf(rel, sizeof(rel), "MUSIC" MELD_SEP "Track0%d.ogg", t);
            meld_join(path, sizeof(path), harness_game_config.game_dirs[g].directory, rel);
            f = OS_fopen(path, "rb");
            if (f == NULL) {
                continue;
            }
            n = fread(head, 1, sizeof(head), f);
            fseek(f, 0, SEEK_END);
            {
                long sz = ftell(f);
                h = meld_fnv1a(head, n) ^ (uint64_t)sz;
            }
            fclose(f);

            for (k = 0; k < s_music_count; k++) {
                // Compare against previously stored hashes (stored inline via
                // path recompute would be costly; use a parallel array).
                (void)k;
            }
            // Simple dedup: rehash stored paths lazily.
            {
                for (k = 0; k < s_music_count; k++) {
                    // recompute stored hash
                    FILE* sf = OS_fopen(s_music_paths[k], "rb");
                    if (sf) {
                        unsigned char sh[4096];
                        size_t sn = fread(sh, 1, sizeof(sh), sf);
                        long ssz;
                        uint64_t hh;
                        fseek(sf, 0, SEEK_END);
                        ssz = ftell(sf);
                        fclose(sf);
                        hh = meld_fnv1a(sh, sn) ^ (uint64_t)ssz;
                        if (hh == h) {
                            dup = 1;
                            break;
                        }
                    }
                }
            }
            if (dup) {
                continue;
            }
            if (s_music_count < MELD_MAX_MUSIC) {
                snprintf(s_music_paths[s_music_count], MAX_PATH, "%s", path);
                s_music_count++;
            }
        }
    }
}

int Meld_MusicAvailable(void) {
    return s_music_count > 0;
}

void Meld_ResolveMusicPath(int track, char* out, size_t len) {
    (void)track;
    if (s_music_count == 0) {
        snprintf(out, len, "MUSIC" MELD_SEP "Track02.ogg");
        return;
    }
    strncpy(out, s_music_paths[s_music_index % s_music_count], len - 1);
    out[len - 1] = 0;
    s_music_index++;
}

// ---------------------------------------------------------------------------
// Save path
// ---------------------------------------------------------------------------

void Meld_SavePath(int slot, char* out, size_t len) {
    (void)slot;
    snprintf(out, len, "SAVEGAME_M");
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void Meld_Init(void) {
    int g;

    if (!harness_game_config.meld || harness_game_config.game_dirs_count < 2) {
        return;
    }

    gMeld_both_starting_cars = harness_game_config.meld_both_starting_cars;

    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        s_game_method[g] = meld_detect_method(g);
        s_game_contributed[g] = 0;
    }

    meld_build_races();
    if (s_race_count == 0) {
        // Nothing merged; do not activate.
        return;
    }
    meld_build_opponents();
    meld_build_partshop();
    meld_build_music();

    meld_build_conflict_map();

    s_active_game = 0;
    gMeld_active = 1;

    LOG_INFO2("Meld active: %d races merged from games", gMeld_total_race_count);
}

// ---------------------------------------------------------------------------
// Merged file accessors
// ---------------------------------------------------------------------------

static FILE* meld_tmpfile_from(const char* data, size_t len) {
    FILE* f = tmpfile();
    if (f == NULL) {
        return NULL;
    }
    if (data != NULL && len > 0) {
        fwrite(data, 1, len, f);
    }
    fflush(f);
    fseek(f, 0, SEEK_SET);
    return f;
}

FILE* Meld_OpenRaceFile(void) {
    return meld_tmpfile_from(s_races_buf, s_races_len);
}

FILE* Meld_OpenOpponentFile(void) {
    return meld_tmpfile_from(s_oppo_buf, s_oppo_len);
}

FILE* Meld_OpenPartshopFile(void) {
    return meld_tmpfile_from(s_partshop_buf, s_partshop_len);
}

// ---------------------------------------------------------------------------
// Active-game selection
// ---------------------------------------------------------------------------

void Meld_SetActiveGame(int race_index) {
    if (race_index >= 0 && race_index < s_race_count) {
        s_active_game = s_race_source_game[race_index];
    }
}

void Meld_SetActiveGame_Opponent(int opponent_index) {
    if (opponent_index >= 0 && opponent_index < MELD_MAX_OPPONENTS) {
        s_active_game = s_opponent_source_game[opponent_index];
    }
}

// ---------------------------------------------------------------------------
// Phase 6: TXT file patching
// ---------------------------------------------------------------------------

// Scan a decoded line, replace any conflicting asset basename with "N:basename".
static void meld_patch_line(const char* line, int game_idx, char* out, size_t outsize) {
    const char* p = line;
    char* q = out;
    char* const qend = out + outsize - 1;

    while (*p && q < qend) {
        // Pass through separators unchanged.
        if (*p == ' ' || *p == '\t' || *p == ',' || *p == '/' ||
            *p == '\r' || *p == '\n') {
            *q++ = *p++;
            continue;
        }

        // Collect a token (run of non-separator chars).
        const char* tok_start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' &&
               *p != '/' && *p != '\r' && *p != '\n') {
            p++;
        }
        int tok_len = (int)(p - tok_start);

        if (tok_len >= 5 && tok_len < MELD_CONFLICT_NAMELEN) {
            char tok[MELD_CONFLICT_NAMELEN];
            memcpy(tok, tok_start, tok_len);
            tok[tok_len] = 0;

            // Must have a .ext of 2-4 chars.
            char* dot = strrchr(tok, '.');
            if (dot && dot != tok && (int)strlen(dot + 1) >= 2 &&
                (int)strlen(dot + 1) <= 4 && meld_is_conflict(tok)) {
                int written = snprintf(q, (size_t)(qend - q), "%d:%s", game_idx, tok);
                if (written > 0) {
                    q += written;
                }
                continue;
            }
        }

        // Not a conflicting filename; copy the token verbatim.
        int copy_len = tok_len;
        if (q + copy_len > qend) {
            copy_len = (int)(qend - q);
        }
        memcpy(q, tok_start, copy_len);
        q += copy_len;
    }
    *q = 0;
}

// Return 1 if this tail path should be decoded and patched for asset refs.
static int meld_needs_patching(const char* tail) {
    const char* base;
    size_t blen;

    base = meld_basename(tail);
    blen = strlen(base);

    // Must be a .TXT file.
    if (blen < 5 || strcasecmp(base + blen - 4, ".TXT") != 0) {
        return 0;
    }

    // Skip files whose content is already merged by meld.c itself.
    if (strcasecmp(base, "RACES.TXT") == 0 ||
        strcasecmp(base, "NETRACES.TXT") == 0 ||
        strcasecmp(base, "PEDRACES.TXT") == 0 ||
        strcasecmp(base, "OPPONENT.TXT") == 0 ||
        strcasecmp(base, "PARTSHOP.TXT") == 0 ||
        strcasecmp(base, "GENERAL.TXT") == 0) {
        return 0;
    }

    // Only patch TXT files in asset-bearing subdirs.
    if (strstr(tail, "RACES/") || strstr(tail, "RACES\\") ||
        strstr(tail, "races/") || strstr(tail, "races\\") ||
        strstr(tail, "CARS/")  || strstr(tail, "CARS\\")  ||
        strstr(tail, "cars/")  || strstr(tail, "cars\\")  ||
        strstr(tail, "NONCARS/") || strstr(tail, "NONCARS\\") ||
        strstr(tail, "noncars/") || strstr(tail, "noncars\\")) {
        return 1;
    }
    return 0;
}

// Read raw_path, decode @-prefixed lines (method 1), patch conflicting asset
// basenames to "N:basename", re-encode, and serve the result as a tmpfile.
static FILE* meld_patch_txt_serve(const char* raw_path, int game_idx, int method) {
    FILE* src;
    tMeld_buf buf;
    char line[1024];
    char decoded[1024];
    char patched[1024];

    src = OS_fopen(raw_path, "rt");
    if (src == NULL) {
        return NULL;
    }

    mbuf_init(&buf);

    while (fgets(line, sizeof(line), src) != NULL) {
        int is_encoded = (line[0] == '@') && (method == 1);

        if (is_encoded) {
            strncpy(decoded, &line[1], sizeof(decoded) - 1);
            decoded[sizeof(decoded) - 1] = 0;
            meld_decode_method1(decoded);
        } else {
            strncpy(decoded, line, sizeof(decoded) - 1);
            decoded[sizeof(decoded) - 1] = 0;
            meld_strip_eol(decoded);
        }

        meld_patch_line(decoded, game_idx, patched, sizeof(patched));

        if (is_encoded) {
            // XOR is self-inverse: applying meld_decode_method1 to plaintext
            // produces the correctly encoded ciphertext for that line length.
            meld_decode_method1(patched);
            mbuf_append(&buf, "@");
            mbuf_append(&buf, patched);
        } else {
            mbuf_append(&buf, patched);
        }
        mbuf_append(&buf, "\n");
    }
    fclose(src);

    if (buf.data == NULL) {
        return NULL;
    }

    {
        FILE* out = meld_tmpfile_from(buf.data, buf.len);
        free(buf.data);
        return out;
    }
}

// ---------------------------------------------------------------------------
// VFS routing
// ---------------------------------------------------------------------------

// Extract the trailing basename from a path (portable, handles both seps).
static const char* meld_basename(const char* path) {
    const char* p = path;
    const char* last = path;
    for (; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
    }
    return last;
}

// Return the portion of `path` after gApplication_path prefix, else the whole
// path. gApplication_path is not visible here, so we heuristically strip up to
// and including a "DATA" / "MUSIC" style leading segment: instead we simply try
// the path as-is first, then reconstruct with each game dir using the tail
// after the last occurrence of a known data root, falling back to basename.
static void meld_relative_tail(const char* path, char* out, size_t len) {
    // Find "DATA" or "MUSIC" (case-insensitive) segment and keep from there.
    const char* candidates[] = { "DATA", "data", "MUSIC", "music" };
    size_t i;
    const char* best = NULL;
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        const char* found = strstr(path, candidates[i]);
        if (found != NULL) {
            // must be at a path boundary
            if (found == path || found[-1] == '/' || found[-1] == '\\') {
                if (best == NULL || found > best) {
                    best = found;
                }
            }
        }
    }
    if (best != NULL) {
        strncpy(out, best, len - 1);
        out[len - 1] = 0;
    } else {
        strncpy(out, meld_basename(path), len - 1);
        out[len - 1] = 0;
    }
}

FILE* Meld_fopen(const char* path, const char* mode) {
    const char* base = meld_basename(path);
    int writing = (strchr(mode, 'w') != NULL || strchr(mode, 'a') != NULL || strchr(mode, '+') != NULL);

    // Intercept the merged campaign files (read-only).
    if (!writing) {
        if (strcasecmp(base, "RACES.TXT") == 0 && s_races_buf != NULL) {
            return Meld_OpenRaceFile();
        }
        if (strcasecmp(base, "OPPONENT.TXT") == 0 && s_oppo_buf != NULL) {
            return Meld_OpenOpponentFile();
        }
        if (strcasecmp(base, "PARTSHOP.TXT") == 0 && s_partshop_buf != NULL && s_partshop_len > 0) {
            return Meld_OpenPartshopFile();
        }
    }

    // Phase 3.3: synthetic "N:basename" asset paths written by Phase 6 patching.
    // Detect pattern: basename starts with a single digit + ':'.
    if (!writing && s_conflict_count > 0 &&
        isdigit((unsigned char)base[0]) && base[1] == ':') {
        int n_game = base[0] - '0';
        if (n_game >= 0 && n_game < harness_game_config.game_dirs_count) {
            const char* real_base = base + 2;
            // Reconstruct real path: same directory part, with the "N:" prefix stripped.
            size_t dir_len = (size_t)(base - path);
            char real_path[MAX_PATH];
            if (dir_len < sizeof(real_path) - 1) {
                memcpy(real_path, path, dir_len);
                real_path[dir_len] = 0;
                strncat(real_path, real_base, sizeof(real_path) - dir_len - 1);
            } else {
                strncpy(real_path, real_base, sizeof(real_path) - 1);
                real_path[sizeof(real_path) - 1] = 0;
            }
            char tail[MAX_PATH];
            meld_relative_tail(real_path, tail, sizeof(tail));
            char candidate[MAX_PATH];
            meld_join(candidate, sizeof(candidate), harness_game_config.game_dirs[n_game].directory, tail);
            {
                FILE* f = OS_fopen(candidate, mode);
                if (f != NULL) {
                    return f;
                }
            }
            return NULL;
        }
    }

    // 1. Try the path as-is.
    {
        FILE* f = OS_fopen(path, mode);
        if (f != NULL) {
            return f;
        }
    }

    // Writes go to the primary game dir tail only (fall through to as-is above
    // already attempted); do not fan out writes across game dirs.
    if (writing) {
        return NULL;
    }

    // 2. Rebuild with each game dir: active game first, then all in order.
    {
        char tail[MAX_PATH];
        char candidate[MAX_PATH];
        int order[MELD_MAX_GAMES];
        int n = 0;
        int g;

        meld_relative_tail(path, tail, sizeof(tail));

        if (s_active_game >= 0 && s_active_game < harness_game_config.game_dirs_count) {
            order[n++] = s_active_game;
        }
        for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
            if (g == s_active_game) {
                continue;
            }
            order[n++] = g;
        }

        for (g = 0; g < n; g++) {
            FILE* f;
            meld_join(candidate, sizeof(candidate), harness_game_config.game_dirs[order[g]].directory, tail);
            f = OS_fopen(candidate, mode);
            if (f != NULL) {
                // Phase 6: patch TXT files in RACES/, CARS/, NONCARS/ when
                // the conflict map has entries (assets differ across games).
                if (s_conflict_count > 0 && meld_needs_patching(tail)) {
                    fclose(f);
                    return meld_patch_txt_serve(candidate, order[g], s_game_method[order[g]]);
                }
                return f;
            }
        }
    }

    return NULL;
}
