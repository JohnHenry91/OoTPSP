#!/usr/bin/env python3
"""Generate the PSP audio tables and the ranged-blob registry from the game's
own table headers.

The real decomp tables (src/audio/tables/*.c) cannot be used on PSP: their
romAddr fields are `(u32)Name_Start` linker symbols, which only mean anything
because the N64 build LINKS every soundfont/sequence/sample bank into the ROM's
Audiobank/Audioseq/Audiotable segments. This port serves them as separate blob
files instead, so it needs the same tables with its own addresses -- but
derived from the same headers rather than hand-maintained, so a wrong entry
cannot be introduced by transcription.

One script owns the address assignment because two consumers have to agree on
it exactly: the C table the engine reads, and the blob registry PspBlob_Read
looks addresses up in. They are emitted together, from the same data.

Addresses sit far above any real pal-1.0 ROM offset (the ROM is ~55 MB, well
under 0x20000000), so collision with a scene/room blob is impossible by
construction. Each class gets a fixed stride rather than being packed by size:
that keeps an asset's address stable when an unrelated asset changes size,
which matters because a stale blobs/ directory would otherwise be served
against a freshly generated table. The stride is checked against the real file
size below, so outgrowing it is a build error, never a silent truncation.
"""
import argparse
import os
import re
import sys

SEQ_BASE, SEQ_STRIDE = 0x20000000, 0x00040000
FONT_BASE, FONT_STRIDE = 0x24000000, 0x00040000
BANK_BASE, BANK_STRIDE = 0x28000000, 0x00800000


def parse_includes(path):
    """The generated table headers interleave #include lines with their macro
    lines -- each soundfont's own header is what defines the SFn_NUM_* counts
    the macro arguments then reference. Carry them across verbatim so the
    generated table keeps tracking those headers instead of freezing a copy of
    the numbers."""
    out = []
    with open(path) as f:
        for line in f:
            if line.lstrip().startswith('#include'):
                out.append(line.rstrip())
    return out


def parse_defines(path, macro, argc):
    """Yield the argument lists of every `macro(...)` line, in file order."""
    out = []
    pattern = re.compile(r'^\s*' + macro + r'\s*\(([^)]*)\)')
    with open(path) as f:
        for line in f:
            if line.lstrip().startswith('*') or line.lstrip().startswith('/*'):
                continue
            m = pattern.match(line)
            if m:
                args = [a.strip() for a in m.group(1).split(',')]
                if len(args) != argc:
                    raise SystemExit(f"{path}: expected {argc} args, got {len(args)}: {line.strip()}")
                out.append(args)
    return out


def parse_sequence_table(path):
    """Sequences need both macros kept in one ordered list: table index is
    position in the file, and a PTR entry is a real, load-bearing redirect."""
    entries = []
    seq_re = re.compile(r'^\s*DEFINE_SEQUENCE\s*\(([^)]*)\)')
    ptr_re = re.compile(r'^\s*DEFINE_SEQUENCE_PTR\s*\(([^)]*)\)')
    with open(path) as f:
        for line in f:
            m = ptr_re.match(line)
            if m:
                a = [x.strip() for x in m.group(1).split(',')]
                entries.append(('ptr', a))
                continue
            m = seq_re.match(line)
            if m:
                a = [x.strip() for x in m.group(1).split(',')]
                entries.append(('seq', a))
    return entries


def blob_size(blob_dir, name):
    p = os.path.join(blob_dir, name + '.bin')
    if not os.path.exists(p):
        raise SystemExit(f"missing blob {p} -- build it before generating the tables")
    return os.path.getsize(p)


