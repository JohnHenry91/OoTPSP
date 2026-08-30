#include "array_count.h"
#include "avoid_ub.h"
#include "printf.h"
#include "regs.h"
#include "romfile.h"
#include "seqcmd.h"
#include "segment_symbols.h"
#include "segmented_address.h"
#include "terminal.h"
#include "translation.h"
#include "versions.h"
#include "z_actor_dlftbls.h"
#include "z_lib.h"
#include "play_state.h"
#include "player.h"
#include "save.h"
#include "scene.h"

SceneCmdHandlerFunc sSceneCmdHandlers[SCENE_CMD_ID_MAX];
RomFile sNaviQuestHintFiles[];

/**
 * Spawn an object file of a specified ID that will persist through room changes.
 *
 * This waits for the file to be fully loaded, the data is available when the function returns.
 *
 * @return The new object slot corresponding to the requested object ID.
 *
 * @note This function is not meant to be called externally to spawn object files on the fly.
 * When an object is spawned with this function, all objects that come before it in the entry list will be treated as
 * persistent, which will likely cause either the amount of free slots or object space memory to run out.
 * This function is only meant to be called internally on scene load, before the object list from any room is processed.
 */
#if TARGET_PSP
/* Forward/backward-movement hang (session 11).
 *
 * Caught live: frames frozen, user_main's pc cycling through the message-queue
 * shim, and DmaMgr_RequestSync called 91672 times with vrom=0, size=0,
 * ram=NULL. The return address resolves to Object_SpawnPersistent+0x54, i.e.
 * the DMA_REQUEST_SYNC below -- so gObjectTable[objectId] is an EMPTY row
 * (vromStart == vromEnd == 0) and the caller retries forever.
 *
 * Record which object, so the empty row can be identified rather than guessed
 * at. numEntries/segment come along because a full slot table or a NULL
 * segment would point at a different failure with the same symptom. */
u32 gPspObjSpawnMagic = 0x504F424A; /* 'POBJ' */
u32 gPspObjSpawnCount;
s32 gPspObjSpawnLastId;
s32 gPspObjSpawnEmptyId;   /* last objectId whose table row was empty */
u32 gPspObjSpawnEmptyCount;
u32 gPspObjSpawnNumEntries;
u32 gPspObjSpawnSegment;
/* Overflow guard, see below. gPspObjSpawnOverflowRa names the caller. */
u32 gPspObjSpawnOverflowCount;
u32 gPspObjSpawnOverflowRa;
u32 gPspObjSpawnOverflowNum;
u32 gPspObjSpawnCallerRa;
/* Scene_ExecuteCommands runaway guard, see the bound inside that loop. */
u32 gPspSceneCmdRunaway;
u32 gPspSceneCmdRunawayAddr;
u32 gPspSceneCmdRunawayCode;
#endif

s32 Object_SpawnPersistent(ObjectContext* objectCtx, s16 objectId) {
    u32 size;

#if TARGET_PSP
    /* HARD BOUND. Measured: numEntries reached 193 against a slots[] of
     * ARRAY_COUNT(objectCtx->slots) entries, with the function called ~10^8
     * times. Vanilla relies on the ASSERT below to catch this, but this build
     * compiles with -DNDEBUG so the ASSERT is gone, and the very first
     * statement of this function then writes slots[numEntries].id far past the
     * array -- an unbounded memory smash that corrupts whatever follows
     * (including, ironically, these diagnostic counters, which is why their
     * values could not be trusted before this guard existed).
     *
     * Same philosophy as the PspRom_Read and os_cache guards: turn an
     * unbounded smash into a bounded, observable event, and record who caused
     * it. Returning the last valid slot keeps callers that index the result
     * from going out of bounds in turn. */
    gPspObjSpawnCallerRa = (u32)__builtin_return_address(0);
    if (objectCtx->numEntries >= ARRAY_COUNT(objectCtx->slots)) {
        gPspObjSpawnOverflowCount++;
        gPspObjSpawnOverflowRa = gPspObjSpawnCallerRa;
        gPspObjSpawnOverflowNum = objectCtx->numEntries;
        return (s32)ARRAY_COUNT(objectCtx->slots) - 1;
    }
#endif

    objectCtx->slots[objectCtx->numEntries].id = objectId;
    size = gObjectTable[objectId].vromEnd - gObjectTable[objectId].vromStart;

#if TARGET_PSP
    gPspObjSpawnCount++;
    gPspObjSpawnLastId = objectId;
    gPspObjSpawnNumEntries = objectCtx->numEntries;
    gPspObjSpawnSegment = (u32)objectCtx->slots[objectCtx->numEntries].segment;
    if (gObjectTable[objectId].vromStart == 0 || size == 0) {
        gPspObjSpawnEmptyId = objectId;
        gPspObjSpawnEmptyCount++;
    }
#endif

    PRINTF("OBJECT[%d] SIZE %fK SEG=%x\n", objectId, size / 1024.0f, objectCtx->slots[objectCtx->numEntries].segment);

    PRINTF("num=%d adrs=%x end=%x\n", objectCtx->numEntries,
           (uintptr_t)objectCtx->slots[objectCtx->numEntries].segment + size, objectCtx->spaceEnd);

    ASSERT(((objectCtx->numEntries < ARRAY_COUNT(objectCtx->slots)) &&
            (((uintptr_t)objectCtx->slots[objectCtx->numEntries].segment + size) < (uintptr_t)objectCtx->spaceEnd)),
           "this->num < OBJECT_EXCHANGE_BANK_MAX && (this->status[this->num].Segment + size) < this->endSegment",
           "../z_scene.c", 142);

    DMA_REQUEST_SYNC(objectCtx->slots[objectCtx->numEntries].segment, gObjectTable[objectId].vromStart, size,
                     "../z_scene.c", 145);

    if (objectCtx->numEntries < (ARRAY_COUNT(objectCtx->slots) - 1)) {
        objectCtx->slots[objectCtx->numEntries + 1].segment =
            (void*)ALIGN16((uintptr_t)objectCtx->slots[objectCtx->numEntries].segment + size);
    }

    objectCtx->numEntries++;
    objectCtx->numPersistentEntries = objectCtx->numEntries;

    return objectCtx->numEntries - 1;
}

