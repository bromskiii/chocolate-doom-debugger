
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p_snapshot.h"
#include "p_local.h"
#include "p_mobj.h"
#include "g_game.h"
#include "m_random.h"
#include "z_zone.h"
#include "memio.h"
#include "i_system.h"
#include "r_state.h"

// Ring buffer for snapshots
#define MAX_SNAPSHOTS 30
static tic_snapshot_t *snapshots[MAX_SNAPSHOTS];
static int snapshot_idx = 0;

// Helper struct for pointer fix-up during serialization
typedef struct
{
    mobj_t *mobj;
    uint32_t id;
} mobj_id_map_t;

// Helper struct for pointer fix-up during deserialization
typedef struct {
    mobj_t *mobj;
    uint32_t target_id;
    uint32_t tracer_id;
} mobj_fixup_t;

// Thinker classes, from p_saveg.c
typedef enum
{
    tc_end,
    tc_mobj
} thinkerclass_t;

//
// Memory Stream I/O
//
static void snap_write8(MEMFILE *stream, byte value) { mem_fwrite(&value, 1, 1, stream); }
static void snap_write32(MEMFILE *stream, int value) { mem_fwrite(&value, 4, 1, stream); }
static byte snap_read8(MEMFILE *stream) { byte val; mem_fread(&val, 1, 1, stream); return val; }
static int snap_read32(MEMFILE *stream) { int val; mem_fread(&val, 4, 1, stream); return val; }

//
// Snapshot Serialization (Archiving)
//

// Find the ID for a given mobj_t pointer
static uint32_t find_mobj_id(mobj_t *mobj, mobj_id_map_t *map, int count)
{
    int i;
    if (!mobj) return (uint32_t)-1;
    for (i = 0; i < count; i++) {
        if (map[i].mobj == mobj) {
            return map[i].id;
        }
    }
    return (uint32_t)-1; // Not found
}

// Serialize a single mobj_t, converting pointers to IDs
static void snap_write_mobj_t(MEMFILE *stream, mobj_t *str, mobj_id_map_t *map, int map_count)
{
    uint32_t state_idx = str->state ? (str->state - states) : (uint32_t)-1;
    int player_idx = str->player ? (str->player - players) : -1;
    uint32_t target_id = find_mobj_id(str->target, map, map_count);
    uint32_t tracer_id = find_mobj_id(str->tracer, map, map_count);

    // Write all mobj_t fields. Pointers that are recalculated on restore
    // (like thinker.prev/next) are written as-is but ignored on read.
    mem_fwrite(str, sizeof(mobj_t), 1, stream);

    // Overwrite the pointers we need to fix up with their IDs
    mem_fseek(stream, (long)offsetof(mobj_t, state), MEM_SEEK_SET);
    snap_write32(stream, state_idx);

    mem_fseek(stream, (long)offsetof(mobj_t, player), MEM_SEEK_SET);
    snap_write32(stream, player_idx);

    mem_fseek(stream, (long)offsetof(mobj_t, target), MEM_SEEK_SET);
    snap_write32(stream, target_id);

    mem_fseek(stream, (long)offsetof(mobj_t, tracer), MEM_SEEK_SET);
    snap_write32(stream, tracer_id);

    // Return to end for next write
    mem_fseek(stream, 0, MEM_SEEK_END);
}

// Serialize all thinkers
static void SNAP_ArchiveThinkers(MEMFILE *stream)
{
    thinker_t *th;
    int mobj_count = 0;
    mobj_id_map_t *mobj_map;
    int i;

    // First pass: count thinkers and build mobj_t -> id map
    for (th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker) {
            mobj_count++;
        }
    }

    mobj_map = Z_Malloc(mobj_count * sizeof(mobj_id_map_t), PU_STATIC, NULL);
    i = 0;
    for (th = thinkercap.next; th != &thinkercap; th = th->next) {
        if (th->function.acp1 == (actionf_p1)P_MobjThinker) {
            mobj_map[i].mobj = (mobj_t *)th;
            mobj_map[i].id = i;
            i++;
        }
    }

    // Write the count of mobjs first
    snap_write32(stream, mobj_count);

    // Second pass: serialize thinkers
    for (i = 0; i < mobj_count; i++) {
        snap_write_mobj_t(stream, mobj_map[i].mobj, mobj_map, mobj_count);
    }

    Z_Free(mobj_map);
}

