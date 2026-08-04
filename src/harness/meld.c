// Meld feature: merge all games listed under [Games] into one unified campaign
// at runtime. Tracks, opponents, and assets are combined without manual file
// copying. See harness/meld.h for the public API.

#include "harness/meld.h"
#include "harness/config.h"
#include "harness/os.h"
#include "harness/trace.h"

extern void EncodeLine(char* pS);
extern int gEncryption_method;
extern char* GetALineWithNoPossibleService(FILE* pF, unsigned char* pS);

#include <ctype.h>
#if defined(_MSC_VER) && _MSC_VER <= 1020
typedef unsigned long uint64_t;
#define MELD_FNV_OFFSET 2166136261UL
#define MELD_FNV_PRIME  16777619UL
#else
#include <stdint.h>
#define MELD_FNV_OFFSET 1469598103934665603ULL
#define MELD_FNV_PRIME  1099511628211ULL
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define MELD_SEP "\\"
#define MELD_SEP_CH '\\'

static FILE* meld_fmemopen(void* buf, size_t size, const char* mode) {
    char* name = _tempnam(NULL, "mld");
    FILE* f;
    (void)mode;
    if (name == NULL) {
        return NULL;
    }
    f = fopen(name, "w+b");
    free(name);
    if (f == NULL) {
        return NULL;
    }
    fwrite(buf, 1, size, f);
    rewind(f);
    return f;
}
#define fmemopen meld_fmemopen
#include <direct.h>
#define getcwd _getcwd
#else
#define MELD_SEP "/"
#define MELD_SEP_CH '/'
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

int gMeld_active = 0;
int gMeld_both_starting_cars = 0;
int gMeld_total_race_count = 0;
int gMeld_primary_race_count = 0;
int gMeld_net_races_active = 0;
int gMeld_use_net_starts = 0;

#define MELD_MAX_GAMES 10
#define MELD_MAX_RACES 200
#define MELD_MAX_RACE_LINES 80
// Deduped opponent pool size. Melding several installs — plus each racer's
// per-livery entries (Max Damage alone has ~6 eagle colours per game) — pushes
// the pool well past a single game's roster, so this must be generous: any
// opponent beyond it would lose its character id (per-race dedup) and asset
// routing, breaking mugshots for the overflow racers.
#define MELD_MAX_OPPONENTS 600
// Raw opponents read across all games before dedup (>= the deduped pool).
#define MELD_MAX_RAW_OPPONENTS 600
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
// Race slot -> 1 if this is a net-only arena track (no SP path sections).
static int s_race_is_arena[MELD_MAX_RACES];
// Race slot -> map pixelmap basename (for arena tracks), empty string otherwise.
#define MELD_MAP_PIX_LEN 16
static char s_race_map_pix[MELD_MAX_RACES][MELD_MAP_PIX_LEN];
// Race slot -> 1 if a custom scene FLI (DATA/ANIM/{track}.FLI) was found for this arena race.
static int s_race_has_scene_fli[MELD_MAX_RACES];
// Current race index (updated by Meld_SetActiveGame).
static int s_current_race_index = -1;
// Opponent slot -> source game index.
static int s_opponent_source_game[MELD_MAX_OPPONENTS];
// Opponent slot -> car_file name (first token of car file line).
static char s_slot_car_file[MELD_MAX_OPPONENTS][256];
// Opponent slot -> character name (used to group car variants of one racer).
static char s_slot_name[MELD_MAX_OPPONENTS][32];
// Opponent slot -> character id: variants of the same racer (same name but a
// different car per game) share an id, so a race can pick a character only
// once. Cops (car 500/501) are exempt and get -1.
static int s_opponent_char_id[MELD_MAX_OPPONENTS];

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

#define MELD_MAX_CONFLICTS 2048
#define MELD_CONFLICT_NAMELEN 64

static char s_conflict_basenames[MELD_MAX_CONFLICTS][MELD_CONFLICT_NAMELEN];
static int s_conflict_count = 0;

// ---------------------------------------------------------------------------
// XOR decode (method 1 only; method 2 used only by CARDEMO which has no unique
// content, so those games are skipped)
// ---------------------------------------------------------------------------

// Strip trailing CR/LF and non-ASCII garbage bytes in place. Some mod files
// append high-byte chars after encoded content; the XOR decoder reproduces
// them faithfully, so strip them here before the decoded string is used.
static void meld_strip_eol(char* buf) {
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || (unsigned char)buf[len - 1] >= 0x80)) {
        buf[--len] = 0;
    }
}

// Read one meaningful data line from f (decoded if needed, skipping
// comments/blanks). Returns 0 on EOF, 1 on success.
static int meld_readline_m(FILE* f, int method, char* buf) {
    gEncryption_method = method;
    return GetALineWithNoPossibleService(f, (unsigned char*)buf) != NULL;
}

// ---------------------------------------------------------------------------
// Hashing (FNV-1a of file contents; not cryptographic)
// ---------------------------------------------------------------------------

static uint64_t meld_fnv1a(const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    uint64_t h = MELD_FNV_OFFSET;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= MELD_FNV_PRIME;
    }
    return h;
}

