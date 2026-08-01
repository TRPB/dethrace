#ifndef HARNESS_MELD_H
#define HARNESS_MELD_H

#include <stddef.h>
#include <stdio.h>

// Set to 1 by Meld_Init() when melding is active (Meld=1 and >1 game dirs and
// the merge succeeded). Always defined so both the harness and game-side code
// can reference it regardless of DETHRACE_FIX_BUGS.
extern int gMeld_active;
extern int gMeld_both_starting_cars;

// Total number of races in the merged campaign.
extern int gMeld_total_race_count;
// Number of races contributed by the primary (first) game.
extern int gMeld_primary_race_count;

// Build the merged RACES.TXT / OPPONENT.TXT / PARTSHOP.TXT and set up VFS
// routing. Called from init.c before LoadOpponents(). Sets gMeld_active.
void Meld_Init(void);

// Return a rewound FILE* for the pre-built merged campaign files.
FILE* Meld_OpenRaceFile(void);
FILE* Meld_OpenOpponentFile(void);
FILE* Meld_OpenPartshopFile(void);

// Set the active game (for VFS routing) based on a campaign race index.
void Meld_SetActiveGame(int race_index);
// Set the active game based on an opponent index (into gOpponents).
void Meld_SetActiveGame_Opponent(int opponent_index);

// VFS router: intercepts merged files and routes asset loads through the
// active game dir, then all [Games] dirs in order.
FILE* Meld_fopen(const char* path, const char* mode);

// Music pool support.
int Meld_MusicAvailable(void);
void Meld_ResolveMusicPath(int track, char* out, size_t len);

// Save-file path (writes the meld-specific "SAVEGAME_M" directory path).
void Meld_SavePath(int slot, char* out, size_t len);

#endif