// PAL N64 versions reduce the size of object space by 4 KiB in order to give some space back to
// the Zelda arena, which can help prevent an issue where actors fail to spawn in specific areas
// (sometimes referred to as the "Hyrule Field Glitch" although it can happen in more places than Hyrule Field).
#if !OOT_PAL_N64
#define OBJECT_SPACE_ADJUSTMENT 0
#else
#define OBJECT_SPACE_ADJUSTMENT (4 * 1024)
#endif

void Object_InitContext(PlayState* play, ObjectContext* objectCtx) {
    PlayState* play2 = play;
    s32 pad;
    u32 spaceSize;
    s32 i;

    if (play2->sceneId == SCENE_HYRULE_FIELD) {
        spaceSize = 1000 * 1024 - OBJECT_SPACE_ADJUSTMENT;
    } else if (play2->sceneId == SCENE_GANON_BOSS) {
        if (gSaveContext.sceneLayer != 4) {
            spaceSize = 1150 * 1024 - OBJECT_SPACE_ADJUSTMENT;
        } else {
            spaceSize = 1000 * 1024 - OBJECT_SPACE_ADJUSTMENT;
        }
    } else if (play2->sceneId == SCENE_SPIRIT_TEMPLE_BOSS) {
        spaceSize = 1050 * 1024 - OBJECT_SPACE_ADJUSTMENT;
    } else if (play2->sceneId == SCENE_CHAMBER_OF_THE_SAGES) {
        spaceSize = 1050 * 1024 - OBJECT_SPACE_ADJUSTMENT;
    } else if (play2->sceneId == SCENE_GANONDORF_BOSS) {
        spaceSize = 1050 * 1024 - OBJECT_SPACE_ADJUSTMENT;
    } else {
        spaceSize = 1000 * 1024 - OBJECT_SPACE_ADJUSTMENT;
    }

    objectCtx->numEntries = objectCtx->numPersistentEntries = 0;
    objectCtx->mainKeepSlot = objectCtx->subKeepSlot = 0;

    for (i = 0; i < ARRAY_COUNT(objectCtx->slots); i++) {
        objectCtx->slots[i].id = OBJECT_INVALID;
    }

    PRINTF_COLOR_GREEN();
    PRINTF(T("オブジェクト入れ替えバンク情報 %8.3fKB\n", "Object exchange bank data %8.3fKB\n"), spaceSize / 1024.0f);
    PRINTF_RST();

    objectCtx->spaceStart = objectCtx->slots[0].segment =
        GAME_STATE_ALLOC(&play->state, spaceSize, "../z_scene.c", 219);
    objectCtx->spaceEnd = (void*)((uintptr_t)objectCtx->spaceStart + spaceSize);

#if TARGET_PSP
    {
        extern void PspDebugLogKeepObject(unsigned int vromStart, unsigned int vromEnd, void* slot0Segment,
                                           void* spaceEnd);
        PspDebugLogKeepObject((unsigned int)gObjectTable[OBJECT_GAMEPLAY_KEEP].vromStart,
                               (unsigned int)gObjectTable[OBJECT_GAMEPLAY_KEEP].vromEnd, objectCtx->slots[0].segment,
                               objectCtx->spaceEnd);
    }
#endif
    objectCtx->mainKeepSlot = Object_SpawnPersistent(objectCtx, OBJECT_GAMEPLAY_KEEP);
    gSegments[4] = OS_K0_TO_PHYSICAL(objectCtx->slots[objectCtx->mainKeepSlot].segment);
#if TARGET_PSP
    {
        extern void PspDebugLogKeepObject2(void* mainKeepSegment, unsigned int gSegments4);
        PspDebugLogKeepObject2(objectCtx->slots[objectCtx->mainKeepSlot].segment, (unsigned int)gSegments[4]);
    }
#endif
}

void Object_UpdateEntries(ObjectContext* objectCtx) {
    s32 i;
    ObjectEntry* entry = &objectCtx->slots[0];
    RomFile* objectFile;
    u32 size;

    for (i = 0; i < objectCtx->numEntries; i++) {
        if (entry->id < 0) {
            if (entry->dmaRequest.vromAddr == 0) {
                osCreateMesgQueue(&entry->loadQueue, &entry->loadMsg, 1);
                objectFile = &gObjectTable[-entry->id];
                size = objectFile->vromEnd - objectFile->vromStart;

                PRINTF("OBJECT EXCHANGE BANK-%2d SIZE %8.3fK SEG=%08x\n", i, size / 1024.0f, entry->segment);

#if TARGET_PSP
                /* Load object files (character models/textures) synchronously
                 * on this port: the async request runs on a real background
                 * DMA thread here, and the real N64 code relies on single-core
                 * PI-bus-DMA ordering to have the data ready before that
                 * object's display lists are interpreted later the same frame.
                 * With a real background thread that ordering isn't guaranteed,
                 * so gfx_run can read partially-written object memory. Same
                 * fix pattern already proven for the animation-frame DMA (see
                 * z_skelanime.c). Mark it loaded immediately (id flipped
                 * positive) since the data is fully present on return. */
                DMA_REQUEST_SYNC(entry->segment, objectFile->vromStart, size, "../z_scene.c", 266);
                entry->dmaRequest.vromAddr = objectFile->vromStart;
                entry->id = -entry->id;
#else
                DMA_REQUEST_ASYNC(&entry->dmaRequest, entry->segment, objectFile->vromStart, size, 0, &entry->loadQueue,
                                  NULL, "../z_scene.c", 266);
            } else if (osRecvMesg(&entry->loadQueue, NULL, OS_MESG_NOBLOCK) == 0) {
                entry->id = -entry->id;
#endif
            }
        }
        entry++;
    }
}

