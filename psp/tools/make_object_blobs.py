#!/usr/bin/env python3
"""Build native-endian, segment-addressed OBJECT blobs from the decomp's own C
asset sources -- the same trick psp/tools/make_scene_blob.sh already uses for
scenes and rooms, applied to the object bank.

WHY THIS EXISTS
---------------
Enabling all 426 actors means resolving ~2300 asset symbols (gZoraSkel,
gDodongoIdleAnim, ...). The port resolved those by adding the object's .c to
Makefile.psp, i.e. by linking the object's DATA into the EBOOT. That does not
scale: the game's 381 objects are 9.8 MB of data, and a PSP-1000 has ~24 MB of
user RAM to hold the module, the 1000 KB object bank, the ~1.8 MB Zelda arena
and the audio heap. Linking all of it in is not a tuning problem, it does not
fit.

The console does not hold them all either -- it DMAs one object at a time into
the object bank and points segment 6 at it. So do that. Each object is linked
at its N64 SEGMENT base (0x06000000 for nearly all of them, 0x04000000 for
gameplay_keep, 0x05000000 for the two field/dungeon keeps) and objcopy'd to a
flat binary. Every internal pointer in the result is then a real segment
address, exactly what SEGMENTED_TO_VIRTUAL expects, and the blob is position
independent -- it can be loaded anywhere in the object bank.

The actor's reference to `gZoraSkel` is satisfied the same way: the link tells
us that symbol sits at 0x0600ABCD inside the blob, and we emit that as an
absolute symbol. At draw time gfx_pc's seg_addr() resolves it against
gSegments[6], which Object_UpdateEntries already points at the loaded blob.

SPEC IS THE SOURCE OF TRUTH
---------------------------
File order and segment number come from the decomp's own `spec` (preprocessed
to build/pal-1.0/spec by the N64 Makefile), not from a directory listing. Order
matters: a blob whose layout differs from the ROM file's is a different size,
and the engine reads exactly vromEnd - vromStart bytes into an allocation of
that size. Every blob's size is therefore checked against the ROM's, and a
mismatch is reported rather than silently shipped.

Outputs (all generated, none hand-edited):
  <out>/<object>.bin            the blob itself, shipped on the memory stick
  <out>/objects.reg             registry fragment for psp_blob_assets.c
  <out>/objects_syms.c          absolute symbol definitions, compiled into the EBOOT

Usage: make_object_blobs.py [--jobs N] [--only NAME[,NAME...]] <out-dir>
"""

import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Same value, same reason, as psp/src/segment_roms_psp.c: PSP's module loader
# rebases every relocated word, including ones targeting an absolute symbol.
# Pre-subtracting the load base makes that rebase cancel out exactly.
PSP_MODULE_BASE = 0x08804000

SEGMENT_BASE = {4: 0x04000000, 5: 0x05000000, 6: 0x06000000, 8: 0x08000000}

CFLAGS = (
    "-O2 -G0 -w -fno-strict-aliasing -fwrapv -D_PSP_FW_VERSION=600 "
    "-DPLATFORM_N64=1 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 -DOOT_VERSION=PAL_1_0 "
    "-DDEBUG_FEATURES=0 -DNDEBUG -DTARGET_PSP=1 -DNON_MATCHING=1 -DAVOID_UB=1 "
    "-DF3DEX_GBI_2 -DMML_VERSION=MML_VERSION_OOT -include libc/assert.h "
    # Keeps all-zero arrays in .data. Without it they land in .bss, which is
    # NOBITS and contributes nothing to an objcopy'd flat binary -- the blob
    # would be short and every pointer past that point wrong. Same reason
    # make_scene_blob.sh carries it.
    "-fno-zero-initialized-in-bss"
).split()

INCS = [
    "-Ipsp/include", "-Ipsp/include/gfx", "-Iinclude", "-Isrc", "-I.",
    "-Iextracted/pal-1.0", "-Ibuild/pal-1.0",
]

CC = os.environ.get("CC", "psp-gcc")
LD = os.environ.get("LD", "psp-ld")
OBJCOPY = os.environ.get("OBJCOPY", "psp-objcopy")
NM = os.environ.get("NM", "psp-nm")


