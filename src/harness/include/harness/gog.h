#ifndef HARNESS_GOG_H
#define HARNESS_GOG_H

#include <stdio.h>

/* Init the GOG slot for one game directory (idx must be < 10).
 * Called by meld.c for each [Games] entry during Meld_Init. No-op if no
 * .GOG file is found under dir. */
void Gog_InitSlot(int idx, const char* dir);

/* Scan all initialised game slots for MIX_INTR.SMK and pick one at random.
 * Must be called after all Gog_InitSlot calls in Meld_Init. */
void Gog_BuildIntroProviders(void);

/* VFS fopen for meld mode: searches GOG images for the named SMK file.
 * active_game slot is tried first; others are searched in order.
 * Returns NULL if not found or if path is not an SMK. Never handles writes. */
FILE* Gog_FopenMeld(const char* path, int active_game);

/* Init for single-game mode (Meld=0). dir may be NULL to fall back to
 * getcwd(). */
void Gog_InitSingle(const char* dir);

/* VFS fopen for single-game mode: returns NULL if not an SMK or not in GOG. */
FILE* Gog_FopenSingle(const char* path, const char* mode);

#endif
