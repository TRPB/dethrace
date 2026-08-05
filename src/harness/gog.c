// GOG ISO VFS: serves SMK cutscene files from GAME.GOG / SPLAT.GOG images
// without requiring them to be extracted or mounted by the OS.
// Works in both single-game mode (Meld=0, via Gog_InitSingle / Gog_FopenSingle)
// and meld mode (Meld=1, via Gog_InitSlot / Gog_BuildIntroProviders / Gog_FopenMeld).

#include "harness/gog.h"
#include "harness/config.h"
#include "harness/iso.h"
#include "harness/os.h"
#include "harness/trace.h"
#include "meld_internal.h"

#include <ctype.h>
#if defined(_MSC_VER) && _MSC_VER <= 1020
typedef unsigned long uint32_t;
#else
#include <stdint.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

/* Slot used for single-game (Meld=0) mode. */
#define GOG_SINGLE_IDX  MELD_MAX_GAMES
/* Total array size: one per game dir + one for single-game mode. */
#define GOG_ARRAY_SIZE  (MELD_MAX_GAMES + 1)
/* Max SMK entries tracked per GOG image. */
#define GOG_MAX_SMKS      32

typedef struct {
    tIso_image* img;
    tIso_entry  smks[GOG_MAX_SMKS];
    int         smk_count;
} tGog_slot;

static tGog_slot s_gog[GOG_ARRAY_SIZE];

/* Randomly selected intro provider index (into s_gog[]), or -1 if not yet set. */
static int  s_intro_game_idx = -1;
/* Non-empty: serve the intro from this on-disk path rather than a GOG slot. */
static char s_intro_disk_path[MAX_PATH];

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

static int gog_ext_eq_smk(const char* path) {
    size_t n = strlen(path);
    if (n < 4) {
        return 0;
    }
    return path[n - 4] == '.'
        && (path[n - 3] == 's' || path[n - 3] == 'S')
        && (path[n - 2] == 'm' || path[n - 2] == 'M')
        && (path[n - 1] == 'k' || path[n - 1] == 'K');
}

// ---------------------------------------------------------------------------
// Internal slot lookup
// ---------------------------------------------------------------------------