def parse_spec(path):
    """Yield (name, [source paths], segment number) for every object segment.

    The preprocessed spec names .o files under build/pal-1.0/; the matching
    source is the same relative path with .c, in either the hand-decompiled
    tree (assets/) or the extracted one (extracted/pal-1.0/assets/).
    """
    segs = []
    name = None
    includes = []
    number = None
    for line in open(path):
        line = line.strip()
        if line == "beginseg":
            name, includes, number = None, [], None
        elif line.startswith('name "'):
            name = line[6:-1]
        elif line.startswith('include "'):
            includes.append(line[9:-1])
        elif line.startswith("number "):
            number = int(line.split()[1])
        elif line == "endseg":
            if name and includes and number in SEGMENT_BASE:
                segs.append((name, includes, number))
    return segs


def obj_to_source(o):
    """build/pal-1.0/assets/objects/X/Y.o -> the .c that produces it."""
    rel = o.replace("build/pal-1.0/", "", 1)
    if not rel.endswith(".o"):
        return None
    c = rel[:-2] + ".c"
    for cand in (os.path.join(ROOT, c),
                 os.path.join(ROOT, "extracted", "pal-1.0", c)):
        if os.path.isfile(cand):
            return cand
    return None


def rom_sizes():
    """{segment name: (vromStart, size)} from the generated ROM-offset file.

    Values there are stored pre-biased by -PSP_MODULE_BASE (see that file's
    header); add the base back to get the true ROM offsets, which is what the
    blob registry keys on.
    """
    txt = open(os.path.join(ROOT, "psp/src/segment_roms_psp.c")).read()
    marks = {}
    for m in re.finditer(r"_([A-Za-z0-9_]+)SegmentRom(Start|End) = (0x[0-9a-fA-F]+)", txt):
        marks.setdefault(m.group(1), {})[m.group(2)] = \
            (int(m.group(3), 16) + PSP_MODULE_BASE) & 0xFFFFFFFF
    out = {}
    for k, v in marks.items():
        if "Start" in v and "End" in v:
            out[k] = (v["Start"], v["End"] - v["Start"])
    return out