s32 Object_GetSlot(ObjectContext* objectCtx, s16 objectId) {
    s32 i;

    for (i = 0; i < objectCtx->numEntries; i++) {
        if (ABS(objectCtx->slots[i].id) == objectId) {
            return i;
        }
    }

    return -1;
}

s32 Object_IsLoaded(ObjectContext* objectCtx, s32 slot) {
    if (objectCtx->slots[slot].id > 0) {
        return true;
    } else {
        return false;
    }
}

void func_800981B8(ObjectContext* objectCtx) {
    s32 i;
    s32 id;
    u32 size;

    for (i = 0; i < objectCtx->numEntries; i++) {
        id = objectCtx->slots[i].id;
        size = gObjectTable[id].vromEnd - gObjectTable[id].vromStart;
        PRINTF("OBJECT[%d] SIZE %fK SEG=%x\n", objectCtx->slots[i].id, size / 1024.0f, objectCtx->slots[i].segment);
        PRINTF("num=%d adrs=%x end=%x\n", objectCtx->numEntries, (uintptr_t)objectCtx->slots[i].segment + size,
               objectCtx->spaceEnd);
        DMA_REQUEST_SYNC(objectCtx->slots[i].segment, gObjectTable[id].vromStart, size, "../z_scene.c", 342);
    }
}

void* func_800982FC(ObjectContext* objectCtx, s32 slot, s16 objectId) {
    ObjectEntry* entry = &objectCtx->slots[slot];
    RomFile* objectFile = &gObjectTable[objectId];
    u32 size;
    void* nextPtr;

    entry->id = -objectId;
    entry->dmaRequest.vromAddr = 0;

    size = objectFile->vromEnd - objectFile->vromStart;
    PRINTF("OBJECT EXCHANGE NO=%2d BANK=%3d SIZE=%8.3fK\n", slot, objectId, size / 1024.0f);

    nextPtr = (void*)ALIGN16((uintptr_t)entry->segment + size);

    ASSERT(nextPtr < objectCtx->spaceEnd, "nextptr < this->endSegment", "../z_scene.c", 381);

    PRINTF(T("オブジェクト入れ替え空きサイズ=%08x\n", "Object exchange free size=%08x\n"),
           (uintptr_t)objectCtx->spaceEnd - (uintptr_t)nextPtr);

    return nextPtr;
}

#if TARGET_PSP
/* Set by Scene_CommandAlternateHeaderList instead of the vanilla trick of
 * writing SCENE_CMD_ID_END into the NEXT command in place
 * ((cmd + 1)->base.code = SCENE_CMD_ID_END). That self-modification is safe
 * on N64, where the scene command stream is a fresh, per-load, writable copy
 * in RAM; this port's native-blob pipeline keeps scene data in memory that is
 * not necessarily re-populated the same way between loads, and other actors
 * (En_Holl, Door_Shutter) read the room's OWN alternate-header list on later
 * room transitions -- the same list a stray leftover END from THIS scene's
 * headers could still be sitting in. A flag checked right after the handler
 * returns has the identical effect (stop dispatching further commands in
 * THIS list) without touching the list's own bytes. Ported from
 * reference/oot-psp-z2442 commit f1ba596b9 ("fix room changes").
 *
 * Saved/restored around the loop, not just set/cleared, because
 * Scene_ExecuteCommands calls itself recursively -- Scene_CommandAlternateHeaderList
 * IS one of the handlers this loop dispatches, and its own inner
 * Scene_ExecuteCommands call must not leave the flag set for the OUTER
 * loop that is still walking the room's alternate-header list itself. */
static s32 sStopSceneCommandsAfterAlternateHeader;
#endif

