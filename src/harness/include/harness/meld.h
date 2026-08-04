#ifndef HARNESS_MELD_H
#define HARNESS_MELD_H

#include <stddef.h>
#include <stdio.h>
#include "brender.h"

// Set to 1 by Meld_Init() when melding is active (Meld=1 and >1 game dirs and
// the merge succeeded). Always defined so both the harness and game-side code
// can reference it regardless of DETHRACE_FIX_BUGS.
extern int gMeld_active;
extern int gMeld_both_starting_cars;

// Set to 1 by Meld_NetRaces_Init() when net-only tracks have been injected into
// the SP campaign (MeldNetRaces=1 and at least one net-exclusive track was found).
extern int gMeld_net_races_active;

// Set to 1 by Meld_SetActiveGame() when the current race is a net-only arena
// track. Car placement in car.c uses net_starts[] instead of the SP grid.
extern int gMeld_use_net_starts;

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
int Meld_IsOpponentEligible(int opponent_index);
// Character group id shared by every car variant of one racer (same racer,
// different car per game), so a race picks a character at most once. Returns
// -1 when not melding or for racers that may repeat (cops).
int Meld_OpponentCharacterId(int opponent_index);

// VFS router: intercepts merged files and routes asset loads through the
// active game dir, then all [Games] dirs in order.
FILE* Meld_fopen(const char* path, const char* mode);

// Music pool support.
int Meld_MusicAvailable(void);
void Meld_ResolveMusicPath(int track, char* out, size_t len);

// Save-file path (writes the meld-specific "SAVEGAME_M" directory path).
void Meld_SavePath(int slot, char* out, size_t len);

// Add extra starting-car indices to cars_available for the current character.
// Call immediately after setting cars_available[0] = frank_or_anniness in InitGame.
void Meld_AddBothStartingCars(int frank, int* cars_avail, int* num_cars);

// Inject net-only tracks (those absent from every game's SP RACES.TXT) into the
// SP campaign. Rebuilds s_races_buf and sets gMeld_net_races_active. Call after
// Meld_Init() when MeldNetRaces=1.
void Meld_NetRaces_Init(void);
// Counts used by LoadRaces() to assign correct rank data to the injected entries.
int Meld_NetRaces_GetSPCount(void);
int Meld_NetRaces_GetNetCount(void);
// Returns 1 when the current race is a net-only arena track (no path sections,
// net_starts used for placement).
int Meld_IsArenaTrack(void);
// Returns the map pixelmap basename for the current arena track (e.g.
// "NSUMOMAP.PIX"), or NULL if not an arena track or name not found.
const char* Meld_ArenaMapPixName(void);
// Returns 1 if the current arena track has a custom scene FLI thumbnail.
int Meld_ArenaHasSceneFli(void);
// Blit the arena track overhead map pixelmap centred into the panel area of
// back_screen. Remaps pixels from src_palette to dst_palette colour space.
void Meld_DrawArenaMapPanel(br_pixelmap* back_screen, br_pixelmap* map_image,
    br_pixelmap* src_palette, br_pixelmap* dst_palette, int pl, int pt, int pr, int pb);

// Test helpers: manipulate the conflict map and invoke the patch pipeline
// directly without a full Meld_Init(). Not for production use.
void Meld_Test_AddConflict(const char* basename);
void Meld_Test_ClearConflicts(void);
FILE* Meld_Test_PatchTxt(const char* path, int game_idx);

#endif