//
// Snapshot Deserialization (Unarchiving)
//

// Deserialize a single mobj_t, storing pointer IDs for later fix-up
static void snap_read_mobj_t(MEMFILE *stream, mobj_t *str, mobj_fixup_t *fixup)
{
    uint32_t state_idx;
    int player_idx;

    mem_fread(str, sizeof(mobj_t), 1, stream);

    // Read back the IDs we stored over the pointers
    mem_fseek(stream, (long)offsetof(mobj_t, state), MEM_SEEK_SET);
    state_idx = snap_read32(stream);
    str->state = (state_idx != (uint32_t)-1) ? &states[state_idx] : NULL;

    mem_fseek(stream, (long)offsetof(mobj_t, player), MEM_SEEK_SET);
    player_idx = snap_read32(stream);
    str->player = (player_idx != -1) ? &players[player_idx] : NULL;

    mem_fseek(stream, (long)offsetof(mobj_t, target), MEM_SEEK_SET);
    fixup->target_id = snap_read32(stream);

    mem_fseek(stream, (long)offsetof(mobj_t, tracer), MEM_SEEK_SET);
    fixup->tracer_id = snap_read32(stream);

    // Return to end for next read
    mem_fseek(stream, 0, MEM_SEEK_END);
}

// Deserialize all thinkers and perform pointer fix-up
static void SNAP_UnArchiveThinkers(MEMFILE *stream)
{
    mobj_t *mobj;
    thinker_t *currentthinker, *next;
    int mobj_count, i;
    mobj_t **id_to_mobj_map;
    mobj_fixup_t *fixups;

    // Clear existing thinkers
    currentthinker = thinkercap.next;
    while (currentthinker != &thinkercap) {
        next = currentthinker->next;
        if (currentthinker->function.acp1 == (actionf_p1)P_MobjThinker)
            P_RemoveMobj((mobj_t *)currentthinker);
        else
            Z_Free(currentthinker);
        currentthinker = next;
    }
    P_InitThinkers();

    // Read in saved thinkers
    mobj_count = snap_read32(stream);
    if (mobj_count == 0) return;

    id_to_mobj_map = Z_Malloc(mobj_count * sizeof(mobj_t*), PU_LEVEL, NULL);
    fixups = Z_Malloc(mobj_count * sizeof(mobj_fixup_t), PU_LEVEL, NULL);

    for (i = 0; i < mobj_count; i++) {
        mobj = Z_Malloc(sizeof(*mobj), PU_LEVEL, NULL);
        fixups[i].mobj = mobj;
        snap_read_mobj_t(stream, mobj, &fixups[i]);

        id_to_mobj_map[i] = mobj;

        // Restore player's mobj pointer
        if (mobj->player) {
            mobj->player->mo = mobj;
        }

        // Re-link into world
        P_SetThingPosition(mobj);
        mobj->info = &mobjinfo[mobj->type];
        mobj->thinker.function.acp1 = (actionf_p1)P_MobjThinker;
        P_AddThinker(&mobj->thinker);
    }

    // Pointer fix-up pass
    for (i = 0; i < mobj_count; i++) {
        mobj = fixups[i].mobj;
        if (fixups[i].target_id != (uint32_t)-1) {
            mobj->target = id_to_mobj_map[fixups[i].target_id];
        } else {
            mobj->target = NULL;
        }
        if (fixups[i].tracer_id != (uint32_t)-1) {
            mobj->tracer = id_to_mobj_map[fixups[i].tracer_id];
        } else {
            mobj->tracer = NULL;
        }
    }

    Z_Free(id_to_mobj_map);
    Z_Free(fixups);
}