s32 Scene_ExecuteCommands(PlayState* play, SceneCmd* sceneCmd) {
#if TARGET_PSP
    extern void PspDebugLogSceneCmd(void* addr, unsigned int code, unsigned int data1, unsigned int data2);
    int dbgIter = 0;
    int pspIter = 0; /* always increments, unlike dbgIter which stops at 41 */
    s32 prevStopSceneCommands = sStopSceneCommandsAfterAlternateHeader;

    sStopSceneCommandsAfterAlternateHeader = 0;
#endif
    while (true) {
        u32 cmdCode = sceneCmd->base.code;

#if TARGET_PSP
        /* BOUND. Measured during the forward/backward-movement hang: this loop
         * never reaches SCENE_CMD_ID_END and keeps dispatching handlers off the
         * end of the command list -- Object_SpawnPersistent alone was re-entered
         * from here (Scene_ExecuteCommands+0x88, the handler inlined at -O2)
         * 8.4 million times, driving objectCtx->numEntries to 193 against a
         * 19-slot array before the guard in Object_SpawnPersistent stopped it.
         *
         * This is the same bug class session 9 already hit once: a command
         * stream that is not terminated (there, zeroed display-list stubs) runs
         * forever instead of faulting. A real scene header is a few dozen
         * commands; 4096 is far past any legitimate list, so hitting this means
         * the list is garbage -- record where and stop, rather than hang.
         *
         * NOTE this is containment, not the fix. The open question is why a
         * scene/room whose command list is not the compiled-in hakaana2 one is
         * being executed at all on movement (psp_static_assets.c only registers
         * hakaana2's scene + room 0; anything else falls back to a DMA that has
         * nothing valid behind it). */
        if (++pspIter > 4096) {
            gPspSceneCmdRunaway++;
            gPspSceneCmdRunawayAddr = (u32)sceneCmd;
            gPspSceneCmdRunawayCode = cmdCode;
            break;
        }
        if (dbgIter < 40) {
            PspDebugLogSceneCmd(sceneCmd, cmdCode, sceneCmd->base.data1, sceneCmd->base.data2);
            dbgIter++;
        } else if (dbgIter == 40) {
            PspDebugLogSceneCmd(sceneCmd, 0xDEADBEEF, 0, 0);
            dbgIter++;
        }
#endif

        PRINTF("*** Scene_Word = { code=%d, data1=%02x, data2=%04x } ***\n", cmdCode, sceneCmd->base.data1,
               sceneCmd->base.data2);

        if (cmdCode == SCENE_CMD_ID_END) {
            break;
        }

        if (cmdCode < ARRAY_COUNT(sSceneCmdHandlers)) {
            sSceneCmdHandlers[cmdCode](play, sceneCmd);
#if TARGET_PSP
            if (sStopSceneCommandsAfterAlternateHeader) {
                break;
            }
#endif
        } else {
            PRINTF_COLOR_RED();
            PRINTF(T("code の値が異常です\n", "code variable is abnormal\n"));
            PRINTF_RST();
        }

        sceneCmd++;
    }

#if TARGET_PSP
    sStopSceneCommandsAfterAlternateHeader = prevStopSceneCommands;
#endif
    return 0;
}

BAD_RETURN(s32) Scene_CommandPlayerEntryList(PlayState* play, SceneCmd* cmd) {
    ActorEntry* playerEntry = play->playerEntry =
        (ActorEntry*)SEGMENTED_TO_VIRTUAL(cmd->playerEntryList.data) + play->spawnList[play->spawn].playerEntryIndex;
    s16 linkObjectId;

#if TARGET_PSP
    /* Unlike the room's own actor list (Scene_CommandActorEntryList, fixed
     * above), Player's spawn entry lives in the SCENE's playerEntryList --
     * a separate raw-DMA'd array that was never routed through the same
     * endian fixup, discovered via a real symptom: playerStartBgCamIndex
     * (PLAYER_GET_START_BG_CAM_INDEX, low byte of params) came out as the
     * byte-reversed value of the real params (e.g. real 0x0D00 read back as
     * 0x000D), so the "start with this fixed bg camera" path silently
     * requested a garbage index instead of the intended one. Only the
     * single selected entry needs fixing, not the whole array (its total
     * length isn't tracked here). */
    {
        extern void PspFixupActorEntryListEndian(void* actorEntryList, unsigned int count);
        PspFixupActorEntryListEndian(playerEntry, 1);
    }
#endif

#if TARGET_PSP
    /* Debug A/B for the open "Link's geometry is displaced" bug: render ADULT
     * Link instead of child. Adult is a different object (object_link_boy), a
     * different skeleton (gLinkAdultSkel) and different limb display lists, all
     * through the SAME renderer -- so if adult breaks identically the defect is
     * in the renderer and every model-data theory dies, and if adult is clean
     * it is something about the child assets.
     *
     * This is the right single point: both linkAgeOnLoad (which Player_Init
     * copies back into gSaveContext, z_player.c:12419) and linkObjectId below
     * read gSaveContext.save.linkAge, so overriding it here covers skeleton and
     * object together. It runs on every scene load, so poking the flag and then
     * walking through a scene transition switches age without a rebuild.
     *
     * -1 = leave alone, 0 = LINK_AGE_ADULT, 1 = LINK_AGE_CHILD.
     * NOTE: equipment is not age-swapped here (no Inventory_SwapAgeEquipment),
     * so adult Link may hold child gear or nothing. That is fine for a geometry
     * comparison and deliberately avoided to keep this to one variable. */
    {
        extern int gDebugForceLinkAge;
        if (gDebugForceLinkAge >= 0) {
            gSaveContext.save.linkAge = (u8)gDebugForceLinkAge;
        }
    }
#endif

    play->linkAgeOnLoad = ((void)0, gSaveContext.save.linkAge);

    linkObjectId = gLinkObjectIds[((void)0, gSaveContext.save.linkAge)];

    gActorOverlayTable[playerEntry->id].profile->objectId = linkObjectId;
    Object_SpawnPersistent(&play->objectCtx, linkObjectId);
}

BAD_RETURN(s32) Scene_CommandActorEntryList(PlayState* play, SceneCmd* cmd) {
    play->numActorEntries = cmd->actorEntryList.length;
    play->actorEntryList = SEGMENTED_TO_VIRTUAL(cmd->actorEntryList.data);
#if TARGET_PSP
    {
        extern void PspFixupActorEntryListEndian(void* actorEntryList, unsigned int count);
        PspFixupActorEntryListEndian(play->actorEntryList, play->numActorEntries);
    }
#endif
}

BAD_RETURN(s32) Scene_CommandUnused2(PlayState* play, SceneCmd* cmd) {
    play->unk_11DFC = SEGMENTED_TO_VIRTUAL(cmd->unused02.segment);
}