static FILE* gog_serve(int idx, const char* smk_name) {
    int i;
    if (s_gog[idx].img == NULL) {
        return NULL;
    }
    for (i = 0; i < s_gog[idx].smk_count; i++) {
        if (strcasecmp(s_gog[idx].smks[i].name, smk_name) == 0) {
            return Iso_ServeEntry(s_gog[idx].img, &s_gog[idx].smks[i]);
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Gog_InitSlot(int idx, const char* dir) {
    char gog_path[MAX_PATH];
    tIso_image* img;

    if (idx < 0 || idx >= GOG_ARRAY_SIZE) {
        return;
    }
    if (!Iso_FindGogInDir(dir, gog_path, sizeof(gog_path))) {
        return;
    }
    img = Iso_Open(gog_path);
    if (img == NULL) {
        return;
    }
    if (Iso_ListDir(img, "DATA/CUTSCENE", s_gog[idx].smks, GOG_MAX_SMKS, &s_gog[idx].smk_count) != 0) {
        LOG_WARN2("GOG: open OK but DATA/CUTSCENE not found: %s", gog_path);
        Iso_Close(img);
        return;
    }
    s_gog[idx].img = img;
}

void Gog_BuildIntroProviders(void) {
    int g, i;
    int      providers[MELD_MAX_GAMES];
    char     src_paths[MELD_MAX_GAMES][MAX_PATH]; /* empty string = GOG source */
    uint32_t seen_sizes[MELD_MAX_GAMES];
    int n = 0, seen_n = 0;
    const char* disk_subpaths[] = {
        "DATA" MELD_SEP "CUTSCENE" MELD_SEP "MIX_INTR.SMK",
        "CUTSCENE" MELD_SEP "MIX_INTR.SMK",
    };

    s_intro_disk_path[0] = '\0';
    s_intro_game_idx = -1;

    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        uint32_t file_size = 0;
        char src_path[MAX_PATH];
        int found = 0;
        int dup;
        size_t sp;

        src_path[0] = '\0';

        /* Prefer a disk file over the GOG image for the same game dir. */
        for (sp = 0; sp < sizeof(disk_subpaths) / sizeof(disk_subpaths[0]); sp++) {
            char candidate[MAX_PATH];
            FILE* f;
            long sz;
            meld_join(candidate, sizeof(candidate),
                harness_game_config.game_dirs[g].directory, disk_subpaths[sp]);
            f = OS_fopen(candidate, "rb");
            if (f == NULL) {
                continue;
            }
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fclose(f);
            if (sz > 0) {
                file_size = (uint32_t)sz;
                strncpy(src_path, candidate, MAX_PATH - 1);
                src_path[MAX_PATH - 1] = '\0';
                found = 1;
            }
            break;
        }
        if (!found) {
            /* GOG image: size is in the directory entry — no file I/O needed. */
            for (i = 0; i < s_gog[g].smk_count; i++) {
                if (strcasecmp(s_gog[g].smks[i].name, "MIX_INTR.SMK") == 0) {
                    file_size = s_gog[g].smks[i].data_size;
                    /* src_path stays empty: served from GOG at play time */
                    found = 1;
                    break;
                }
            }
        }
        if (!found || file_size == 0) {
            continue;
        }

        /* Dedup by file size: identical intros across games have the same byte
           count (e.g. Splat Pack GOG and Xmas Demo disk are both 6,574,960 B).
           This avoids reading large SMK files at startup just to hash them. */
        dup = 0;
        for (i = 0; i < seen_n; i++) {
            if (seen_sizes[i] == file_size) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        seen_sizes[seen_n++] = file_size;

        providers[n] = g;
        memcpy(src_paths[n], src_path, MAX_PATH);
        n++;
    }

    if (n >= 1) {
        int chosen = 0;
        if (n > 1) {
            /* Mix time with clock() for sub-second entropy so repeated quick
               launches don't always land on the same provider. */
            srand((unsigned)time(NULL) ^ (unsigned)clock());
            chosen = rand() % n;
        }
        s_intro_game_idx = providers[chosen];
        strncpy(s_intro_disk_path, src_paths[chosen], MAX_PATH - 1);
        s_intro_disk_path[MAX_PATH - 1] = '\0';
    }
}

FILE* Gog_FopenMeld(const char* path, int active_game) {
    const char* base;
    int order[MELD_MAX_GAMES];
    int n = 0;
    int g;

    if (!gog_ext_eq_smk(path)) {
        return NULL;
    }
    base = meld_basename(path);

    if (s_intro_game_idx >= 0 && strcasecmp(base, "MIX_INTR.SMK") == 0) {
        if (s_intro_disk_path[0] != '\0') {
            return OS_fopen(s_intro_disk_path, "rb");
        }
        return gog_serve(s_intro_game_idx, base);
    }

    if (active_game >= 0 && active_game < harness_game_config.game_dirs_count) {
        order[n++] = active_game;
    }
    for (g = 0; g < harness_game_config.game_dirs_count && g < MELD_MAX_GAMES; g++) {
        if (g != active_game) {
            order[n++] = g;
        }
    }
    for (g = 0; g < n; g++) {
        FILE* f = gog_serve(order[g], base);
        if (f != NULL) {
            return f;
        }
    }
    return NULL;
}

void Gog_InitSingle(const char* dir) {
    char cwd[MAX_PATH];
    const char* search_dir = dir;
    if (search_dir == NULL || search_dir[0] == '\0') {
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            search_dir = cwd;
        } else {
            return;
        }
    }
    Gog_InitSlot(GOG_SINGLE_IDX, search_dir);
}

FILE* Gog_FopenSingle(const char* path, const char* mode) {
    const char* base;
    if (!gog_ext_eq_smk(path)) {
        return NULL;
    }
    if (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) {
        return NULL;
    }
    base = meld_basename(path);
    return gog_serve(GOG_SINGLE_IDX, base);
}
