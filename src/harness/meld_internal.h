// Internal header for the meld module: shared types, constants, state, and
// helper declarations used across meld.c, meld_netraces.c, and gog.c.
// Not part of the public harness API.

#ifndef HARNESS_MELD_INTERNAL_H
#define HARNESS_MELD_INTERNAL_H

#include <stddef.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Platform
// ---------------------------------------------------------------------------

#ifdef _WIN32
#define MELD_SEP    "\\"
#define MELD_SEP_CH '\\'
#else
#define MELD_SEP    "/"
#define MELD_SEP_CH '/'
#endif

// Return pointer to the filename component of path, recognising both '/' and
// '\' as separators. Returns a pointer into the original string (no copy).
// Not OS_Basename: that uses _splitpath on Windows (drops extension) and POSIX
// basename() on Linux (ignores '\', static buffer).
static inline const char* meld_basename(const char* path) {
    const char* p = path;
    const char* last = path;
    for (; *p; p++) {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
    }
    return last;
}

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MELD_MAX_GAMES      10
#define MELD_MAX_RACES      200
#define MELD_MAX_RACE_LINES 80
#define MELD_LINE_LEN       512
#define MELD_MAP_PIX_LEN    16

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct {
    char  lines[MELD_MAX_RACE_LINES][MELD_LINE_LEN];
    int   num_lines;
    int   game_idx;
    int   order;
    float norm;
} tMeld_race;

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} tMeld_buf;

// ---------------------------------------------------------------------------
// External game functions (defined in the DETHRACE module)
// ---------------------------------------------------------------------------

extern void EncodeLine(char* pS);
extern int  gEncryption_method;

// ---------------------------------------------------------------------------
// Shared state — defined in meld.c, shared with meld_netraces.c
// ---------------------------------------------------------------------------

extern tMeld_race s_races[MELD_MAX_RACES];
extern int        s_race_count;
extern char*      s_races_buf;
extern size_t     s_races_len;
extern int        s_race_source_game[MELD_MAX_RACES];
extern int        s_race_is_arena[MELD_MAX_RACES];
extern char       s_race_map_pix[MELD_MAX_RACES][MELD_MAP_PIX_LEN];
extern int        s_race_has_scene_fli[MELD_MAX_RACES];
extern int        s_game_method[MELD_MAX_GAMES];
extern int        s_current_race_index;

// ---------------------------------------------------------------------------
// Shared helpers — defined in meld.c (non-static), used in meld_netraces.c
// ---------------------------------------------------------------------------

void  meld_join(char* dest, size_t len, const char* a, const char* b);
FILE* meld_open_data(int game_idx, const char* name, const char* mode);
int   meld_detect_method(int game_idx);
int   meld_read_one_race(FILE* f, int method, tMeld_race* r);
void  mbuf_init(tMeld_buf* b);
void  mbuf_append(tMeld_buf* b, const char* s);
void  mbuf_append_m1(tMeld_buf* b, const char* line);

#endif