def build_one(name, sources, number, outdir, roms, defsyms=None):
    base = SEGMENT_BASE[number]
    tmp = tempfile.mkdtemp(prefix="objblob-")
    try:
        objs = []
        for i, src in enumerate(sources):
            o = os.path.join(tmp, "%03d.o" % i)
            r = subprocess.run([CC] + CFLAGS + INCS + ["-c", src, "-o", o],
                               cwd=ROOT, capture_output=True, text=True)
            if r.returncode != 0:
                return name, None, "compile %s: %s" % (
                    os.path.basename(src), r.stderr.strip().splitlines()[-1:] or "")
            objs.append(o)

        # Undefined references, read from the OBJECT FILES. Reading them from
        # the linked ELF instead does not work and does not fail loudly:
        # --unresolved-symbols=ignore-all resolves them to 0 and removes the
        # `U` rows entirely, so nm reports a clean link over data that now
        # holds NULL pointers. Measured, not assumed.
        r = subprocess.run([NM] + objs, cwd=ROOT, capture_output=True, text=True)
        undef = []
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == "U":
                undef.append(parts[1])

        # One section at the segment base, contents in spec order. Keeping the
        # order is what makes the blob the same size and layout as the ROM file.
        ld = os.path.join(tmp, "seg.ld")
        with open(ld, "w") as f:
            f.write("SECTIONS {\n  .seg 0x%08X : {\n" % base)
            for o in objs:
                # 16-byte align every file's contribution, and the segment end.
                #
                # Not cosmetic, and not guessed: the N64 build's own linker does
                # this, and without it the blob comes out SHORTER than the ROM
                # file the engine sizes its buffer from -- 8 bytes for a
                # single-file object like object_link_child, 272 for
                # gameplay_keep's 276 files. The engine then reads the ROM's
                # length, the tail is zero-filled, and real data at the end of
                # the segment is lost.
                #
                # The alignment matters in its own right too: the PSP GE wants
                # texture data 16-byte aligned, so a texture that lands at an
                # odd offset inside the blob is not merely misplaced, it is
                # read wrongly.
                #
                # This is checkable rather than hopeful -- every blob's size is
                # compared against the ROM's, and the report at the end names
                # any that still differ.
                f.write("    . = ALIGN(16);\n")
                f.write('    "%s"(.data .data.* .rodata .rodata.* .text)\n' % o)
            f.write("    . = ALIGN(16);\n")
            f.write("  }\n  /DISCARD/ : {\n"
                    "    *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.mdebug*)\n"
                    "    *(.comment) *(.gcc_compiled*) *(.note.*)\n  }\n}\n")

        # PASS 2 supplies --defsym for every symbol this object references but
        # another object defines (object_mori_objects -> object_mori_tex, the
        # ~180 references into gameplay_keep, ...). Those values come from
        # pass 1's link of the OTHER object, which is safe to do in two passes
        # because an object's own layout does not depend on them: an undefined
        # reference is a relocation slot, not storage.
        #
        # Without this the link still "succeeds" -- and writes NULL into every
        # one of those slots. See the note above nm-on-object-files.
        args = [LD, "-T", ld, "--unresolved-symbols=ignore-all"]
        if defsyms:
            for sym, addr in defsyms:
                args += ["--defsym", "%s=0x%08X" % (sym, addr)]
        elf = os.path.join(tmp, "link.elf")
        r = subprocess.run(args + ["-o", elf], cwd=ROOT,
                           capture_output=True, text=True)
        if r.returncode != 0:
            return name, None, "link: " + r.stderr.strip()

        binpath = os.path.join(outdir, name + ".bin")
        r = subprocess.run([OBJCOPY, "-O", "binary", "--only-section=.seg",
                            elf, binpath], cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            return name, None, "objcopy: " + r.stderr.strip()

        # Every global symbol and where it landed, for the absolute-symbol file.
        r = subprocess.run([NM, elf], cwd=ROOT, capture_output=True, text=True)
        syms = []
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in ("D", "R", "T"):
                syms.append((parts[2], int(parts[0], 16)))

        blobsz = os.path.getsize(binpath)
        romsz = roms.get(name, (None, None))[1]
        vrom = roms.get(name, (None, None))[0]
        return name, (vrom, blobsz, romsz, syms, undef), None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--only", default=None)
    ap.add_argument("--syms-out", default=None,
                    help="C file of weak absolute symbol definitions")
    ap.add_argument("--reg-out", default=None, help="blob registry fragment")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    roms = rom_sizes()
    segs = parse_spec(os.path.join(ROOT, "build/pal-1.0/spec"))
    if args.only:
        want = set(args.only.split(","))
        segs = [s for s in segs if s[0] in want]

    work = []
    skipped = []
    for name, includes, number in segs:
        # Objects only. build/pal-1.0/spec also carries the UI texture segments
        # (icon_item_static and friends, segment 8), which are not loaded
        # through Object_Spawn and whose 564 KB would put every reference into
        # them past PSP_SEG89_NATIVE_MIN, where seg_addr deliberately treats a
        # segment-8 value as a native pointer instead.
        if not all(("/assets/objects/" in (o or "")) for o in includes):
            continue
        srcs = [obj_to_source(o) for o in includes]
        if any(s is None for s in srcs):
            skipped.append((name, "no source for %s" % [
                o for o, s in zip(includes, srcs) if s is None][:1]))
            continue
        if name not in roms:
            skipped.append((name, "no ROM range in segment_roms_psp.c"))
            continue
        work.append((name, srcs, number))

    def run_pass(defsyms_for):
        res, errs = {}, []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(build_one, n, s, num, args.outdir, roms,
                              defsyms_for(n)): n for n, s, num in work}
            for f in concurrent.futures.as_completed(futs):
                name, r, err = f.result()
                if err:
                    errs.append((name, err))
                else:
                    res[name] = r
        return res, errs

    # PASS 1 -- every object linked alone, to learn where each symbol lands.
    p1, errors = run_pass(lambda n: None)
    if errors:
        for n, e in errors[:20]:
            print("   %-28s %s" % (n, e))
        return 1

    allsyms = {}
    for n, (_, _, _, syms, _) in p1.items():
        for sym, addr in syms:
            allsyms.setdefault(sym, (n, addr))

    # PASS 2 -- relink, this time telling each object where the symbols it
    # borrows from other objects actually are.
    def defsyms_for(n):
        _, _, _, syms, undef = p1[n]
        own = {s for s, _ in syms}
        out = []
        for u in sorted(set(undef)):
            if u not in own and u in allsyms:
                out.append((u, allsyms[u][1]))
        return out

    results, errors = run_pass(defsyms_for)
    if errors:
        for n, e in errors[:20]:
            print("   %-28s %s" % (n, e))
        return 1

    resolved = sum(len(defsyms_for(n)) for n in results)
    external = {}
    for n, (_, _, _, syms, undef) in results.items():
        own = {s for s, _ in syms}
        for u in undef:
            if u not in own and u not in allsyms:
                external.setdefault(u, []).append(n)

    mismatch = [(n, r[1], r[2]) for n, r in sorted(results.items())
                if r[2] is not None and r[1] != r[2]]

    reg = args.reg_out or os.path.join(args.outdir, "objects.reg")
    with open(reg, "w") as f:
        f.write("/* GENERATED by psp/tools/make_object_blobs.py -- do not edit. */\n")
        for name in sorted(results):
            f.write('    { 0x%08Xu, "blobs/%s.bin" },\n' % (results[name][0], name))

    if args.syms_out:
        write_syms_c(args.syms_out, results)

    with open(os.path.join(args.outdir, "external_refs.txt"), "w") as f:
        for u in sorted(external):
            f.write("%-44s %3d %s\n" % (u, len(external[u]), external[u][0]))

    total = sum(r[1] for r in results.values())
    print("objects built            : %d" % len(results))
    print("blob bytes               : %.1f MB" % (total / 1048576.0))
    print("symbols emitted          : %d" % sum(len(r[3]) for r in results.values()))
    print("cross-object refs bound  : %d" % resolved)
    print("refs left NULL (external): %d symbols" % len(external))
    if skipped:
        print("skipped                  : %d" % len(skipped))
        for n, why in skipped[:10]:
            print("   %-28s %s" % (n, why))
    if mismatch:
        # Every observed case is trailing alignment padding the ROM file has
        # and the blob does not, i.e. the blob is a little SHORT and the
        # engine zero-fills the tail. A blob that came out LONGER would be a
        # real problem -- the engine reads only vromEnd - vromStart bytes --
        # so those are called out separately.
        longer = [m for m in mismatch if m[1] > m[2]]
        print("size differs from ROM    : %d (%d longer -- these lose data)"
              % (len(mismatch), len(longer)))
        for n, b, r in (longer or mismatch)[:20]:
            print("   %-28s blob %7d  rom %7d  diff %+d" % (n, b, r, b - r))
    return 0