//
// Dummy serialization functions for other game state.
// A full implementation would serialize/deserialize these as well.
// For now, they are stubs.
//
static void SNAP_ArchivePlayers(MEMFILE *stream) { (void)stream; }
static void SNAP_UnArchivePlayers(MEMFILE *stream) { (void)stream; }
static void SNAP_ArchiveWorld(MEMFILE *stream) { (void)stream; }
static void SNAP_UnArchiveWorld(MEMFILE *stream) { (void)stream; }
static void SNAP_ArchiveSpecials(MEMFILE *stream) { (void)stream; }
static void SNAP_UnArchiveSpecials(MEMFILE *stream) { (void)stream; }

//
// Public API Implementation
//

static void free_snapshot_data(tic_snapshot_t *snap)
{
    if (!snap) return;
    free(snap->player_data);
    free(snap->world_data);
    free(snap->thinker_data);
    free(snap->specials_data);
}

void SNAP_Clear(void)
{
    int i;
    for (i = 0; i < MAX_SNAPSHOTS; i++) {
        if (snapshots[i]) {
            free_snapshot_data(snapshots[i]);
            Z_Free(snapshots[i]);
            snapshots[i] = NULL;
        }
    }
    snapshot_idx = 0;
}

void SNAP_Take(void)
{
    MEMFILE *stream;
    tic_snapshot_t *snap;
    byte *temp_buf;
    size_t len;

    // Overwrite oldest snapshot
    if (snapshots[snapshot_idx]) {
        free_snapshot_data(snapshots[snapshot_idx]);
    } else {
        snapshots[snapshot_idx] = Z_Malloc(sizeof(tic_snapshot_t), PU_STATIC, NULL);
    }
    snap = snapshots[snapshot_idx];

    snap->tic = gametic;
    snap->leveltime = leveltime;
    snap->rndindex = rndindex;
    snap->prndindex = prndindex;

    // For this implementation, we only serialize thinkers, as it's the most complex part.
    // A full implementation would serialize players, world, and specials as well.
    snap->player_data = NULL; snap->player_len = 0;
    snap->world_data = NULL; snap->world_len = 0;
    snap->specials_data = NULL; snap->specials_len = 0;

    // Archive thinkers
    stream = mem_fopen_write();
    SNAP_ArchiveThinkers(stream);
    mem_get_buf(stream, (void**)&temp_buf, &len);
    snap->thinker_data = malloc(len);
    memcpy(snap->thinker_data, temp_buf, len);
    snap->thinker_len = len;
    mem_fclose(stream);

    // Advance ring buffer index
    snapshot_idx = (snapshot_idx + 1) % MAX_SNAPSHOTS;
}

tic_snapshot_t *SNAP_FindBefore(int target_tic)
{
    tic_snapshot_t *best_snap = NULL;
    int best_tic = -1;
    int i;

    for (i = 0; i < MAX_SNAPSHOTS; i++) {
        if (snapshots[i] && snapshots[i]->tic <= target_tic) {
            if (best_snap == NULL || snapshots[i]->tic > best_tic) {
                best_tic = snapshots[i]->tic;
                best_snap = snapshots[i];
            }
        }
    }
    return best_snap;
}

void SNAP_Restore(tic_snapshot_t *snap)
{
    MEMFILE *stream;

    // A full implementation would unarchive players and world state first.
    // For now, we only handle thinkers and globals.

    // Restore globals
    gametic = snap->tic;
    leveltime = snap->leveltime;
    rndindex = snap->rndindex;
    prndindex = snap->prndindex;

    // Unarchive thinkers
    if (snap->thinker_data) {
        stream = mem_fopen_read(snap->thinker_data, snap->thinker_len);
        SNAP_UnArchiveThinkers(stream);
        mem_fclose(stream);
    }
}

