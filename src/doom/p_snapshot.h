//
// Copyright(C) 2024, P_UnArchiveThinkers
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//   In-memory snapshot system for demo seeking.
//

#ifndef __P_SNAPSHOT__
#define __P_SNAPSHOT__

#include <stdlib.h>
#include <string.h>

// A snapshot of the game state at a specific tic.
typedef struct tic_snapshot_s
{
    int tic;

    // Serialized data buffers
    byte *thinker_data;
    size_t thinker_len;

    // Key global variables
    int leveltime;
    int rndindex;
    int prndindex;

} tic_snapshot_t;

void SNAP_Take(void);
tic_snapshot_t *SNAP_FindBefore(int target_tic);
void SNAP_Restore(tic_snapshot_t *snap);
void SNAP_Clear(void);

#endif

