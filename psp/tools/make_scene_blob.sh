#!/bin/sh
# Build native-endian, segment-addressed scene/room blobs from the decomp's own
# C asset sources.
#
# WHY THIS EXISTS
# ---------------
# reference/libultraship/docs/PORTING.md calls the raw-ROM approach "Option B"
# and warns you must hand-massage endianness and bit widths. This port did that
# for a long time (10 PspFixup*Endian functions, 82 struct field offsets listed
# by hand) and it produced a bug every session -- most recently a scene command
# list that never reached SCENE_CMD_ID_END and spun Object_SpawnPersistent
# ~8.4 million times.
#
# The documented Option A is OTR/Torch + libultraship. That is not viable here:
# every LUS Fast3D backend is shader-based and the PSP GE is fixed-function, so
# LUS would not remove the hard part, and its C++ resource system targets far
# more RAM than a PSP has. But Option A's actual *substance* is only this:
#
#     convert assets at build time to native byte order, with references resolved
#
# and the decomp already hands us that for free -- all 32k assets exist as
# ordinary typed C with real symbol references. The compiler is the byte-order
# converter and the linker is the reference resolver.
#
# HOW
# ---
# Each scene is linked at 0x02000000 and each room at 0x03000000 -- their N64
# SEGMENT bases -- and then objcopy'd to a flat binary. Every internal pointer in
# the result is therefore a genuine segment address (0x02xxxxxx / 0x03xxxxxx),
# which is exactly what SEGMENTED_TO_VIRTUAL already expects, and it is position
# independent: the blob can be loaded anywhere. This mirrors what the N64 build's
# own `spec` file does, and it means ZERO endian fixups.
#
# Rooms are linked one at a time, each at 0x03000000, because only one room is
# resident in segment 3 at a time. The scene is included in every room link (and
# discarded afterwards) so that cross-file references resolve -- rooms really do
# reference the scene's textures, e.g. hakaana2_room_0 uses
# hakaana2_scene_0000A890_Tex.
#
# Usage: make_scene_blob.sh <asset-dir> <scene-name> <out-dir> <room-count>
#   e.g. make_scene_blob.sh extracted/pal-1.0/assets/scenes/misc/hakaana2 hakaana2 build/blobs 1
set -eu

DIR="$1"; NAME="$2"; OUT="$3"; NROOMS="$4"

CC=${CC:-psp-gcc}
LD=${LD:-psp-ld}
OBJCOPY=${OBJCOPY:-psp-objcopy}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)

# Must match Makefile.psp's CFLAGS, plus one addition:
#   -fno-zero-initialized-in-bss keeps all-zero arrays (e.g. a scene's empty
#   BgCamList/SpawnList) in .data. Without it they land in .bss, which is NOBITS
#   and so contributes nothing to an objcopy'd flat binary -- the blob would be
#   short and every pointer past that point would be wrong.
#   -fdata-sections gives every object its own .data.<symbol> section, which is
#   what lets the linker script below pin the scene/room COMMAND LIST to offset 0
#   of its segment. That placement is a hard requirement, not cosmetics:
#   Play_SpawnScene calls Scene_ExecuteCommands(play, this->sceneSegment), i.e.
#   it starts reading commands at the segment base. Everything else in the blob
#   can sit in any order because the linker resolves all internal references.
CFLAGS="-O2 -G0 -Wall -D_PSP_FW_VERSION=600 -DPLATFORM_N64=1 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 \
-DOOT_VERSION=PAL_1_0 -DDEBUG_FEATURES=0 -DNDEBUG -DTARGET_PSP=1 -DNON_MATCHING=1 -DAVOID_UB=1 \
-DF3DEX_GBI_2 -include libc/assert.h -fno-zero-initialized-in-bss -fdata-sections"
# RELATIVE include paths, deliberately: this project's checkout path contains
# spaces ("zelda oot versuch 2"), and an unquoted $INCS would split
# -I/home/henry/Documents/zelda into four arguments. Everything below runs from
# $ROOT, so relative paths sidestep the problem entirely -- which is also how
# Makefile.psp itself does it.
INCS="-Ipsp/include -Ipsp/include/gfx -Iinclude -Isrc -I. -Iextracted/pal-1.0 -Ibuild/pal-1.0"

cd "$ROOT"

mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Pre-rendered backgrounds ("image" rooms: houses, shops, the market).
#
# The N64 build turns each room's <...>_JFIF.jpg into a u64 array of the raw
# JPEG bytes (Makefile's BUILD_JFIF rule) and the console decodes it at runtime
# on the RSP. There is no RSP here, so decode it now instead -- straight into
# the PSP GE's own 5551 pixel format, in place, at the same 153600-byte size the
# array is declared with. See psp/tools/jfif_to_psp.py for the full rationale.
#
# The result is dropped into a shadow tree whose -I comes FIRST, so the room's
# `#include "assets/scenes/.../<name>.jpg.inc.c"` resolves to our decoded
# version instead of build/pal-1.0's JPEG bytes. Nothing in the N64 build
# changes, and no extracted source is edited.
JFIF_SHADOW="$TMP/jfif"
for jpg in "$DIR"/*.jpg; do
    [ -e "$jpg" ] || break
    rel="assets/${DIR#*/assets/}"
    mkdir -p "$JFIF_SHADOW/$rel"
    python3 psp/tools/jfif_to_psp.py "$jpg" \
        "$JFIF_SHADOW/$rel/$(basename "$jpg").inc.c"
