#!/usr/bin/env python3
"""Which actors does each scene need, and which of them are no-op dummies?

Auftrag 02 asked for in-game telemetry that prints the actor ids spawned as
dummies per scene. This answers the same question WITHOUT a run: every actor a
scene will ever spawn from its scene data is listed in that scene's
ActorEntryList / TransitionActorEntryList, and those are plain text in
extracted/<version>/assets/scenes. Reading them is exact, covers all 101 scenes
at once, and cannot be confused by a scene that crashes before it reports.

(It does NOT see actors spawned by other actors at runtime -- Actor_Spawn calls
from actor code. For the "part of the room is missing" class of report that is
irrelevant: scenery is placed by the scene, not spawned dynamically.)

Usage:
    python3 psp/tools/actor_inventory.py                # summary, all scenes
    python3 psp/tools/actor_inventory.py ydan spot04    # named scenes in full

The "in the build" column comes from Makefile.psp: an actor whose .c is compiled
defines a strong name##_Profile, everything else falls back to the weak alias
gDummyActorProfile in psp/src/z_actor_dlftbls_psp.c.

Since the object-blob pipeline landed, the makefile no longer lists actors one
`PSP_C_FILES +=` line at a time -- it sweeps them with

    PSP_ALL_ACTOR_SRCS := $(filter-out %.inc.c,$(wildcard src/overlays/actors/ovl_*/z_*.c))

Reading only the explicit lines therefore reported almost every actor as a
dummy while all of them were in fact built, which is exactly the kind of stale
measurement this port keeps getting caught by. The wildcard is honoured below.
"""
import collections
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCENES = os.path.join(ROOT, "extracted", "pal-1.0", "assets", "scenes")
MAKEFILE = os.path.join(ROOT, "Makefile.psp")


def actor_id_of_overlay(name):
    """ovl_Bg_Ydan_Sp -> ACTOR_BG_YDAN_SP. The actor table's own convention."""
    return "ACTOR_" + name[len("ovl_"):].upper()


def built_actor_ids():
    """Actor ids whose overlay is genuinely compiled into the PSP build.

    Two sources, because the makefile uses both: the explicit `PSP_C_FILES +=`
    lines, and the wildcard sweep over src/overlays/actors. If the sweep is
    present, every directory it matches counts -- checking the filesystem
    rather than the makefile text is the only way to see what it expands to.
    """
    text = open(MAKEFILE).read()
    ids = {actor_id_of_overlay(m) for m in re.findall(r"ovl_[A-Za-z_0-9]+", text)}

    if "wildcard src/overlays/actors/ovl_*/z_*.c" in text:
        actors_dir = os.path.join(ROOT, "src", "overlays", "actors")
        if os.path.isdir(actors_dir):
            for name in os.listdir(actors_dir):
                if not name.startswith("ovl_"):
                    continue
                d = os.path.join(actors_dir, name)
                if not os.path.isdir(d):
                    continue
                # The sweep only picks up z_*.c that are not .inc.c.
                if any(f.startswith("z_") and f.endswith(".c") and not f.endswith(".inc.c")
                       for f in os.listdir(d)):
                    ids.add(actor_id_of_overlay(name))

    # A few actors are not overlays at all -- they are linked into `code` and
    # live in src/code/z_en_*.c (En_Item00 is the one that matters: 173
    # placements across 28 scenes). Judging those by the ovl_ directories alone
    # reported them as dummies while they were compiled all along.
    for line in text.splitlines():
        m = re.search(r"src/code/(z_en_[a-z_0-9]+)\.c", line)
        if m and not line.strip().startswith("#"):
            ids.add("ACTOR_" + m.group(1)[len("z_"):].upper())
    return ids


def scan():
    """{scene: {'actors': Counter, 'trans': Counter}}"""
    out = collections.defaultdict(lambda: {"actors": collections.Counter(),
                                           "trans": collections.Counter()})
    for root, _dirs, files in os.walk(SCENES):
        for f in files:
            if not f.endswith(".inc.c") or "ActorEntryList" not in f:
                continue
            scene = os.path.basename(root)
            ids = re.findall(r"\bACTOR_[A-Z0-9_]+", open(os.path.join(root, f)).read())
            key = "trans" if "Transition" in f else "actors"
            out[scene][key].update(ids)
    return out


def main():
    have = built_actor_ids()
    scenes = scan()
    wanted = sys.argv[1:]

    if wanted:
        for name in wanted:
            d = scenes.get(name)
            if d is None:
                near = sorted(s for s in scenes if name.lower() in s.lower())
                print(f"{name}: no such scene. Did you mean: {near}")
                continue
            print(f"\n=== {name}")
            for label, counter in (("room actors", d["actors"]),
                                   ("transition actors", d["trans"])):
                print(f"  {label}:")
                for actor, n in counter.most_common():
                    mark = "built" if actor in have else "DUMMY"
                    print(f"    {n:4d}x  {mark:5s}  {actor}")
        return

    total = collections.Counter()
    scenes_of = collections.defaultdict(set)
    for scene, d in scenes.items():
        for counter in (d["actors"], d["trans"]):
            total.update(counter)
            for actor in counter:
                scenes_of[actor].add(scene)

    missing = [(a, n) for a, n in total.most_common() if a not in have]
    print(f"{len(scenes)} scenes, {len(total)} distinct actor ids, "
          f"{len(total) - len(missing)} of them built, {len(missing)} still dummies.")
    print("\nDummies by placement count (count, scenes, id):")
    for actor, n in missing[:40]:
        print(f"  {n:5d}  {len(scenes_of[actor]):3d}  {actor}")


if __name__ == "__main__":
    main()