BAD_RETURN(s32) Scene_CommandCollisionHeader(PlayState* play, SceneCmd* cmd) {
    CollisionHeader* colHeader = SEGMENTED_TO_VIRTUAL(cmd->colHeader.data);

#if TARGET_PSP
    {
        extern void PspFixupCollisionHeaderEndian(void* colHeader);
        PspFixupCollisionHeaderEndian(colHeader);
    }
    { extern void PspDebugLogColHeader(void*, int, void*, void*, void*, void*, void*, void*); PspDebugLogColHeader(cmd, 0, colHeader, NULL, NULL, NULL, NULL, NULL); }
#endif

    colHeader->vtxList = SEGMENTED_TO_VIRTUAL(colHeader->vtxList);
    colHeader->polyList = SEGMENTED_TO_VIRTUAL(colHeader->polyList);
    colHeader->surfaceTypeList = SEGMENTED_TO_VIRTUAL(colHeader->surfaceTypeList);
    colHeader->bgCamList = SEGMENTED_TO_VIRTUAL(colHeader->bgCamList);
    colHeader->waterBoxes = SEGMENTED_TO_VIRTUAL(colHeader->waterBoxes);

#if TARGET_PSP
    {
        extern void PspFixupVtxListEndian(void* vtxList, unsigned int count);
        extern void PspFixupPolyListEndian(void* polyList, unsigned int count);
        PspFixupVtxListEndian(colHeader->vtxList, colHeader->numVertices);
        PspFixupPolyListEndian(colHeader->polyList, colHeader->numPolygons);
    }
    { extern void PspDebugLogColHeader(void*, int, void*, void*, void*, void*, void*, void*); PspDebugLogColHeader(cmd, 1, colHeader, colHeader->vtxList, colHeader->polyList, colHeader->surfaceTypeList, colHeader->bgCamList, colHeader->waterBoxes); }
#endif

#if TARGET_PSP
    {
        extern unsigned int gPspColProbe[8];
        gPspColProbe[0] = (unsigned int)(uintptr_t)cmd->colHeader.data;
        gPspColProbe[1] = colHeader->numVertices;
        gPspColProbe[2] = colHeader->numPolygons;
        gPspColProbe[3] = (unsigned int)(uintptr_t)colHeader->polyList;
        gPspColProbe[4] = (unsigned int)(uintptr_t)colHeader->vtxList;
        gPspColProbe[5] = (unsigned int)(int)colHeader->minBounds.y;
        gPspColProbe[6] = (unsigned int)(int)colHeader->maxBounds.y;
        gPspColProbe[7]++;
    }
#endif

    BgCheck_Allocate(&play->colCtx, play, colHeader);

#if TARGET_PSP
    { extern void PspDebugLogColHeader(void*, int, void*, void*, void*, void*, void*, void*); PspDebugLogColHeader(cmd, 2, NULL, NULL, NULL, NULL, NULL, NULL); }
#endif
}

BAD_RETURN(s32) Scene_CommandRoomList(PlayState* play, SceneCmd* cmd) {
    play->roomList.count = cmd->roomList.length;
    play->roomList.romFiles = SEGMENTED_TO_VIRTUAL(cmd->roomList.data);
#if TARGET_PSP
    {
        extern void PspFixupRomFileListEndian(void* romFileList, unsigned int count);
        PspFixupRomFileListEndian(play->roomList.romFiles, play->roomList.count);
    }
#endif
}

BAD_RETURN(s32) Scene_CommandSpawnList(PlayState* play, SceneCmd* cmd) {
    play->spawnList = SEGMENTED_TO_VIRTUAL(cmd->spawnList.data);
}

BAD_RETURN(s32) Scene_CommandSpecialFiles(PlayState* play, SceneCmd* cmd) {
    if (cmd->specialFiles.keepObjectId != OBJECT_INVALID) {
        play->objectCtx.subKeepSlot = Object_SpawnPersistent(&play->objectCtx, cmd->specialFiles.keepObjectId);
        gSegments[5] = OS_K0_TO_PHYSICAL(play->objectCtx.slots[play->objectCtx.subKeepSlot].segment);
    }

    if (cmd->specialFiles.naviQuestHintFileId != NAVI_QUEST_HINTS_NONE) {
        play->naviQuestHints = Play_LoadFile(play, &sNaviQuestHintFiles[cmd->specialFiles.naviQuestHintFileId - 1]);
    }
}

BAD_RETURN(s32) Scene_CommandRoomBehavior(PlayState* play, SceneCmd* cmd) {
    play->roomCtx.curRoom.type = cmd->roomBehavior.gpFlag1;
    play->roomCtx.curRoom.environmentType = cmd->roomBehavior.gpFlag2 & 0xFF;
    play->roomCtx.curRoom.lensMode = (cmd->roomBehavior.gpFlag2 >> 8) & 1;
    play->msgCtx.disableWarpSongs = (cmd->roomBehavior.gpFlag2 >> 0xA) & 1;
}

BAD_RETURN(s32) Scene_CommandRoomShape(PlayState* play, SceneCmd* cmd) {
    play->roomCtx.curRoom.roomShape = SEGMENTED_TO_VIRTUAL(cmd->mesh.data);
#if TARGET_PSP
    {
        extern void PspFixupRoomShapeEndian(void* roomShape);
        PspFixupRoomShapeEndian(play->roomCtx.curRoom.roomShape);
    }
#endif
}