def seq_blob_name(build_dir, index):
    """Sequence_N is built from either seq_N.s or seq_N.prg.s."""
    for cand in (f"seq_{index}.prg", f"seq_{index}"):
        if os.path.exists(os.path.join(build_dir, 'sequences', cand + '.s')):
            return cand
    raise SystemExit(f"no built .s for Sequence_{index}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seq-table', required=True)
    ap.add_argument('--font-table', required=True)
    ap.add_argument('--bank-table', required=True)
    ap.add_argument('--audio-build-dir', required=True)
    ap.add_argument('--blob-dir')
    ap.add_argument('--out-c')
    ap.add_argument('--out-reg')
    ap.add_argument('--list-blobs', action='store_true',
                    help='print the blob basenames this table needs, for the Makefile')
    args = ap.parse_args()

    seqs = parse_sequence_table(args.seq_table)
    fonts = parse_defines(args.font_table, 'DEFINE_SOUNDFONT', 8)
    banks = []
    for a in parse_defines(args.bank_table, 'DEFINE_SAMPLE_BANK', 3):
        banks.append(('bank', a))
    # DEFINE_SAMPLE_BANK_PTR shares the file and the index space, so re-read in
    # order rather than concatenating the two lists.
    banks = []
    with open(args.bank_table) as f:
        for line in f:
            m = re.match(r'^\s*DEFINE_SAMPLE_BANK_PTR\s*\(([^)]*)\)', line)
            if m:
                banks.append(('ptr', [x.strip() for x in m.group(1).split(',')]))
                continue
            m = re.match(r'^\s*DEFINE_SAMPLE_BANK\s*\(([^)]*)\)', line)
            if m:
                banks.append(('bank', [x.strip() for x in m.group(1).split(',')]))

    seq_blobs = {}
    for i, (kind, a) in enumerate(seqs):
        if kind == 'seq':
            seq_blobs[i] = seq_blob_name(args.audio_build_dir, int(a[0].split('_')[1]))

    if args.list_blobs:
        for i in sorted(seq_blobs):
            print(seq_blobs[i])
        for a in fonts:
            print(a[0])
        for kind, a in banks:
            if kind == 'bank':
                print(a[0])
        return

    reg = []
    c_seq, c_font, c_bank = [], [], []

    def place(base, stride, index, name, cls):
        addr = base + index * stride
        size = blob_size(args.blob_dir, name)
        if size > stride:
            raise SystemExit(f"{cls} {name} is {size} bytes, over the {stride}-byte stride")
        reg.append(f'    {{ {addr:#010x}u, {size}u, "blobs/{name}.bin" }},')
        return addr, size

    for i, (kind, a) in enumerate(seqs):
        if kind == 'ptr':
            c_seq.append(f'    {{ (u32)({a[0]}), 0, {a[2]}, {a[3]}, 0, 0, 0 }},')
        else:
            addr, size = place(SEQ_BASE, SEQ_STRIDE, i, seq_blobs[i], 'sequence')
            c_seq.append(f'    {{ {addr:#010x}, {size:#x}, {a[2]}, {a[3]}, 0, 0, 0 }},')

    for i, a in enumerate(fonts):
        addr, size = place(FONT_BASE, FONT_STRIDE, i, a[0], 'soundfont')
        c_font.append(f'    {{ {addr:#010x}, {size:#x}, {a[1]}, {a[2]}, '
                      f'(({a[3]}) << 8) | ({a[4]}), (({a[5]}) << 8) | ({a[6]}), ({a[7]}) }},')

    for i, (kind, a) in enumerate(banks):
        if kind == 'ptr':
            c_bank.append(f'    {{ ({a[0]}), 0, {a[1]}, {a[2]}, 0, 0, 0 }},')
        else:
            addr, size = place(BANK_BASE, BANK_STRIDE, i, a[0], 'sample bank')
            c_bank.append(f'    {{ {addr:#010x}, {size:#x}, {a[1]}, {a[2]}, 0, 0, 0 }},')

    with open(args.out_reg, 'w') as f:
        f.write('\n'.join(reg) + '\n')

    with open(args.out_c, 'w') as f:
        f.write(HEADER)
        for inc in parse_includes(args.font_table):
            f.write(inc + '\n')
        f.write('\n')
        f.write(f'#define NUM_SEQ_ENTRIES {len(seqs)}\n')
        f.write(f'#define NUM_FONT_ENTRIES {len(fonts)}\n')
        f.write(f'#define NUM_BANK_ENTRIES {len(banks)}\n\n')
        f.write(BODY_TOP)
        f.write('\nstatic const AudioTableEntry sSeqInit[NUM_SEQ_ENTRIES] = {\n')
        f.write('\n'.join(c_seq) + '\n};\n')
        f.write('\nstatic const AudioTableEntry sFontInit[NUM_FONT_ENTRIES] = {\n')
        f.write('\n'.join(c_font) + '\n};\n')
        f.write('\nstatic const AudioTableEntry sBankInit[NUM_BANK_ENTRIES] = {\n')
        f.write('\n'.join(c_bank) + '\n};\n')
        f.write(BODY_BOTTOM)


HEADER = '''/* GENERATED by psp/tools/gen_audio_tables.py -- do not hand-edit.
 *
 * Stands in for src/audio/tables/{sequence,soundfont,samplebank}_table.c,
 * whose romAddr fields are `(u32)Name_Start` linker symbols that only mean
 * something because the N64 build links every asset into the ROM's
 * Audiobank/Audioseq/Audiotable segments. This port serves them as blob files,
 * so the addresses are blob addresses -- but the mediums, cache policies,
 * sample-bank ids and instrument counts all come from the game's own headers,
 * so they cannot drift from what the engine expects.
 *
 * romAddrs are stored SEGMENT-RELATIVE because AudioLoad_InitTable adds the
 * segment ROM start back onto every MEDIUM_CART entry exactly once at init;
 * the subtraction has to happen at runtime because PSP's module loader
 * relocates those symbols. See psp/docs/AUDIO_N64_VS_PSP.md section 3.
 */
#include "attributes.h"
#include "audio.h"
#include "segment_symbols.h"
#include "sequence.h"
#include "sfx.h"

'''

BODY_TOP = '''#pragma weak gSequenceTable = sSequenceTableStorage
#pragma weak gSoundFontTable = sSoundFontTableStorage
#pragma weak gSampleBankTable = sSampleBankTableStorage

/* One struct per table, header immediately followed by its entries: every
 * engine access is `table->entries[i]` off a single AudioTable*, so header and
 * entries must be contiguous. Splitting them into separate globals puts them
 * in different sections, far apart, and every lookup silently reads unrelated
 * memory -- a bug this port has already paid for once. */
typedef struct {
    AudioTableHeader header;
    AudioTableEntry entries[NUM_SEQ_ENTRIES];
} SeqTableStorage;

typedef struct {
    AudioTableHeader header;
    AudioTableEntry entries[NUM_FONT_ENTRIES];
} FontTableStorage;

typedef struct {
    AudioTableHeader header;
    AudioTableEntry entries[NUM_BANK_ENTRIES];
} BankTableStorage;

NO_REORDER SeqTableStorage sSequenceTableStorage = {
    { NUM_SEQ_ENTRIES, 0, 0x00000000, { 0, 0, 0, 0, 0, 0, 0, 0 } }, { { 0 } },
};
NO_REORDER FontTableStorage sSoundFontTableStorage = {
    { NUM_FONT_ENTRIES, 0, 0x00000000, { 0, 0, 0, 0, 0, 0, 0, 0 } }, { { 0 } },
};
NO_REORDER BankTableStorage sSampleBankTableStorage = {
    { NUM_BANK_ENTRIES, 0, 0x00000000, { 0, 0, 0, 0, 0, 0, 0, 0 } }, { { 0 } },
};
'''

BODY_BOTTOM = '''
/* romAddr is stored ABSOLUTE above (a real compile-time constant) and made
 * segment-relative here, because AudioLoad_InitTable adds the segment ROM
 * start back onto every entry exactly once at init. The subtraction cannot be
 * a static initializer: the segment symbols are relocated by PSP's module
 * loader, so their value is not known until runtime.
 *
 * The guard mirrors AudioLoad_InitTable's own condition exactly -- it only
 * adjusts entries with size != 0 and medium == MEDIUM_CART. A pointer entry
 * (size == 0) carries a table INDEX in romAddr, not an address, and adjusting
 * it would silently redirect to the wrong asset.
 *
 * Must run before AudioLoad_Init; psp/src/main.c calls it before Main(). */
static void PspAudioTables_Relocate(AudioTableEntry* dst, const AudioTableEntry* src, s32 count, u32 segmentRomStart) {
    s32 i;

    for (i = 0; i < count; i++) {
        dst[i] = src[i];
        if ((src[i].size != 0) && (src[i].medium == MEDIUM_CART)) {
            dst[i].romAddr -= segmentRomStart;
        }
    }
}

void PspAudioTables_Init(void) {
    PspAudioTables_Relocate(sSequenceTableStorage.entries, sSeqInit, NUM_SEQ_ENTRIES,
                            (u32)_AudioseqSegmentRomStart);
    PspAudioTables_Relocate(sSoundFontTableStorage.entries, sFontInit, NUM_FONT_ENTRIES,
                            (u32)_AudiobankSegmentRomStart);
    PspAudioTables_Relocate(sSampleBankTableStorage.entries, sBankInit, NUM_BANK_ENTRIES,
                            (u32)_AudiotableSegmentRomStart);
}
'''

if __name__ == '__main__':
    main()