done
INCS="-I$JFIF_SHADOW $INCS"

# A scene's RoomList is built from ROM_FILE(<room>), which references the
# _<room>SegmentRomStart/End linker symbols. Those describe where the room lives
# in the *N64 ROM*, and the runtime blob registry keys on exactly that vromStart
# (same key psp_static_assets.c already uses), so define them to their real
# values rather than stubbing them to zero.
#
# psp/src/segment_roms_psp.c stores every value pre-biased by -PSP_MODULE_BASE to
# cancel the PRX loader's unconditional rebase (see the comment at the top of
# that file). Here we want the plain ROM offsets, so add the base back.
defsyms() {
    awk -v base=$((0x08804000)) -v name="$NAME" '
        match($0, /_[A-Za-z0-9_]+SegmentRom(Start|End)/) {
            sym = substr($0, RSTART, RLENGTH)
            if (index(sym, "_" name "_room_") != 1 && index(sym, "_" name "Segment") != 1) next
            if (match($0, /0x[0-9a-fA-F]+/)) {
                v = strtonum(substr($0, RSTART, RLENGTH))
                printf " --defsym %s=0x%08x", sym, and(v + base, 0xffffffff)
            }
        }' psp/src/segment_roms_psp.c
}
DEFSYMS=$(defsyms)

compile() { $CC $CFLAGS $INCS -c "$1" -o "$2"; }

compile "$DIR/${NAME}_scene.c" "$TMP/scene.o"

# See psp/tools/blob_identity_mtx.c: a handful of rooms reference gIdentityMtx,
# which lives in the main binary and so cannot be resolved here. Linked into
# every blob unconditionally -- it is 64 bytes, and making its presence depend
# on whether a given room happens to reference it would mean two link recipes to
# keep in step. Unreferenced, it simply sits in the blob unused.
compile psp/tools/blob_identity_mtx.c "$TMP/identity.o"

link_one() {
    # $1 = room object or empty, $2 = output section script
    cat > "$TMP/seg.ld" <<LDEOF
SECTIONS {
    .scene 0x02000000 : {
        /* The scene command list MUST land at the segment base -- see the
           -fdata-sections note above. */
        "$TMP/scene.o"(.data.${NAME}_scene)
        "$TMP/scene.o"(.data .data.* .rodata .rodata.* .text)
    }
    .room  0x03000000 : { $1 "$TMP/identity.o"(.data .data.* .rodata .rodata.*) }
    /DISCARD/ : {
        *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.mdebug*)
        *(.comment) *(.gcc_compiled*) *(.note.*)
    }
}
LDEOF
    # shellcheck disable=SC2086
    $LD -T "$TMP/seg.ld" $DEFSYMS -o "$TMP/link.elf"
}

# Scene blob (room section empty).
link_one ""
$OBJCOPY -O binary --only-section=.scene "$TMP/link.elf" "$OUT/${NAME}_scene.bin"

# One blob per room, each based at 0x03000000.
i=0
while [ "$i" -lt "$NROOMS" ]; do
    compile "$DIR/${NAME}_room_${i}.c" "$TMP/room.o"
    link_one "\"$TMP/room.o\"(.data.${NAME}_room_${i}) \"$TMP/room.o\"(.data .data.* .rodata .rodata.* .text)"
    $OBJCOPY -O binary --only-section=.room "$TMP/link.elf" "$OUT/${NAME}_room_${i}.bin"
    i=$((i + 1))
done

# Emit this scene's registry fragment. psp_blob_assets.c includes the
# concatenation of all of them (build/blobs/blobs_registry.inc, assembled by
# Makefile.psp), so no vrom hex is ever transcribed by hand -- the values come
# from segment_roms_psp.c, which is itself generated from the ROM map.
#
# Match on "<symbol> = ", not on the symbol alone: segment_roms_psp.c writes each
# symbol twice per line (once after .globl, once as the assignment) and the line
# contains literal backslash-n escapes, so anything cleverer than the assignment
# text is fragile.
vrom_of() {
    awk -v base=$((0x08804000)) -v sym="$1" '
        {
            k = index($0, sym " = ")
            if (k > 0) {
                rest = substr($0, k)
                if (match(rest, /0x[0-9a-fA-F]+/)) {
                    printf "%d", and(strtonum(substr(rest, RSTART, RLENGTH)) + base, 0xffffffff)
                    exit
                }
            }
        }' psp/src/segment_roms_psp.c
}

emit() {
    if [ -z "$1" ]; then
        echo "make_scene_blob.sh: no vrom for $2 -- not in segment_roms_psp.c?" >&2
        exit 1
    fi
    printf '    { 0x%08Xu, "blobs/%s" },\n' "$1" "$2"
}

{
    emit "$(vrom_of "_${NAME}_sceneSegmentRomStart")" "${NAME}_scene.bin"
    j=0
    while [ "$j" -lt "$NROOMS" ]; do
        emit "$(vrom_of "_${NAME}_room_${j}SegmentRomStart")" "${NAME}_room_${j}.bin"
        j=$((j + 1))
    done
} > "$OUT/${NAME}.reg"

ls -l "$OUT" | grep "$NAME"