BAD_RETURN(s32) Scene_CommandObjectList(PlayState* play, SceneCmd* cmd) {
    s32 i;
    s32 j;
    s32 k;
    ObjectEntry* entry;
    ObjectEntry* invalidatedEntry;
    ObjectEntry* entries;
    s16* objectListEntry = SEGMENTED_TO_VIRTUAL(cmd->objectList.data);
    void* nextPtr;

#if TARGET_PSP
    {
        extern void PspFixupS16ArrayEndian(void* data, unsigned int count);
        PspFixupS16ArrayEndian(objectListEntry, (unsigned int)cmd->objectList.length);
    }
#endif

    k = 0;
    i = play->objectCtx.numPersistentEntries;
    entries = play->objectCtx.slots;
    entry = &play->objectCtx.slots[i];

    while (i < play->objectCtx.numEntries) {
        if (entry->id != *objectListEntry) {

            invalidatedEntry = &play->objectCtx.slots[i];
            for (j = i; j < play->objectCtx.numEntries; j++) {
                invalidatedEntry->id = OBJECT_INVALID;
                invalidatedEntry++;
            }

            play->objectCtx.numEntries = i;
            Actor_KillAllWithMissingObject(play, &play->actorCtx);

            continue;
        }

        i++;
        k++;
        objectListEntry++;
        entry++;
    }

    ASSERT(cmd->objectList.length <= ARRAY_COUNT(play->objectCtx.slots),
           "scene_info->object_bank.num <= OBJECT_EXCHANGE_BANK_MAX", "../z_scene.c", 705);

    while (k < cmd->objectList.length) {
        nextPtr = func_800982FC(&play->objectCtx, i, *objectListEntry);
        if (i < (ARRAY_COUNT(play->objectCtx.slots) - 1)) {
            entries[i + 1].segment = nextPtr;
        }
        i++;
        k++;
        objectListEntry++;
    }

    play->objectCtx.numEntries = i;
}

BAD_RETURN(s32) Scene_CommandLightList(PlayState* play, SceneCmd* cmd) {
    s32 i;
    LightInfo* lightInfo = SEGMENTED_TO_VIRTUAL(cmd->lightList.data);

    for (i = 0; i < cmd->lightList.length; i++) {
        LightContext_InsertLight(play, &play->lightCtx, lightInfo);
        lightInfo++;
    }
}

BAD_RETURN(s32) Scene_CommandPathList(PlayState* play, SceneCmd* cmd) {
    play->pathList = SEGMENTED_TO_VIRTUAL(cmd->pathList.data);
}

BAD_RETURN(s32) Scene_CommandTransitionActorEntryList(PlayState* play, SceneCmd* cmd) {
    play->transitionActors.count = cmd->transiActorList.length;
    play->transitionActors.list = SEGMENTED_TO_VIRTUAL(cmd->transiActorList.data);
#if TARGET_PSP
    {
        extern void PspFixupTransitionActorEntryListEndian(void* transitionActorList, unsigned int count);
        PspFixupTransitionActorEntryListEndian(play->transitionActors.list, play->transitionActors.count);
    }
    {
        extern unsigned int gPspTransProbe[8];
        gPspTransProbe[0] = (unsigned int)(uintptr_t)play->transitionActors.list;
        gPspTransProbe[1] = play->transitionActors.count;
        if (play->transitionActors.list != NULL && play->transitionActors.count > 0) {
            TransitionActorEntry* e = &play->transitionActors.list[0];

            gPspTransProbe[2] = (unsigned int)(int)e->id;
            gPspTransProbe[3] = (unsigned int)(int)e->pos.x;
            gPspTransProbe[4] = (unsigned int)(int)e->pos.y;
            gPspTransProbe[5] = (unsigned int)(int)e->pos.z;
            gPspTransProbe[6] = (unsigned int)(int)e->rotY;
            gPspTransProbe[7] = (unsigned int)(unsigned short)e->params;
        }
    }
#endif
}

void Scene_ResetTransitionActorList(GameState* state, TransitionActorList* transitionActors) {
    transitionActors->count = 0;
}

BAD_RETURN(s32) Scene_CommandLightSettingsList(PlayState* play, SceneCmd* cmd) {
    play->envCtx.numLightSettings = cmd->lightSettingList.length;
    play->envCtx.lightSettingsList = SEGMENTED_TO_VIRTUAL(cmd->lightSettingList.data);
}

BAD_RETURN(s32) Scene_CommandSkyboxSettings(PlayState* play, SceneCmd* cmd) {
    play->skyboxId = cmd->skyboxSettings.skyboxId;
    play->envCtx.skyboxConfig = play->envCtx.changeSkyboxNextConfig = cmd->skyboxSettings.skyboxConfig;
    play->envCtx.lightMode = cmd->skyboxSettings.envLightMode;
}

BAD_RETURN(s32) Scene_CommandSkyboxDisables(PlayState* play, SceneCmd* cmd) {
    play->envCtx.skyboxDisabled = cmd->skyboxDisables.skyboxDisabled;
    play->envCtx.sunMoonDisabled = cmd->skyboxDisables.sunMoonDisabled;
}