def write_syms_c(path, results):
    """Weak absolute definitions for every object symbol.

    WEAK is the whole trick. Some objects are still linked into the EBOOT
    directly (gameplay_keep above all, whose player animation headers hold real
    pointers into the compiled-in link_animetion and therefore cannot become
    segment addresses). A weak definition loses to any strong one, so those
    keep their EBOOT addresses and everything else falls through to the blob's
    segment address -- with no per-symbol exception list to maintain, and no
    duplicate-symbol error possible. Same mechanism z_actor_dlftbls_psp.c uses
    to let a ported actor's profile override the dummy.

    Values are stored pre-biased by -PSP_MODULE_BASE for the reason given at
    the top of psp/src/segment_roms_psp.c: the PSP module loader adds the load
    base to every relocated word, including ones targeting an absolute symbol,
    so pre-subtracting it makes the two cancel.
    """
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    n = 0
    with open(path, "w") as f:
        f.write("/* GENERATED by psp/tools/make_object_blobs.py -- do not edit.\n"
                " * See that file's header for why object data lives in blobs and\n"
                " * why these definitions are weak. */\n\n")
        for name in sorted(results):
            syms = results[name][3]
            if not syms:
                continue
            f.write("/* %s */\n" % name)
            for sym, addr in sorted(syms):
                biased = (addr - PSP_MODULE_BASE) & 0xFFFFFFFF
                f.write('__asm__(".globl %s\\n.weak %s\\n%s = 0x%08x\\n");\n'
                        % (sym, sym, sym, biased))
                n += 1
    return n


if __name__ == "__main__":
    sys.exit(main())