// Hash a file's full contents. Returns 0 if the file could not be opened.
static uint64_t meld_hash_file(const char* path) {
    FILE* f = OS_fopen(path, "rb");
    uint64_t h = MELD_FNV_OFFSET;
    unsigned char buf[4096];
    size_t n;
    if (f == NULL) {
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t i;
        for (i = 0; i < n; i++) {
            h ^= buf[i];
            h *= MELD_FNV_PRIME;
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

// Scan one subdir across all game dirs; any file whose content differs across
// two games is added to the conflict set.

static int meld_file_exists(const char* path) {
    FILE* f = OS_fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return 1;
    }
    return 0;
}

// Compare two files by size then content using buffered reads.
// Binary files are compared exactly; text files (.txt) strip \r so CRLF==LF.
// Files larger than 4 MB are compared by size only.
#define MELD_MAX_COMPARE_BYTES (4 * 1024 * 1024)
#define MELD_CMP_CHUNK 65536
static int meld_files_same(const char* path_a, const char* path_b) {
    FILE* fa;
    FILE* fb;
    long size_a, size_b;
    int same;
    int is_text;
    const char* ext;
    static unsigned char buf_a[MELD_CMP_CHUNK];
    static unsigned char buf_b[MELD_CMP_CHUNK];

    fa = OS_fopen(path_a, "rb");
    fb = OS_fopen(path_b, "rb");
    if (fa == NULL || fb == NULL) {
        if (fa) { fclose(fa); }
        if (fb) { fclose(fb); }
        return 0;
    }

    fseek(fa, 0, SEEK_END); size_a = ftell(fa); rewind(fa);
    fseek(fb, 0, SEEK_END); size_b = ftell(fb); rewind(fb);

    if (size_a > MELD_MAX_COMPARE_BYTES || size_b > MELD_MAX_COMPARE_BYTES) {
        fclose(fa); fclose(fb);
        return size_a == size_b;
    }

    /* Binary files: sizes must match for them to be the same. */
    ext = strrchr(path_a, '.');
    is_text = ext && strcasecmp(ext, ".txt") == 0;
    if (!is_text && size_a != size_b) {
        fclose(fa); fclose(fb);
        return 0;
    }

    same = 1;
    if (is_text) {
        /* Text: strip \r so CRLF and LF files compare equal. */
        int ca, cb;
        do {
            do { ca = fgetc(fa); } while (ca == '\r');
            do { cb = fgetc(fb); } while (cb == '\r');
            if (ca != cb) { same = 0; break; }
        } while (ca != EOF);
    } else {
        /* Binary: bulk fread comparison. */
        size_t ra, rb;
        do {
            ra = fread(buf_a, 1, MELD_CMP_CHUNK, fa);
            rb = fread(buf_b, 1, MELD_CMP_CHUNK, fb);
            if (ra != rb || memcmp(buf_a, buf_b, ra) != 0) {
                same = 0;
                break;
            }
        } while (ra == MELD_CMP_CHUNK);
    }
    fclose(fa); fclose(fb);
    return same;
}

static void meld_scan_subdir_conflicts(const char* subdir) {
    char dir_a[MAX_PATH];
    char dir_b[MAX_PATH];
    char file_a[MAX_PATH];
    char file_b[MAX_PATH];
    const char* fname;
    int g, other;
    int gc = harness_game_config.game_dirs_count;

    if (gc < 2) {
        return;
    }

    LOG_INFO2("Meld conflict scan: %s", subdir);

    for (g = 0; g < gc && g < MELD_MAX_GAMES; g++) {
        meld_join(dir_a, sizeof(dir_a), harness_game_config.game_dirs[g].directory, subdir);
        fname = OS_GetFirstFileInDirectory(dir_a);
        while (fname != NULL) {
            if (fname[0] != '.' && !meld_is_conflict(fname)) {
                meld_join(file_a, sizeof(file_a), dir_a, fname);
                for (other = 0; other < gc && other < MELD_MAX_GAMES; other++) {
                    if (other == g) {
                        continue;
                    }
                    meld_join(dir_b, sizeof(dir_b), harness_game_config.game_dirs[other].directory, subdir);
                    meld_join(file_b, sizeof(file_b), dir_b, fname);
                    // Only a conflict if the file actually exists in the other
                    // game AND the content genuinely differs beyond line endings.
                    if (meld_file_exists(file_b) && !meld_files_same(file_a, file_b)) {
                        meld_add_conflict(fname);
                        break;
                    }
                }
            }
            fname = OS_GetNextFileInDirectory();
        }
    }
}

static void meld_build_conflict_map(void) {
    static const char* asset_subdirs[] = {
        "DATA", "DATA/CARS", "DATA/MODELS", "DATA/PIXELMAP",
        "DATA/MATERIAL", "DATA/ACTORS", "DATA/ANIM", NULL
    };
    int i;

    LOG_INFO("Meld: building asset conflict map");

    for (i = 0; asset_subdirs[i] != NULL; i++) {
        meld_scan_subdir_conflicts(asset_subdirs[i]);
    }

    LOG_INFO2("Meld: conflict map done, %d conflicting asset basenames", s_conflict_count);
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

static void meld_join(char* dest, size_t len, const char* a, const char* b) {
    size_t a_len = strlen(a);
    if (a_len + 1 >= len) {
        strncpy(dest, a, len - 1);
        dest[len - 1] = '\0';
        return;
    }
    memcpy(dest, a, a_len);
    dest[a_len] = MELD_SEP_CH;
    dest[a_len + 1] = '\0';
    strncat(dest, b, len - a_len - 2);
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
        return 2;
    }
    memmove(buf, buf + 1, strlen(buf));
    gEncryption_method = 1;
    EncodeLine(buf);
    buf[6] = '\0';
    if (strcmp(buf, "0.01\t\t") == 0) {
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
        char* nd;
        size_t newcap = b->cap * 2;
        while (b->len + sl + 1 > newcap) {
            newcap *= 2;
        }
        nd = (char*)realloc(b->data, newcap);
        if (nd == NULL) {
            return;
        }
        b->data = nd;
        b->cap = newcap;
    }
    memcpy(b->data + b->len, s, sl + 1);
    b->len += sl;
}

// Encode a plain-text line with method-1 and append "@<encoded>\n" to buf.
static void mbuf_append_m1(tMeld_buf* b, const char* line) {
    char encoded[MELD_LINE_LEN];
    strncpy(encoded, line, sizeof(encoded) - 1);
    encoded[sizeof(encoded) - 1] = 0;
    gEncryption_method = 1;
    EncodeLine(encoded);
    mbuf_append(b, "@");
    mbuf_append(b, encoded);
    mbuf_append(b, "\n");
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
static int s_net_map_base_count = 0;

// Read one full race record (starting at its name line) into r. Returns:
//   1 = read a race, 0 = hit "END", -1 = EOF/error.
static int meld_read_one_race(FILE* f, int method, tMeld_race* r) {
    char s[MELD_LINE_LEN];
    int chunk_count;
    int j;

    r->num_lines = 0;
    if (!meld_readline_m(f, method, s)) {
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
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    if (r->num_lines < MELD_MAX_RACE_LINES) {
        strcpy(r->lines[r->num_lines++], s);
    }
    // TRACK.TXT line
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    if (r->num_lines < MELD_MAX_RACE_LINES) {
        strcpy(r->lines[r->num_lines++], s);
    }
    // chunk count
    if (!meld_readline_m(f, method, s)) {
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
        if (!meld_readline_m(f, method, s)) {
            return -1;
        }
        if (r->num_lines < MELD_MAX_RACE_LINES) {
            strcpy(r->lines[r->num_lines++], s);
        }
        // frame/frame_end line
        if (!meld_readline_m(f, method, s)) {
            return -1;
        }
        if (r->num_lines < MELD_MAX_RACE_LINES) {
            strcpy(r->lines[r->num_lines++], s);
        }
        // line count
        if (!meld_readline_m(f, method, s)) {
            return -1;
        }
        line_count = atoi(s);
        if (r->num_lines < MELD_MAX_RACE_LINES) {
            snprintf(r->lines[r->num_lines++], MELD_LINE_LEN, "%d", line_count);
        }
        for (k = 0; k < line_count; k++) {
            if (!meld_readline_m(f, method, s)) {
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
            // num_lines==4 means chunk_count==0: demo placeholder entry with no
            // race description text. These are teasers for locked content and
            // should not appear in the merged campaign.
            if (s_races[s_race_count].num_lines <= 4) {
                continue;
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
                s_races[start + idx].norm = 0.5f;
            } else {
                s_races[start + idx].norm = (float)idx / (float)(count - 1);
            }
        }
    }

    gMeld_total_race_count = s_race_count;

    // Stable sort by norm then game order.
    qsort(s_races, s_race_count, sizeof(tMeld_race), meld_race_cmp);

    // Record per-slot source game and emit method-1 encoded merged file.
    mbuf_init(&buf);
    for (i = 0; i < s_race_count; i++) {
        int l;
        s_race_source_game[i] = s_races[i].game_idx;
        for (l = 0; l < s_races[i].num_lines; l++) {
            mbuf_append_m1(&buf, s_races[i].lines[l]);
        }
    }
    mbuf_append_m1(&buf, "END");

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
    // Set when the mugshot flic equals the stolen-car flic (e.g. both
    // EAGLERED.FLI): the demos strip an opponent's graphics to a shared
    // placeholder, so this record's art is not really its own.
    int is_placeholder;
} tMeld_opponent;

static tMeld_opponent s_oppos[MELD_MAX_RAW_OPPONENTS];

// Pre-computed extra starting-car output slots for each frank_or_anniness (0=Max, 1=Anna).
// Built in meld_build_opponents when MeldBothStartingCars=1.
#define MELD_MAX_EXTRA_START 8
static int s_extra_start_slots[2][MELD_MAX_EXTRA_START];
static int s_extra_start_counts[2];
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
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    if (strcmp(s, "END") == 0) {
        return 0;
    }
    strcpy(o->name, s);
    strcpy(o->lines[o->num_lines++], s);
    // abbrev
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // car number
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    o->car_number = atoi(s);
    strcpy(o->lines[o->num_lines++], s);
    // strength
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // net avail
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // mug shot
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // car file name
    if (!meld_readline_m(f, method, s)) {
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
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    strcpy(o->lines[o->num_lines++], s);
    // chunk count
    if (!meld_readline_m(f, method, s)) {
        return -1;
    }
    chunk_count = atoi(s);
    snprintf(o->lines[o->num_lines++], MELD_LINE_LEN, "%d", chunk_count);
    for (j = 0; j < chunk_count; j++) {
        int line_count;
        int k;
        if (!meld_readline_m(f, method, s)) {
            return -1;
        }
        if (o->num_lines < MELD_MAX_OPPO_LINES) {
            strcpy(o->lines[o->num_lines++], s);
        }
        if (!meld_readline_m(f, method, s)) {
            return -1;
        }
        if (o->num_lines < MELD_MAX_OPPO_LINES) {
            strcpy(o->lines[o->num_lines++], s);
        }
        if (!meld_readline_m(f, method, s)) {
            return -1;
        }
        line_count = atoi(s);
        if (o->num_lines < MELD_MAX_OPPO_LINES) {
            snprintf(o->lines[o->num_lines++], MELD_LINE_LEN, "%d", line_count);
        }
        for (k = 0; k < line_count; k++) {
            if (!meld_readline_m(f, method, s)) {
                return -1;
            }
            if (o->num_lines < MELD_MAX_OPPO_LINES) {
                strcpy(o->lines[o->num_lines++], s);
            }
        }
    }
    o->game_idx = game_idx;
    // Hash the car TXT file for dedup. Fall back through other game dirs if
    // the file isn't present locally (e.g. XMASDEMO shares CARSPLAT's CARS/).
    {
        char carpath[MAX_PATH];
        char cars[MAX_PATH];
        int g2;
        o->car_hash = 0;
        for (g2 = 0; g2 < harness_game_config.game_dirs_count && g2 < MELD_MAX_GAMES; g2++) {
            meld_join(cars, sizeof(cars), harness_game_config.game_dirs[g2].directory, "DATA" MELD_SEP "CARS");
            meld_join(carpath, sizeof(carpath), cars, o->car_file);
            o->car_hash = meld_hash_file(carpath);
            if (o->car_hash != 0) {
                break;
            }
        }
    }
    // A racer's mugshot flic (line 5) and stolen-car flic (line 7) are normally
    // distinct (a portrait vs the car). The demos replace both with one shared
    // placeholder (e.g. EAGLERED.FLI), so equal flics mark a degraded record.
    o->is_placeholder = 0;
    if (o->num_lines > 7) {
        char mug[MELD_LINE_LEN];
        char stolen[MELD_LINE_LEN];
        char* mug_tok;
        char* stolen_tok;
        strcpy(mug, o->lines[5]);
        strcpy(stolen, o->lines[7]);
        mug_tok = strtok(mug, "\t ,/");
        stolen_tok = strtok(stolen, "\t ,/");
        if (mug_tok != NULL && stolen_tok != NULL && strcasecmp(mug_tok, stolen_tok) == 0) {
            o->is_placeholder = 1;
        }
    }
    return 1;
}

// Read frank's and anna's starting car filenames from a game's GENERAL.TXT.
// LoadGeneralParameters reads 34 data lines before gBasic_car_names[0/1]:
// 4 scalars + 2 ints + 2x3-ints + 6 floats + 3x3-floats + 14-line branch
// (7 data + 7 garbage, order varies by sausage_eater_mode but total is same)
// + 3x3-ints = 34 lines total.
static int meld_read_general_car_names(int game_idx, char* frank_out, char* anna_out) {
    FILE* f;
    char s[MELD_LINE_LEN];
    int method = s_game_method[game_idx];
    int saved_enc = gEncryption_method;
    int i;
    char tmp[MELD_LINE_LEN];
    char* tok;

    f = meld_open_data(game_idx, "GENERAL.TXT", "rb");
    if (f == NULL) {
        return 0;
    }
    for (i = 0; i < 34; i++) {
        if (!meld_readline_m(f, method, s)) {
            fclose(f);
            gEncryption_method = saved_enc;
            return 0;
        }
    }
    if (!meld_readline_m(f, method, s)) {
        fclose(f);
        gEncryption_method = saved_enc;
        return 0;
    }
    strcpy(tmp, s);
    tok = strtok(tmp, "\t ,/");
    strncpy(frank_out, tok ? tok : "", 255);
    frank_out[255] = '\0';

    if (!meld_readline_m(f, method, s)) {
        fclose(f);
        gEncryption_method = saved_enc;
        return 0;
    }
    strcpy(tmp, s);
    tok = strtok(tmp, "\t ,/");
    strncpy(anna_out, tok ? tok : "", 255);
    anna_out[255] = '\0';

    fclose(f);
    gEncryption_method = saved_enc;
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

        f = meld_open_data(g, "OPPONENT.TXT", "rb");
        if (f == NULL) {
            continue;
        }
        // count header
        if (!meld_readline_m(f, method, header)) {
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
            // num_lines==9 means chunk_count==0: demo placeholder with no text.
            if (s_oppos[s_oppo_raw_count].num_lines <= 9) {
                continue;
            }
            s_oppo_raw_count++;
        }
        fclose(f);
    }

    // Dedup: include one entry per (name, car_hash). For player cars
    // (car_number < 0) and cops (500/501) dedup by car_hash only.
    // Mark included entries.
    {
        static int included[MELD_MAX_RAW_OPPONENTS];
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
            // MeldBothStartingCars: never dedup player-car entries across
            // different games — keep each game's starting car distinct.
            // Only dedup within the same game (same game_idx + same hash).
            if (dup && gMeld_both_starting_cars && s_oppos[i].car_number < 0) {
                int true_dup = 0;
                for (j = 0; j < i; j++) {
                    if (included[j] && s_oppos[j].car_number < 0 &&
                        s_oppos[j].game_idx == s_oppos[i].game_idx &&
                        s_oppos[j].car_hash == s_oppos[i].car_hash) {
                        true_dup = 1;
                        break;
                    }
                }
                dup = true_dup;
            }
            // Drop a degraded demo record: its graphics are a shared
            // placeholder (mugshot flic == stolen-car flic, e.g. both
            // EAGLERED.FLI) AND its car is a generic model reused by other
            // racers, AND a proper record for the same racer exists elsewhere.
            // Requiring a generic model keeps genuine mod cars (a unique model
            // that merely lacks its own flics) and demo-only racers.
            if (!dup && s_oppos[i].is_placeholder) {
                int has_real = 0;
                int generic_model = 0;
                for (j = 0; j < s_oppo_raw_count; j++) {
                    if (j == i) {
                        continue;
                    }
                    if (!s_oppos[j].is_placeholder && strcmp(s_oppos[j].name, s_oppos[i].name) == 0) {
                        has_real = 1;
                    }
                    if (s_oppos[i].car_hash != 0 && s_oppos[j].car_hash == s_oppos[i].car_hash && strcmp(s_oppos[j].name, s_oppos[i].name) != 0) {
                        generic_model = 1;
                    }
                }
                if (has_real && generic_model) {
                    dup = 1;
                }
            }
            included[i] = !dup;
            if (included[i]) {
                out_count++;
            }
        }

        mbuf_init(&buf);
        snprintf(numbuf, sizeof(numbuf), "%d", out_count);
        mbuf_append_m1(&buf, numbuf);

        {
            int slot = 0;
            s_extra_start_counts[0] = s_extra_start_counts[1] = 0;

            for (i = 0; i < s_oppo_raw_count; i++) {
                int l;
                if (!included[i]) {
                    continue;
                }
                if (slot < MELD_MAX_OPPONENTS) {
                    s_opponent_source_game[slot] = s_oppos[i].game_idx;
                    strncpy(s_slot_car_file[slot], s_oppos[i].car_file, 255);
                    s_slot_car_file[slot][255] = '\0';
                    strncpy(s_slot_name[slot], s_oppos[i].name, sizeof(s_slot_name[slot]) - 1);
                    s_slot_name[slot][sizeof(s_slot_name[slot]) - 1] = '\0';
                    // Assign the character id: reuse an earlier slot's id when
                    // the name matches (a car variant of the same racer),
                    // otherwise start a new group. Cops may repeat, so exempt.
                    if (s_oppos[i].car_number == 500 || s_oppos[i].car_number == 501) {
                        s_opponent_char_id[slot] = -1;
                    } else {
                        int p;
                        s_opponent_char_id[slot] = slot;
                        for (p = 0; p < slot; p++) {
                            if (s_opponent_char_id[p] >= 0 && strcmp(s_slot_name[p], s_slot_name[slot]) == 0) {
                                s_opponent_char_id[slot] = s_opponent_char_id[p];
                                break;
                            }
                        }
                    }
                }
                slot++;
                for (l = 0; l < s_oppos[i].num_lines; l++) {
                    mbuf_append_m1(&buf, s_oppos[i].lines[l]);
                }
            }

            // For each secondary game, read its GENERAL.TXT car names and find
            // the corresponding slots in the merged output.
            if (gMeld_both_starting_cars) {
                int g;
                for (g = 1; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
                    char frank_car[256], anna_car[256];
                    int s;
                    if (!meld_read_general_car_names(g, frank_car, anna_car)) {
                        continue;
                    }
                    for (s = 0; s < slot && s < MELD_MAX_OPPONENTS; s++) {
                        if (s_opponent_source_game[s] != g) {
                            continue;
                        }
                        if (frank_car[0] && strcasecmp(s_slot_car_file[s], frank_car) == 0) {
                            int already = 0, k;
                            for (k = 0; k < s_extra_start_counts[0]; k++) {
                                if (strcasecmp(s_slot_car_file[s_extra_start_slots[0][k]], frank_car) == 0) {
                                    already = 1;
                                    break;
                                }
                            }
                            if (!already && s_extra_start_counts[0] < MELD_MAX_EXTRA_START) {
                                s_extra_start_slots[0][s_extra_start_counts[0]++] = s;
                            }
                        }
                        if (anna_car[0] && strcasecmp(s_slot_car_file[s], anna_car) == 0) {
                            int already = 0, k;
                            for (k = 0; k < s_extra_start_counts[1]; k++) {
                                if (strcasecmp(s_slot_car_file[s_extra_start_slots[1][k]], anna_car) == 0) {
                                    already = 1;
                                    break;
                                }
                            }
                            if (!already && s_extra_start_counts[1] < MELD_MAX_EXTRA_START) {
                                s_extra_start_slots[1][s_extra_start_counts[1]++] = s;
                            }
                        }
                    }
                }
            }
        }
        mbuf_append_m1(&buf, "END");
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

        if (!s_game_contributed[g]) {
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
            if (!meld_readline_m(f, method, s)) {
                break;
            }
            part_count = atoi(s);
            for (p = 0; p < part_count; p++) {
                char* tok;
                int rank;
                if (!meld_readline_m(f, method, s)) {
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
        snprintf(line, sizeof(line), "%d", MELD_PS_PARTS);
        mbuf_append_m1(&buf, line);
        for (p = 0; p < MELD_PS_PARTS; p++) {
            snprintf(line, sizeof(line), "%d\t%s\t%d\t%d\t%d",
                parts[cat][p].rank_required,
                parts[cat][p].part_name[0] ? parts[cat][p].part_name : "NONE",
                parts[cat][p].prices[0],
                parts[cat][p].prices[1],
                parts[cat][p].prices[2]);
            mbuf_append_m1(&buf, line);
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
// Overlay dir
// ---------------------------------------------------------------------------

void Meld_CaptureExeDir(const char* argv0) {
    char abs_path[MAX_PATH];
    char* sep;
    char* sep2;
    int is_abs;

    if (argv0 == NULL) {
        return;
    }
    is_abs = (argv0[0] == '/') || (argv0[0] != '\0' && argv0[1] == ':');
    if (is_abs) {
        strncpy(abs_path, argv0, sizeof(abs_path) - 1);
        abs_path[sizeof(abs_path) - 1] = '\0';
    } else {
        char cwd[MAX_PATH];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            return;
        }
        snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd, argv0);
    }
    sep = strrchr(abs_path, '/');
    sep2 = strrchr(abs_path, '\\');
    if (sep2 > sep) {
        sep = sep2;
    }
    if (sep != NULL) {
        *sep = '\0';
        strncpy(harness_game_config.meld_overlay_dir, abs_path,
                sizeof(harness_game_config.meld_overlay_dir) - 1);
        harness_game_config.meld_overlay_dir[sizeof(harness_game_config.meld_overlay_dir) - 1] = '\0';
        LOG_INFO2("Meld: overlay dir: %s", harness_game_config.meld_overlay_dir);
    }
}

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

    LOG_INFO2("Meld_Init: starting with %d game dirs", harness_game_config.game_dirs_count);

    gMeld_both_starting_cars = harness_game_config.meld_both_starting_cars;

    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        s_game_method[g] = meld_detect_method(g);
        s_game_contributed[g] = 0;
        LOG_INFO3("Meld_Init: game[%d] dir=%s", g, harness_game_config.game_dirs[g].directory);
        LOG_INFO3("Meld_Init: game[%d] method=%d", g, s_game_method[g]);
    }

    meld_build_races();
    LOG_INFO2("Meld_Init: races built, count=%d", s_race_count);
    if (s_race_count == 0) {
        // Nothing merged; do not activate.
        LOG_INFO("Meld_Init: no races found, not activating");
        return;
    }
    meld_build_opponents();
    meld_build_partshop();
    meld_build_music();

    meld_build_conflict_map();

    s_active_game = 0;
    gMeld_active = 1;

    // Reset so LoadGeneralParameters re-detects the correct method from the
    // primary game's GENERAL.TXT (meld_readline_m leaves it set to the last
    // secondary game's method).
    gEncryption_method = 0;

    LOG_INFO2("Meld active: %d races merged from games", gMeld_total_race_count);
}

// ---------------------------------------------------------------------------
// Merged file accessors
// ---------------------------------------------------------------------------

FILE* Meld_OpenRaceFile(void) {
    return fmemopen(s_races_buf, s_races_len, "rb");
}

FILE* Meld_OpenOpponentFile(void) {
    return fmemopen(s_oppo_buf, s_oppo_len, "rb");
}

FILE* Meld_OpenPartshopFile(void) {
    return fmemopen(s_partshop_buf, s_partshop_len, "rb");
}

// ---------------------------------------------------------------------------
// Active-game selection
// ---------------------------------------------------------------------------

void Meld_SetActiveGame(int race_index) {
    if (race_index >= 0 && race_index < s_race_count) {
        s_active_game = s_race_source_game[race_index];
    }
    gMeld_use_net_starts = gMeld_net_races_active && race_index >= 0 && race_index < s_race_count && s_race_is_arena[race_index];
    s_current_race_index = race_index;
}

const char* Meld_ArenaMapPixName(void) {
    if (s_current_race_index >= 0 && s_current_race_index < s_race_count && s_race_is_arena[s_current_race_index]) {
        return s_race_map_pix[s_current_race_index];
    }
    return NULL;
}

int Meld_ArenaHasSceneFli(void) {
    if (s_current_race_index >= 0 && s_current_race_index < s_race_count) {
        return s_race_has_scene_fli[s_current_race_index];
    }
    return 0;
}

void Meld_DrawArenaMapPanel(br_pixelmap* back_screen, br_pixelmap* map_image,
    br_pixelmap* src_palette, br_pixelmap* dst_palette,
    int pl, int pt, int pr, int pb) {
    int pw = pr - pl;
    int ph = pb - pt;
    int mw, mh, dx, dy, sx, sy, cw, ch;
    if (map_image == NULL) {
        return;
    }
    mw = map_image->width;
    mh = map_image->height;
    dx = pl + (pw - mw) / 2;
    dy = pt + (ph - mh) / 2;
    sx = dx < pl ? pl - dx : 0;
    sy = dy < pt ? pt - dy : 0;
    cw = mw - sx * 2;
    ch = mh - sy * 2;
    if (dx < pl) { dx = pl; }
    if (dy < pt) { dy = pt; }
    if (cw > pw) { cw = pw; }
    if (ch > ph) { ch = ph; }
    if (cw <= 0 || ch <= 0) {
        return;
    }
    if (src_palette != NULL && dst_palette != NULL && src_palette != dst_palette) {
        /* Remap pixels from src_palette (DRRENDER) colour space into dst_palette (FLI) space */
        unsigned char remap[256];
        const unsigned char* sp = (const unsigned char*)src_palette->pixels;
        const unsigned char* dp = (const unsigned char*)dst_palette->pixels;
        const unsigned char* src_row;
        unsigned char* dst_row;
        int i, j, row, col;
        for (i = 0; i < 256; i++) {
            int sr = sp[i * 4], sg = sp[i * 4 + 1], sb = sp[i * 4 + 2];
            long best = 0x7fffffff;
            int best_j = 0;
            for (j = 0; j < 256; j++) {
                int dr = sr - dp[j * 4], dg = sg - dp[j * 4 + 1], db = sb - dp[j * 4 + 2];
                long dist = dr * dr + dg * dg + db * db;
                if (dist < best) { best = dist; best_j = j; if (dist == 0) break; }
            }
            remap[i] = (unsigned char)best_j;
        }
        src_row = (const unsigned char*)map_image->pixels + sy * map_image->row_bytes + sx;
        dst_row = (unsigned char*)back_screen->pixels + dy * back_screen->row_bytes + dx;
        for (row = 0; row < ch; row++) {
            for (col = 0; col < cw; col++) {
                dst_row[col] = remap[src_row[col]];
            }
            src_row += map_image->row_bytes;
            dst_row += back_screen->row_bytes;
        }
    } else {
        BrPixelmapRectangleCopy(back_screen, dx, dy, map_image, sx, sy, cw, ch);
    }
}

void Meld_SetActiveGame_Opponent(int opponent_index) {
    if (opponent_index >= 0 && opponent_index < MELD_MAX_OPPONENTS) {
        s_active_game = s_opponent_source_game[opponent_index];
    }
}

int Meld_IsOpponentEligible(int opponent_index) {
    // Every melded opponent is eligible for every race, regardless of which
    // game it came from, so races mix Carmageddon and Splat Pack racers
    // freely. Rank/difficulty is still enforced by the base game's strength
    // bands (ChooseOpponent), and each opponent's assets are loaded from its
    // own game dir via Meld_SetActiveGame_Opponent.
    (void)opponent_index;
    return 1;
}

int Meld_OpponentCharacterId(int opponent_index) {
    // Returns a character group id shared by every car variant of one racer,
    // for per-race dedup. Returns -1 when not melding, out of range, or for
    // racers that may legitimately repeat (cops).
    if (!gMeld_active || opponent_index < 0 || opponent_index >= MELD_MAX_OPPONENTS) {
        return -1;
    }
    return s_opponent_char_id[opponent_index];
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
        const char* tok_start;
        int tok_len;
        char tok[MELD_CONFLICT_NAMELEN];
        char* dot;
        int copy_len;

        // Pass through separators unchanged.
        if (*p == ' ' || *p == '\t' || *p == ',' || *p == '/' ||
            *p == '\r' || *p == '\n') {
            *q++ = *p++;
            continue;
        }

        // Collect a token (run of non-separator chars).
        tok_start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' &&
               *p != '/' && *p != '\r' && *p != '\n') {
            p++;
        }
        tok_len = (int)(p - tok_start);

        if (tok_len >= 5 && tok_len < MELD_CONFLICT_NAMELEN) {
            memcpy(tok, tok_start, tok_len);
            tok[tok_len] = 0;

            // Must have a .ext of 2-4 chars.
            dot = strrchr(tok, '.');
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
        copy_len = tok_len;
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

    // Patch all TXT files under DATA/ — covers both asset subdirs (RACES/, CARS/,
    // NONCARS/) and DATA-root files like PEDESTRN.TXT that need method transcode.
    if (strstr(tail, "DATA/") || strstr(tail, "DATA\\") ||
        strstr(tail, "data/") || strstr(tail, "data\\")) {
        return 1;
    }
    return 0;
}

// True if the decoded line content is the file's own identity header (the
// filename repeated as first data line, used by LoadCar for corruption checks).
// Resolution-specific car files (e.g. 64X48X8/CARS/) start directly with
// data and must NOT be treated as identity lines.
static int meld_is_identity_line(const char* decoded, const char* basename) {
    int blen = (int)strlen(basename);
    if (strncasecmp(decoded, basename, blen) != 0) {
        return 0;
    }
    return decoded[blen] == '\0' || decoded[blen] == '\t' || decoded[blen] == ' ' ||
           decoded[blen] == '\r' || decoded[blen] == '\n' || decoded[blen] == '/';
}

// Read raw_path, decode @-prefixed lines (method 1), patch conflicting asset
// basenames to "N:basename", re-encode, and serve the result as a tmpfile.
static FILE* meld_patch_txt_serve(const char* raw_path, int game_idx, int method) {
    FILE* src;
    tMeld_buf buf;
    char line[1024];
    char decoded[1024];
    char patched[1024];

    LOG_INFO3("Meld: patching TXT %s (game %d)", raw_path, game_idx);

    src = OS_fopen(raw_path, "rt");
    if (src == NULL) {
        LOG_INFO2("Meld: failed to open TXT for patching: %s", raw_path);
        return NULL;
    }

    mbuf_init(&buf);

    {
        int data_line_idx = 0;
        while (fgets(line, sizeof(line), src) != NULL) {
            int is_encoded = (line[0] == '@') && (method >= 1);

            if (is_encoded) {
                strncpy(decoded, &line[1], sizeof(decoded) - 1);
                decoded[sizeof(decoded) - 1] = 0;
                gEncryption_method = method;
                EncodeLine(decoded);
                meld_strip_eol(decoded);
            } else {
                strncpy(decoded, line, sizeof(decoded) - 1);
                decoded[sizeof(decoded) - 1] = 0;
                meld_strip_eol(decoded);
            }

            // The first decoded data line of car/noncar TXT files is the file's own
            // identity (e.g. "SUBFRAME.TXT"). Patching it would corrupt the game's
            // corruption check (strcmp(first_line, expected_name)). Write the
            // canonical filename from the path instead of the decoded content,
            // because some mods encode trailing non-separator bytes after the name
            // that strtok does not strip, causing the strcmp to fail.
            if (is_encoded && data_line_idx == 0 && meld_is_identity_line(decoded, meld_basename(raw_path))) {
                strncpy(patched, meld_basename(raw_path), sizeof(patched) - 1);
                patched[sizeof(patched) - 1] = 0;
            } else {
                meld_patch_line(decoded, game_idx, patched, sizeof(patched));
            }
            if (is_encoded) {
                data_line_idx++;
            }

            if (is_encoded) {
                mbuf_append_m1(&buf, patched);
            } else {
                mbuf_append(&buf, patched);
                mbuf_append(&buf, "\n");
            }
        }
    }
    fclose(src);
    // Output is always method-1 encoded; restore so the caller can decode it.
    gEncryption_method = 1;

    if (buf.data == NULL) {
        return NULL;
    }

    return fmemopen(buf.data, buf.len, "rb");
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
            size_t dir_len = (size_t)(base - path);
            char real_path[MAX_PATH];
            char tail[MAX_PATH];
            char candidate[MAX_PATH];
            // Reconstruct real path: same directory part, with the "N:" prefix stripped.
            if (dir_len < sizeof(real_path) - 1) {
                memcpy(real_path, path, dir_len);
                real_path[dir_len] = 0;
                strncat(real_path, real_base, sizeof(real_path) - dir_len - 1);
            } else {
                strncpy(real_path, real_base, sizeof(real_path) - 1);
                real_path[sizeof(real_path) - 1] = 0;
            }
            meld_relative_tail(real_path, tail, sizeof(tail));
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

    // 1a. Overlay dir: <exe>/DATA takes precedence over all game dirs.
    if (!writing && harness_game_config.meld_overlay_dir[0] != '\0') {
        char tail[MAX_PATH];
        char candidate[MAX_PATH];
        meld_relative_tail(path, tail, sizeof(tail));
        meld_join(candidate, sizeof(candidate), harness_game_config.meld_overlay_dir, tail);
        {
            FILE* f = OS_fopen(candidate, mode);
            if (f != NULL) {
                return f;
            }
        }
    }

    // 1. Try the path as-is. Skip for known conflict assets opened for reading:
    //    gApplication_path points to the primary game, so "as-is" would always
    //    resolve to the primary game's copy. Step 2 uses s_active_game first.
    if (writing || !meld_is_conflict(meld_basename(path))) {
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
                    FILE* patched;
                    LOG_INFO3("Meld_fopen: conflict patch triggered for %s (game %d)", tail, order[g]);
                    fclose(f);
                    patched = meld_patch_txt_serve(candidate, order[g], s_game_method[order[g]]);
                    if (patched != NULL) {
                        return patched;
                    }
                    LOG_WARN2("Meld_fopen: patch failed, serving raw: %s", candidate);
                    f = OS_fopen(candidate, mode);
                    if (f != NULL) {
                        return f;
                    }
                    continue;
                }
                return f;
            }
        }
    }

    LOG_INFO2("Meld_fopen: not found: %s", path);
    return NULL;
}


// Called from InitGame after cars_available[0] is set to frank_or_anniness.
// Appends extra starting-car indices (pre-computed by meld_build_opponents)
// so both games' starting car appears on the change-car screen.
void Meld_AddBothStartingCars(int frank, int* cars_avail, int* num_cars) {
    int i;
    if (!gMeld_active || !gMeld_both_starting_cars || frank < 0 || frank > 1) {
        return;
    }
    for (i = 0; i < s_extra_start_counts[frank]; i++) {
        if (*num_cars < 60) {
            cars_avail[(*num_cars)++] = s_extra_start_slots[frank][i];
        }
    }
}

// ---------------------------------------------------------------------------
// MeldNetRaces: inject net-only tracks into the SP campaign
// ---------------------------------------------------------------------------

#define MELD_MAX_NET_MAPS 32
static tMeld_race s_net_races[MELD_MAX_NET_MAPS];
static int s_net_map_count = 0;

/* XOR-decode a method-1 game data line in place.
 * Lines that start with '@' are encoded; the '@' is stripped and the rest decoded. */
static void meld_decode_line_m1(char* s) {
    static const unsigned char key[16] = {
        0x6c,0x1b,0x99,0x5f, 0xb9,0xcd,0x5f,0x13,
        0xcb,0x04,0x20,0x0e, 0x5e,0x1c,0xa1,0x0e
    };
    unsigned char* p;
    int len, seed, i;
    len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    if (len == 0 || s[0] != '@') {
        return;
    }
    memmove(s, s + 1, (size_t)len); /* drop '@' */
    len--;
    p = (unsigned char*)s;
    seed = len % 16;
    for (i = 0; i < len; i++) {
        if (p[i] == 0x9f) p[i] = '\t';
        p[i] = ((key[seed] ^ (p[i] - 32)) & 0x7F) + 32;
        seed = (seed + 7) % 16;
        if (p[i] == 0x9f) p[i] = '\t';
    }
}

/* Read one significant (non-blank, non-comment) decoded line from an encrypted track file.
 * Returns 1 on success, 0 on EOF. */
static int meld_read_decoded_sig_line(FILE* f, int method, char* out, int out_len) {
    char buf[256];
    while (fgets(buf, sizeof(buf), f) != NULL) {
        char* p = buf;
        char* end;
        if (method == 1) {
            meld_decode_line_m1(buf);
        } else {
            int l = (int)strlen(buf);
            while (l > 0 && (buf[l - 1] == '\r' || buf[l - 1] == '\n')) buf[--l] = '\0';
            if (l > 0 && buf[0] == '@') memmove(buf, buf + 1, (size_t)l);
        }
        /* trim leading whitespace */
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        /* skip blank and comment lines */
        if (*p == '\0' || *p == '\n' || *p == '\r' || (p[0] == '/' && p[1] == '/')) {
            continue;
        }
        /* trim trailing whitespace and comments */
        end = p + strlen(p) - 1;
        while (end > p && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
            *end-- = '\0';
        }
        /* strip inline comment ( //<space> or // ) */
        {
            char* cm = strstr(p, "//");
            if (cm != NULL) {
                /* back-trim whitespace before the // */
                char* t = cm - 1;
                while (t > p && (*t == ' ' || *t == '\t')) {
                    t--;
                }
                *(t + 1) = '\0';
            }
        }
        if (*p == '\0') {
            continue;
        }
        strncpy(out, p, (size_t)(out_len - 1));
        out[out_len - 1] = '\0';
        return 1;
    }
    return 0;
}

/* Open and scan a track file (DATA/RACES/<name>) from game dir game_idx,
 * returning the map-image pixelmap basename via out[].
 * Strategy: the map-image line immediately follows 3 consecutive .MAT lines
 * (standard_screen x3) + 1 integer (special_screens_count) + count*2 lines. */
static int meld_extract_map_pix(int game_idx, int method, const char* track_file, char* out, int out_len) {
    char path[MAX_PATH];
    FILE* f;
    char line[256];
    int mat_streak;
    size_t ln;

    meld_join(path, sizeof(path), harness_game_config.game_dirs[game_idx].directory, "DATA");
    meld_join(path, sizeof(path), path, "RACES");
    meld_join(path, sizeof(path), path, track_file);

    f = OS_fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }

    mat_streak = 0;
    while (meld_read_decoded_sig_line(f, method, line, sizeof(line))) {
        ln = strlen(line);
        if (ln >= 4 && strcasecmp(line + ln - 4, ".MAT") == 0) {
            mat_streak++;
        } else {
            /* Non-MAT line: if preceded by exactly 3 MAT lines, this IS special_screens_count */
            if (mat_streak == 3) {
                int screens, skip, i;
                char* base;
                char* sep;
                screens = atoi(line);
                /* Each special screen = 4 scalars (1 line) + material name (1 line) */
                skip = screens * 2;
                for (i = 0; i < skip; i++) {
                    if (!meld_read_decoded_sig_line(f, method, line, sizeof(line))) {
                        goto done;
                    }
                }
                /* Next line is the map pixelmap filename */
                if (!meld_read_decoded_sig_line(f, method, line, sizeof(line))) {
                    break;
                }
                base = line;
                for (sep = line; *sep; sep++) {
                    if (*sep == '/' || *sep == '\\') {
                        base = sep + 1;
                    }
                }
                strncpy(out, base, (size_t)(out_len - 1));
                out[out_len - 1] = '\0';
                fclose(f);
                return 1;
            }
            mat_streak = 0;
        }
    }
done:
    fclose(f);
    return 0;
}

int Meld_NetRaces_GetSPCount(void) {
    return s_net_map_base_count;
}

int Meld_NetRaces_GetNetCount(void) {
    return s_net_map_count;
}

int Meld_IsArenaTrack(void) {
    return gMeld_use_net_starts;
}

void Meld_NetRaces_Init(void) {
    int g, i, l, n, m;
    tMeld_race r;
    FILE* f;
    int sp_count;
    tMeld_buf buf;
    int game_tc[MELD_MAX_GAMES];
    int main_idx[MELD_MAX_RACES];
    int solo_idx[MELD_MAX_RACES];
    int main_count, solo_count;
    tMeld_race* result[MELD_MAX_RACES];
    int result_game[MELD_MAX_RACES];

    if (!harness_game_config.meld_net_races) {
        return;
    }
    if (harness_game_config.game_dirs_count < 1) {
        return;
    }

    // Ensure decode methods are known for all configured game dirs.
    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        if (s_game_method[g] == 0) {
            s_game_method[g] = meld_detect_method(g);
        }
    }

    // When meld is not active s_races[] is empty; populate it from each game's
    // RACES.TXT so we have SP track names to compare against.
    if (!gMeld_active && s_race_count == 0) {
        for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
            int method = s_game_method[g];
            f = meld_open_data(g, "RACES.TXT", "rb");
            if (f == NULL) {
                continue;
            }
            while (s_race_count < MELD_MAX_RACES) {
                int rc = meld_read_one_race(f, method, &s_races[s_race_count]);
                if (rc <= 0) {
                    break;
                }
                if (s_races[s_race_count].num_lines <= 4) {
                    continue;
                }
                s_races[s_race_count].game_idx = g;
                s_race_count++;
            }
            fclose(f);
        }
    }

    sp_count = s_race_count;

    // Scan each game's NETRACES.TXT; collect entries not present in any SP list.
    s_net_map_count = 0;
    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        int method = s_game_method[g];
        f = meld_open_data(g, "NETRACES.TXT", "rb");
        if (f == NULL) {
            continue;
        }
        for (;;) {
            int found, rc;
            if (s_net_map_count >= MELD_MAX_NET_MAPS) {
                break;
            }
            rc = meld_read_one_race(f, method, &r);
            if (rc <= 0) {
                break;
            }
            found = 0;
            for (i = 0; i < sp_count && !found; i++) {
                if (strcasecmp(r.lines[0], s_races[i].lines[0]) == 0) {
                    found = 1;
                }
            }
            if (found) {
                continue;
            }
            // Dedup across game dirs: skip if same name was already collected.
            for (i = 0; i < s_net_map_count && !found; i++) {
                if (strcasecmp(r.lines[0], s_net_races[i].lines[0]) == 0) {
                    found = 1;
                }
            }
            if (found) {
                continue;
            }
            s_net_races[s_net_map_count] = r;
            s_net_races[s_net_map_count].game_idx = g;
            s_net_map_count++;
        }
        fclose(f);
    }

    if (s_net_map_count == 0) {
        LOG_INFO("MeldNetRaces: no net-only tracks found");
        return;
    }

    // Interleave SP and net races into final campaign order:
    //   - Games with exactly 1 SP track slot in at rank ~50
    //   - Net-only races are spread between rank 50 and rank 10
    //   - Multi-track SP games fill the remaining slots in their sorted order
    {
        int total = sp_count + s_net_map_count;
        int solo_pos, net_start, net_end;

        memset(game_tc, 0, sizeof(game_tc));
        for (i = 0; i < sp_count; i++) {
            int gi = s_races[i].game_idx;
            if (gi >= 0 && gi < MELD_MAX_GAMES) {
                game_tc[gi]++;
            }
        }
        main_count = 0;
        solo_count = 0;
        for (i = 0; i < sp_count; i++) {
            int gi = s_races[i].game_idx;
            if (gi >= 0 && gi < MELD_MAX_GAMES && game_tc[gi] == 1) {
                solo_idx[solo_count++] = i;
            } else {
                main_idx[main_count++] = i;
            }
        }

        memset(result, 0, sizeof(tMeld_race*) * (size_t)total);
        memset(result_game, 0, sizeof(int) * (size_t)total);

        // Position in the list for rank R: (99-R)*(total-1)/98
        solo_pos = (total > 1) ? (49 * (total - 1) / 98) : 0;
        net_end = (total > 1) ? (89 * (total - 1) / 98) : (total - 1);
        net_start = solo_pos + solo_count;
        if (net_start > net_end) {
            net_end = net_start + s_net_map_count;
        }

        for (i = 0; i < solo_count && solo_pos + i < total; i++) {
            result[solo_pos + i] = &s_races[solo_idx[i]];
            result_game[solo_pos + i] = s_races[solo_idx[i]].game_idx;
        }

        for (n = 0; n < s_net_map_count; n++) {
            int range = net_end - net_start;
            int net_pos = net_start + (s_net_map_count == 1 ? range / 2 : (n + 1) * range / (s_net_map_count + 1));
            while (net_pos < total && result[net_pos] != NULL) {
                net_pos++;
            }
            if (net_pos >= total) {
                net_pos = net_end;
                while (net_pos >= net_start && result[net_pos] != NULL) {
                    net_pos--;
                }
            }
            if (net_pos >= 0 && net_pos < total) {
                result[net_pos] = &s_net_races[n];
                result_game[net_pos] = s_net_races[n].game_idx;
            }
        }

        m = 0;
        for (i = 0; i < total && m < main_count; i++) {
            if (result[i] == NULL) {
                result[i] = &s_races[main_idx[m]];
                result_game[i] = s_races[main_idx[m]].game_idx;
                m++;
            }
        }

        free(s_races_buf);
        s_races_buf = NULL;
        s_races_len = 0;
        mbuf_init(&buf);
        memset(s_race_is_arena, 0, sizeof(s_race_is_arena));
        memset(s_race_map_pix, 0, sizeof(s_race_map_pix));
        {
        int seq = 0; /* sequential index matching gRace_list[] */
        for (i = 0; i < total; i++) {
            int is_net;
            if (result[i] == NULL) {
                continue;
            }
            s_race_source_game[seq] = result_game[i];
            is_net = (result[i] >= s_net_races && result[i] < s_net_races + MELD_MAX_NET_MAPS);
            s_race_is_arena[seq] = is_net;
            if (is_net) {
                /* Extract map pixelmap filename from track file for use in race selection UI */
                meld_extract_map_pix(result_game[i], s_game_method[result_game[i]], result[i]->lines[2],
                    s_race_map_pix[seq], MELD_MAP_PIX_LEN);
                LOG_INFO3("Meld arena track seq=%d track=%s", seq, result[i]->lines[2]);
                LOG_INFO2("Meld arena pix='%s'", s_race_map_pix[seq]);
            }
            seq++;
            if (is_net) {
                /* Net race: line0=name, line1=FLI, line2=track, line3=chunk_count(0) */
                char fli_line[MELD_LINE_LEN];
                char tmp[MELD_LINE_LEN];
                char *tok1, *tok2, *tok3;
                mbuf_append_m1(&buf, result[i]->lines[0]);
                strncpy(tmp, result[i]->lines[1], MELD_LINE_LEN - 1);
                tmp[MELD_LINE_LEN - 1] = '\0';
                tok1 = strtok(tmp, ",");
                tok2 = strtok(NULL, ",");
                tok3 = strtok(NULL, ",");
                if (tok1 && tok2 && tok3) {
                    /* Prefer DATA/ANIM/{trackname}.FLI as the scene thumbnail if it exists
                     * in any game dir; fall back to the info-background (tok3) otherwise. */
                    char scene_fli[MELD_LINE_LEN];
                    char track_stem[MELD_LINE_LEN];
                    char anim_rel[MELD_LINE_LEN];
                    char *dot;
                    int gd;
                    FILE *probe;
                    strncpy(track_stem, result[i]->lines[2], MELD_LINE_LEN - 1);
                    track_stem[MELD_LINE_LEN - 1] = '\0';
                    dot = strrchr(track_stem, '.');
                    if (dot) {
                        *dot = '\0';
                    }
                    snprintf(anim_rel, sizeof(anim_rel), "ANIM" MELD_SEP "%s.FLI", track_stem);
                    scene_fli[0] = '\0';
                    for (gd = 0; gd < harness_game_config.game_dirs_count && gd < MELD_MAX_GAMES; gd++) {
                        probe = meld_open_data(gd, anim_rel, "rb");
                        if (probe) {
                            fclose(probe);
                            snprintf(scene_fli, sizeof(scene_fli), "%s.FLI", track_stem);
                            s_race_has_scene_fli[seq - 1] = 1;
                            break;
                        }
                    }
                    snprintf(fli_line, MELD_LINE_LEN, "%s,%s,%s",
                        scene_fli[0] != '\0' ? scene_fli : tok3, tok2, tok3);
                } else {
                    strncpy(fli_line, result[i]->lines[1], MELD_LINE_LEN - 1);
                    fli_line[MELD_LINE_LEN - 1] = '\0';
                }
                mbuf_append_m1(&buf, fli_line);
                mbuf_append_m1(&buf, result[i]->lines[2]);
                /* Inject one text chunk showing "(???)" for the view-info screen.
                 * GetALineAndDontArgue skips lines not starting with alnum or -./!&('"
                 * so the text must start with an allowed character — '(' qualifies. */
                mbuf_append_m1(&buf, "1");
                mbuf_append_m1(&buf, "5,3");
                mbuf_append_m1(&buf, "0,99");
                mbuf_append_m1(&buf, "1");
                mbuf_append_m1(&buf, "(???)");
            } else {
                for (l = 0; l < result[i]->num_lines; l++) {
                    mbuf_append_m1(&buf, result[i]->lines[l]);
                }
            }
        }
        mbuf_append_m1(&buf, "END");

        s_net_map_base_count = sp_count;
        s_race_count = total;
        }
    }

    s_races_buf = buf.data;
    s_races_len = buf.len;
    gMeld_net_races_active = 1;

    // Reset so LoadGeneralParameters re-detects the encryption method from the
    // primary game's GENERAL.TXT (meld_detect_method leaves it set to the last
    // game it inspected).
    gEncryption_method = 0;

    LOG_INFO2("MeldNetRaces: %d net-only track(s) added to campaign", s_net_map_count);
}

void Meld_Test_AddConflict(const char* basename) {
    meld_add_conflict(basename);
}

void Meld_Test_ClearConflicts(void) {
    s_conflict_count = 0;
}

FILE* Meld_Test_PatchTxt(const char* path, int game_idx) {
    return meld_patch_txt_serve(path, game_idx, 1);
}