BAD_RETURN(s32) Scene_CommandTimeSettings(PlayState* play, SceneCmd* cmd) {
    if ((cmd->timeSettings.hour != 0xFF) && (cmd->timeSettings.min != 0xFF)) {
        gSaveContext.skyboxTime = gSaveContext.save.dayTime =
            ((cmd->timeSettings.hour + (cmd->timeSettings.min / 60.0f)) * 60.0f) / ((f32)(24 * 60) / 0x10000);
    }

    if (cmd->timeSettings.timeSpeed != 0xFF) {
        play->envCtx.sceneTimeSpeed = cmd->timeSettings.timeSpeed;
    } else {
        play->envCtx.sceneTimeSpeed = 0;
    }

    if (gSaveContext.sunsSongState == SUNSSONG_INACTIVE) {
        gTimeSpeed = play->envCtx.sceneTimeSpeed;
    }

    play->envCtx.sunPos.x = -(Math_SinS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 120.0f) * 25.0f;
    play->envCtx.sunPos.y = (Math_CosS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 120.0f) * 25.0f;
    play->envCtx.sunPos.z = (Math_CosS(((void)0, gSaveContext.save.dayTime) - CLOCK_TIME(12, 0)) * 20.0f) * 25.0f;

    if (((play->envCtx.sceneTimeSpeed == 0) && (gSaveContext.save.cutsceneIndex < CS_INDEX_0)) ||
        (gSaveContext.save.entranceIndex == ENTR_LAKE_HYLIA_8)) {
#if OOT_VERSION >= PAL_1_0
        gSaveContext.skyboxTime = ((void)0, gSaveContext.save.dayTime);
#endif

#if OOT_VERSION < PAL_1_0
        if ((gSaveContext.skyboxTime > CLOCK_TIME(4, 0)) && (gSaveContext.skyboxTime <= CLOCK_TIME(5, 0))) {
            gSaveContext.skyboxTime = CLOCK_TIME(5, 0) + 1;
        } else if ((gSaveContext.skyboxTime >= CLOCK_TIME(6, 0)) && (gSaveContext.skyboxTime <= CLOCK_TIME(8, 0))) {
            gSaveContext.skyboxTime = CLOCK_TIME(8, 0) + 1;
#else
        if ((gSaveContext.skyboxTime > CLOCK_TIME(4, 0)) && (gSaveContext.skyboxTime < CLOCK_TIME(6, 30))) {
            gSaveContext.skyboxTime = CLOCK_TIME(5, 0) + 1;
        } else if ((gSaveContext.skyboxTime >= CLOCK_TIME(6, 30)) && (gSaveContext.skyboxTime <= CLOCK_TIME(8, 0))) {
            gSaveContext.skyboxTime = CLOCK_TIME(8, 0) + 1;
#endif
        } else if ((gSaveContext.skyboxTime >= CLOCK_TIME(16, 0)) && (gSaveContext.skyboxTime <= CLOCK_TIME(17, 0))) {
            gSaveContext.skyboxTime = CLOCK_TIME(17, 0) + 1;
        } else if ((gSaveContext.skyboxTime >= CLOCK_TIME(18, 0) + 1) &&
                   (gSaveContext.skyboxTime <= CLOCK_TIME(19, 0))) {
            gSaveContext.skyboxTime = CLOCK_TIME(19, 0) + 1;
        }
    }
}

BAD_RETURN(s32) Scene_CommandWindSettings(PlayState* play, SceneCmd* cmd) {
    s8 x = cmd->windSettings.x;
    s8 y = cmd->windSettings.y;
    s8 z = cmd->windSettings.z;

    play->envCtx.windDirection.x = x;
    play->envCtx.windDirection.y = y;
    play->envCtx.windDirection.z = z;

    play->envCtx.windSpeed = cmd->windSettings.unk_07;
}

BAD_RETURN(s32) Scene_CommandExitList(PlayState* play, SceneCmd* cmd) {
    play->exitList = SEGMENTED_TO_VIRTUAL(cmd->exitList.data);
}

BAD_RETURN(s32) Scene_CommandUndefined9(PlayState* play, SceneCmd* cmd) {
}

BAD_RETURN(s32) Scene_CommandSoundSettings(PlayState* play, SceneCmd* cmd) {
    play->sceneSequences.seqId = cmd->soundSettings.seqId;
    play->sceneSequences.natureAmbienceId = cmd->soundSettings.natureAmbienceId;

    if (gSaveContext.seqId == (u8)NA_BGM_DISABLED) {
        SEQCMD_RESET_AUDIO_HEAP(0, cmd->soundSettings.specId);
    }
}

BAD_RETURN(s32) Scene_CommandEchoSettings(PlayState* play, SceneCmd* cmd) {
    play->roomCtx.curRoom.echo = cmd->echoSettings.echo;
}

BAD_RETURN(s32) Scene_CommandAlternateHeaderList(PlayState* play, SceneCmd* cmd) {
    PRINTF("\n[ZU]sceneset age    =[%X]", ((void)0, gSaveContext.save.linkAge));
    PRINTF("\n[ZU]sceneset time   =[%X]", ((void)0, gSaveContext.save.cutsceneIndex));
    PRINTF("\n[ZU]sceneset counter=[%X]", ((void)0, gSaveContext.sceneLayer));

    if (gSaveContext.sceneLayer != SCENE_LAYER_CHILD_DAY) {
        SceneCmd* altHeader = ((SceneCmd**)SEGMENTED_TO_VIRTUAL(cmd->altHeaders.data))[gSaveContext.sceneLayer - 1];

        if (altHeader != NULL) {
            Scene_ExecuteCommands(play, SEGMENTED_TO_VIRTUAL(altHeader));
#if TARGET_PSP
            sStopSceneCommandsAfterAlternateHeader = true;
#else
            (cmd + 1)->base.code = SCENE_CMD_ID_END;
#endif
        } else {
            PRINTF(T("\nげぼはっ！ 指定されたデータがないでええっす！", "\nCoughh! There is no specified dataaaaa!"));

            if (gSaveContext.sceneLayer == SCENE_LAYER_ADULT_NIGHT) {
                // Due to the condition above, this is equivalent to accessing altHeaders[SCENE_LAYER_ADULT_DAY - 1]
                SceneCmd* altHeader = ((SceneCmd**)SEGMENTED_TO_VIRTUAL(
                    cmd->altHeaders
                        .data))[(gSaveContext.sceneLayer - SCENE_LAYER_ADULT_NIGHT) + SCENE_LAYER_ADULT_DAY - 1];

                PRINTF(T("\nそこで、大人の昼データを使用するでええっす！！", "\nUsing adult day data there!!"));

                if (altHeader != NULL) {
                    Scene_ExecuteCommands(play, SEGMENTED_TO_VIRTUAL(altHeader));
#if TARGET_PSP
                    sStopSceneCommandsAfterAlternateHeader = true;
#else
                    (cmd + 1)->base.code = SCENE_CMD_ID_END;
#endif
                }
            }
        }
    }
}

BAD_RETURN(s32) Scene_CommandCutsceneData(PlayState* play, SceneCmd* cmd) {
    PRINTF("\ngame_play->demo_play.data=[%x]", play->csCtx.script);
    play->csCtx.script = SEGMENTED_TO_VIRTUAL(cmd->cutsceneData.data);
}

BAD_RETURN(s32) Scene_CommandMiscSettings(PlayState* play, SceneCmd* cmd) {
    R_SCENE_CAM_TYPE = cmd->miscSettings.sceneCamType;
    gSaveContext.worldMapArea = cmd->miscSettings.area;

    if ((play->sceneId == SCENE_BAZAAR) || (play->sceneId == SCENE_SHOOTING_GALLERY)) {
        if (LINK_AGE_IN_YEARS == YEARS_ADULT) {
            gSaveContext.worldMapArea = WORLD_MAP_AREA_KAKARIKO_VILLAGE;
        }
    }

    if (((play->sceneId >= SCENE_HYRULE_FIELD) && (play->sceneId <= SCENE_OUTSIDE_GANONS_CASTLE)) ||
        ((play->sceneId >= SCENE_MARKET_ENTRANCE_DAY) && (play->sceneId <= SCENE_TEMPLE_OF_TIME_EXTERIOR_RUINS))) {
        if (gSaveContext.save.cutsceneIndex < CS_INDEX_0) {
            gSaveContext.save.info.worldMapAreaData |= gBitFlags[((void)0, gSaveContext.worldMapArea)];
            PRINTF("０００  ａｒｅａ＿ａｒｒｉｖａｌ＝%x (%d)\n", gSaveContext.save.info.worldMapAreaData,
                   ((void)0, gSaveContext.worldMapArea));
        }
    }
}

void Scene_SetTransitionForNextEntrance(PlayState* play) {
    s16 entranceIndex;

    if (!IS_DAY) {
        if (!LINK_IS_ADULT) {
            entranceIndex = play->nextEntranceIndex + SCENE_LAYER_CHILD_NIGHT;
        } else {
            entranceIndex = play->nextEntranceIndex + SCENE_LAYER_ADULT_NIGHT;
        }
    } else {
        if (!LINK_IS_ADULT) {
            entranceIndex = play->nextEntranceIndex + SCENE_LAYER_CHILD_DAY;
        } else {
            entranceIndex = play->nextEntranceIndex + SCENE_LAYER_ADULT_DAY;
        }
    }

    play->transitionType = ENTRANCE_INFO_START_TRANS_TYPE(gEntranceTable[entranceIndex].field);
}

SceneCmdHandlerFunc sSceneCmdHandlers[SCENE_CMD_ID_MAX] = {
    Scene_CommandPlayerEntryList,          // SCENE_CMD_ID_PLAYER_ENTRY_LIST
    Scene_CommandActorEntryList,           // SCENE_CMD_ID_ACTOR_LIST
    Scene_CommandUnused2,                  // SCENE_CMD_ID_UNUSED_2
    Scene_CommandCollisionHeader,          // SCENE_CMD_ID_COLLISION_HEADER
    Scene_CommandRoomList,                 // SCENE_CMD_ID_ROOM_LIST
    Scene_CommandWindSettings,             // SCENE_CMD_ID_WIND_SETTINGS
    Scene_CommandSpawnList,                // SCENE_CMD_ID_SPAWN_LIST
    Scene_CommandSpecialFiles,             // SCENE_CMD_ID_SPECIAL_FILES
    Scene_CommandRoomBehavior,             // SCENE_CMD_ID_ROOM_BEHAVIOR
    Scene_CommandUndefined9,               // SCENE_CMD_ID_UNDEFINED_9
    Scene_CommandRoomShape,                // SCENE_CMD_ID_ROOM_SHAPE
    Scene_CommandObjectList,               // SCENE_CMD_ID_OBJECT_LIST
    Scene_CommandLightList,                // SCENE_CMD_ID_LIGHT_LIST
    Scene_CommandPathList,                 // SCENE_CMD_ID_PATH_LIST
    Scene_CommandTransitionActorEntryList, // SCENE_CMD_ID_TRANSITION_ACTOR_LIST
    Scene_CommandLightSettingsList,        // SCENE_CMD_ID_LIGHT_SETTINGS_LIST
    Scene_CommandTimeSettings,             // SCENE_CMD_ID_TIME_SETTINGS
    Scene_CommandSkyboxSettings,           // SCENE_CMD_ID_SKYBOX_SETTINGS
    Scene_CommandSkyboxDisables,           // SCENE_CMD_ID_SKYBOX_DISABLES
    Scene_CommandExitList,                 // SCENE_CMD_ID_EXIT_LIST
    NULL,                                  // SCENE_CMD_ID_END
    Scene_CommandSoundSettings,            // SCENE_CMD_ID_SOUND_SETTINGS
    Scene_CommandEchoSettings,             // SCENE_CMD_ID_ECHO_SETTINGS
    Scene_CommandCutsceneData,             // SCENE_CMD_ID_CUTSCENE_DATA
    Scene_CommandAlternateHeaderList,      // SCENE_CMD_ID_ALTERNATE_HEADER_LIST
    Scene_CommandMiscSettings,             // SCENE_CMD_ID_MISC_SETTINGS
};

RomFile sNaviQuestHintFiles[] = {
    ROM_FILE(elf_message_field),
    ROM_FILE(elf_message_ydan),
};
