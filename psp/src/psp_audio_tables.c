/* Hand-built stand-in for src/audio/tables/{sequence,soundfont,samplebank}_table.c
 * -- same shortcut this port already took for gGameStateOverlayTable
 * (psp/src/gamestate_table_psp.c) instead of the real, macro-driven
 * z_game_dlftbls.c: those three real files are driven by
 * include/tables/sequence_table.h, generated (via tools/audio/atblgen) from
 * the FULL game's ~105 sequences/38 soundfonts/6 sample banks. Phase 1
 * bring-up scope is deliberately just Soundfont_0 + Soundfont_1 (sharing
 * SampleBank_0), Sequence_0 (the SFX-id meta-sequence session_init.c's
 * PERMANENT_POOL_SIZE hardcodes needing) and Sequence_72 / NA_BGM_OCA_TIME
 * (Song of Time, an audible smoke test). Full asset coverage is real
 * follow-up work, not this file.
 *
 * romAddr fields below are NOT linker symbols -- unlike scene/room data,
 * soundfont/sample bank internal cross-references are meant to be plain
 * integers relative to wherever a DMA lands them in RAM, fixed up into real
 * pointers only afterward by the real engine code (AudioLoad_RelocateFont,
 * src/audio/internal/load.c) -- confirmed by reading that function, this is
 * the intentional wire format, identical to what real N64 hardware DMAs out
 * of Audiobank/Audioseq/Audiotable. So instead of linking the compiled
 * objects, their payload is objcopy'd to a flat binary and served through
 * the existing blob mechanism (psp_blob_assets.c) via psp/src/libultra/
 * os_pi.c's osEPiStartDma, using a fake-but-real "ROM address" -- these
 * AUDIO_BLOB_VROM_* literals MUST match Makefile.psp's AUDIO_BLOB_VROM_*
 * values exactly (picked from a range no real pal-1.0 vrom offset can ever
 * land in, so collision with a scene/room blob is structurally impossible).
 * Sizes are the real compiled sizes (see Makefile.psp's afile_sizes-
 * generated soundfont_sizes.h/sequence_sizes.h, and SampleBank_0.s's own
 * SampleBank_0_Size -- 0x3EB2A0 confirmed by direct build this session).
 */
#include "attributes.h"
#include "audio.h"

#define AUDIO_BLOB_VROM_SOUNDFONT_0  0x20000000
#define AUDIO_BLOB_VROM_SOUNDFONT_1  0x20100000
#define AUDIO_BLOB_VROM_SAMPLEBANK_0 0x20200000
#define AUDIO_BLOB_VROM_SEQ_0        0x20600000
#define AUDIO_BLOB_VROM_SEQ_72       0x20700000

#pragma weak gSequenceTable = sSequenceTableStorage
#pragma weak gSoundFontTable = sSoundFontTableStorage
#pragma weak gSampleBankTable = sSampleBankTableStorage

/* Sequence_72 is NA_BGM_OCA_TIME = 0x48 = 72 (audio_ids.py's own table,
 * cross-checked against its position in include/tables/sequence_table.h) --
 * the table is real seqId-indexed, so it has to be at least 73 entries long
 * for that index to mean anything. Every index but 0 and 72 is a real,
 * safe "pointer" entry (size == 0), the same convention
 * DEFINE_SEQUENCE_PTR uses -- AudioLoad_GetRealTableIndex redirects through
 * entries[romAddr] whenever entries[id].size == 0. medium/cachePolicy on
 * these still have to be real (AudioLoad_SyncLoad reads them from the
 * ORIGINAL id, only size/romAddr come from the redirected realId) --
 * matching Sequence_0's, since that's what every unused slot aliases to.
 */
#define NUM_SEQ_ENTRIES 73

/* The real AudioTable (include/audio.h) is ONE struct -- a header
 * immediately followed by its entries -- because every real access goes
 * through `table->entries[i]` pointer arithmetic off a single AudioTable*.
 * An EARLIER version of this file declared the header and the entries
 * array as two independent globals and relied on them landing adjacent in
 * memory by coincidence. They don't: a struct with a non-zero initializer
 * (the header) is placed in .data, while a same-named-but-separate array
 * with no initializer (zero-filled at runtime by PspAudioTables_Init) is
 * placed in .bss -- two different sections, never adjacent, confirmed via
 * psp-nm to be ~0xB2000 bytes apart in the actual build. Every
 * `table->entries[i]` read in the real engine (AudioLoad_SyncLoad,
 * AudioLoad_InitTable, etc.) was therefore reading unrelated linked data
 * instead of our table, silently -- the underlying cause of every sequence/
 * font/sample-bank load failing forever (confirmed live via the PPSSPP
 * debugger: entries[72] read back as an unrelated garbage pattern, and
 * gAudioCtx.permanentPool.numEntries stayed 0 the entire session because
 * AudioHeap_Alloc was never even reached). Declaring one real struct per
 * table, mirroring AudioTable's own layout, is what actually guarantees
 * the contiguity `table->entries[i]` depends on. */
typedef struct {
    AudioTableHeader header;
    AudioTableEntry entries[NUM_SEQ_ENTRIES];
} SeqTableStorage;

