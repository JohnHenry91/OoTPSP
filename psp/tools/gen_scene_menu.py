#!/usr/bin/env python3
"""Generate the warp menu's scene list from the decomp's own tables.

Nothing here is hand-maintained. Two headers already hold everything needed:

  include/tables/scene_table.h     DEFINE_SCENE(<file>_scene, card, SCENE_X, ...)
      -> which asset file backs which SCENE_* id
  include/tables/entrance_table.h  DEFINE_ENTRANCE(ENTR_Y, SCENE_X, spawn, ...)
      -> which entrance lands in which scene

The display name is derived from the SCENE_* id rather than the file name,
because the ids are already the readable ones: SCENE_LINKS_HOUSE reads as
"Link's House" where the file is called `link_home`, and SCENE_KOKIRI_FOREST
where the file is `spot04`.

Entrance choice: the FIRST DEFINE_ENTRANCE naming a scene -- but reported as
its GROUP BASE plus a layer index, never as the raw entrance.

entrance_table.h's header states the rule this depends on: entrances come in
groups of four scene layers (child day / child night / adult day / adult
night), "groups of scene layers are indicated by line breaks", and "only the
first entrance within a group of layers is expected to be referenced in code.
The entrance system will apply the offset on its own." Play_Init duly indexes
`gEntranceTable[entranceIndex + sceneLayer]`.

So handing the menu a non-base member applies the offset TWICE. That was a real
bug here: SCENE_MARKET_ENTRANCE_NIGHT's first entrance is 0x034, the layer-1
member of the group based at 0x033, so warping to it at night landed on
0x034+1 = 0x035 = the Ruins variant. Every scene that only ever appears as a
non-base member -- the night/ruins variants of the Market and Market Entrance,
the adult Kokiri Forest layers -- was reachable only as the wrong variant, and
the Day variants were not reachable at all.

The fix is to emit the base entrance together with the layer offset that
selects the wanted variant, and let the menu establish the world state (age and
time of day) that makes the engine compute that same layer.

FILTERING AGAINST BUILT BLOBS
------------------------------
scene_table.h lists every scene the retail ROM ever defined, including unused
leftovers that ship with no real geometry -- e.g. SCENE_BESITU ("Besitu/Strongbox
Warp" per the CloudModding wiki), which has no baserom entry and no asset
directory at all. list_scenes.sh already computes the set of scenes that
actually got a blob built; passing that same list here (as BLOB_SCENES tokens,
"<subdir>:<name>:<rooms>") lets this script drop any menu entry whose blob
doesn't exist, instead of offering a warp into garbage data. Warping into
SCENE_BESITU crashed with a wild jump (PC inside osContInit, $ra = 0x55) --
not a real bug in osContInit, just what happens when a scene loads whatever
happened to be in the blob-less segment.

Usage: gen_scene_menu.py <include-dir> <output.inc> [blob-scene-token ...]
       (with no tokens, every table entry is offered -- e.g. for cases where
       the blob list isn't available yet)
"""
import os
import re
import sys

# Words the mechanical title-caser gets wrong. Kept deliberately short: this is
# a debug menu, not a localisation, so it only covers what actually reads badly.
FIXUPS = {
    "Links": "Link's",
    "Marios": "Mario's",
    "Sarias": "Saria's",
    "Midos": "Mido's",
    "Impas": "Impa's",
    "Dampes": "Dampe's",
    "Gerudos": "Gerudo's",
    "Zoras": "Zora's",
    "Ganons": "Ganon's",
    "Kings": "King's",
    "Fairys": "Fairy's",
    "Grottos": "Grotto's",
    "Carpenters": "Carpenters'",
    "Gravekeepers": "Gravekeeper's",
    "Knowitall": "Know-It-All",
    "Twins": "Twins'",
    "Of": "of",
    "The": "the",
    "And": "and",
    "In": "in",
}


def pretty(scene_id):
    """SCENE_LINKS_HOUSE -> "Link's House"."""
    words = scene_id.removeprefix("SCENE_").split("_")
    out = []
    for i, w in enumerate(words):
        t = w.capitalize()
        t = FIXUPS.get(t, t)
        # Never lowercase the first word ("Of Time" would be wrong as a start).
        if i == 0:
            t = t[0].upper() + t[1:]
        out.append(t)
    return " ".join(out)


def main(incdir, outpath, blob_tokens):
    scene_h = open(os.path.join(incdir, "tables", "scene_table.h")).read()
    entr_h = open(os.path.join(incdir, "tables", "entrance_table.h")).read()

    # "<subdir>:<name>:<rooms>" -> {name}. Empty when no tokens were passed,
    # which disables the filter rather than dropping every entry.
    blobbed = {tok.split(":")[1] for tok in blob_tokens if tok}

    # DEFINE_SCENE(ydan_scene, g_pn_06, SCENE_DEKU_TREE, SDC_DEKU_TREE, 1, 2)
    scenes = []  # [(file_stem, SCENE_id)] in table order
    for m in re.finditer(r"DEFINE_SCENE\(\s*(\w+?)_scene\s*,\s*\w+\s*,\s*(SCENE_\w+)", scene_h):
        scenes.append((m.group(1), m.group(2)))

    # DEFINE_ENTRANCE(ENTR_DEKU_TREE_0, SCENE_DEKU_TREE, 0, ...)
    #
    # Read line by line rather than with one big finditer, because the grouping
    # into scene layers is carried by the BLANK LINES between entries (see the
    # module docstring) and a whole-file regex cannot see those.
    first_entr = {}  # SCENE_id -> (base ENTR_ name, layer offset 0..3)
    group_base = None
    layer = 0
    for line in entr_h.splitlines():
        m = re.search(r"DEFINE_ENTRANCE\(\s*(ENTR_\w+)\s*,\s*(SCENE_\w+)", line)
        if not m:
            # A blank line (or anything else that is not an entry) closes the
            # current group. Comment lines only appear in the header block.
            if not line.strip():
                group_base = None
            continue
        name, sid = m.group(1), m.group(2)
        if group_base is None:
            group_base, layer = name, 0
        first_entr.setdefault(sid, (group_base, layer))
        layer += 1

    rows, skipped, no_blob = [], [], []
    for stem, sid in scenes:
        entr = first_entr.get(sid)
        if entr is None:
            skipped.append(sid)
            continue
        if blobbed and stem not in blobbed:
            no_blob.append(sid)
            continue
        base, layer = entr
        rows.append((pretty(sid), base, layer, stem, sid))

    rows.sort(key=lambda r: r[0].lower())

    with open(outpath, "w") as f:
        f.write("/* Generated by psp/tools/gen_scene_menu.py -- do not edit.\n"
                " * Source: include/tables/scene_table.h + include/tables/entrance_table.h\n")
        if skipped:
            f.write(" * No entrance found for: %s\n" % ", ".join(skipped))
        if no_blob:
            f.write(" * No blob built (unused/leftover scene, no real asset data): %s\n"
                    % ", ".join(no_blob))
        f.write(" */\n")
        for name, base, layer, stem, sid in rows:
            f.write('    { "%s", %s, %d }, /* %s */\n' % (name, base, layer, stem))

    print("gen_scene_menu: %d scenes, %d skipped, %d without a blob -> %s"
          % (len(rows), len(skipped), len(no_blob), outpath))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