typedef struct {
    AudioTableHeader header;
    AudioTableEntry entries[2];
} FontTableStorage;

typedef struct {
    AudioTableHeader header;
    AudioTableEntry entries[1];
} SampleBankTableStorage;

NO_REORDER SeqTableStorage sSequenceTableStorage = {
    { NUM_SEQ_ENTRIES, 0, 0x00000000, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { { 0 } },
};

NO_REORDER FontTableStorage sSoundFontTableStorage = {
    { 2, 0, 0x00000000, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { { 0 } },
};

NO_REORDER SampleBankTableStorage sSampleBankTableStorage = {
    { 1, 0, 0x00000000, { 0, 0, 0, 0, 0, 0, 0, 0 } },
    { { 0 } },
};

#define sSequenceTableEntries  sSequenceTableStorage.entries
#define sSoundFontTableEntries sSoundFontTableStorage.entries
#define sSampleBankTableEntries sSampleBankTableStorage.entries

/* Hand-built stand-in for the real gSequenceFontTable (assets/audio/
 * sequence_font_table.s, generated by "tools/audio/atblgen --sequences" from
 * every sequence in the full game) -- AudioLoad_GetFontsForSequence (src/
 * audio/internal/load.c) reads it as: a u16 per seqId (byte offset into this
 * SAME array), then at that offset a [numFonts, font0, font1, ...] u8 tuple.
 * Real N64 compiles this in directly (no DMA/relocation, unlike soundfonts --
 * gAudioCtx.sequenceFontTable = gSequenceFontTable is a plain pointer
 * assignment, src/audio/internal/load.c), so a plain global is the correct
 * real mechanism here, not a shortcut.
 *
 * Layout: bytes [0,146) = 73 u16 offsets (one per seqId, matching
 * NUM_SEQ_ENTRIES). Sequence_0 uses fonts {Soundfont_1, Soundfont_0} (see
 * assets/audio/sequences/seq_0.prg.seq's #includes); Sequence_72 uses only
 * {Soundfont_0}. Every other seqId in [1,72] points at a shared "zero fonts"
 * tuple -- safe (AudioLoad_GetFontsForSequence returns NULL), but ONLY
 * within this bring-up's own scope (nothing here yet drives a seqId outside
 * gSequenceTable's own 73 entries -- see that table's own comment). */
#define SEQ_FONT_TABLE_SIZE 152
u8 gSequenceFontTable[SEQ_FONT_TABLE_SIZE];

void PspAudioTables_Init(void) {
    s32 i;

    for (i = 0; i < NUM_SEQ_ENTRIES; i++) {
        ((u16*)gSequenceFontTable)[i] = 151; // shared "zero fonts" tuple, see below
    }
    ((u16*)gSequenceFontTable)[0] = 146;
    ((u16*)gSequenceFontTable)[72] = 149;
    gSequenceFontTable[146] = 2;
    gSequenceFontTable[147] = 1; // Soundfont_1
    gSequenceFontTable[148] = 0; // Soundfont_0
    gSequenceFontTable[149] = 1;
    gSequenceFontTable[150] = 0; // Soundfont_0
    gSequenceFontTable[151] = 0; // numFonts = 0

    for (i = 0; i < NUM_SEQ_ENTRIES; i++) {
        sSequenceTableEntries[i] = (AudioTableEntry){ 0, 0, MEDIUM_CART, CACHE_LOAD_PERMANENT, 0, 0, 0 };
    }
    // Sequence_0 / NA_BGM_GENERAL_SFX (real medium/cachePolicy from sequence_table.h)
    sSequenceTableEntries[0] =
        (AudioTableEntry){ AUDIO_BLOB_VROM_SEQ_0, 0x6A90, MEDIUM_CART, CACHE_LOAD_PERMANENT, 0, 0, 0 };
    // Sequence_72 / NA_BGM_OCA_TIME (Song of Time)
    sSequenceTableEntries[72] =
        (AudioTableEntry){ AUDIO_BLOB_VROM_SEQ_72, 0x130, MEDIUM_CART, CACHE_LOAD_PERSISTENT, 0, 0, 0 };

    // Soundfont_0: SampleBank_0 normal, no DD bank (0xFF = none), 92 instruments / 4 drums / 136 sfx
    sSoundFontTableEntries[0] = (AudioTableEntry){
        AUDIO_BLOB_VROM_SOUNDFONT_0, 0x3AA0, MEDIUM_CART, CACHE_LOAD_PERMANENT, (0 << 8) | 0xFF, (92 << 8) | 4, 136
    };
    // Soundfont_1: SampleBank_0 normal, no DD bank, 51 instruments / 1 drum / 41 sfx
    sSoundFontTableEntries[1] = (AudioTableEntry){
        AUDIO_BLOB_VROM_SOUNDFONT_1, 0x17B0, MEDIUM_CART, CACHE_LOAD_PERMANENT, (0 << 8) | 0xFF, (51 << 8) | 1, 41
    };

    sSampleBankTableEntries[0] =
        (AudioTableEntry){ AUDIO_BLOB_VROM_SAMPLEBANK_0, 0x3EB2A0, MEDIUM_CART, CACHE_LOAD_EITHER_NOSYNC, 0, 0, 0 };
}
