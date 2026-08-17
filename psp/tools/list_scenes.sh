#!/bin/sh
# Emit the BLOB_SCENES list for Makefile.psp, one "<subdir>:<name>:<rooms>" per
# scene, derived from the extracted assets rather than hand-maintained.
#
# Everything needed is already on disk and unambiguous:
#   - the set of scenes  = extracted/pal-1.0/baserom/*_scene
#   - the room count      = how many <name>_room_N siblings that scene has
#   - the asset subdir    = the one directory under assets/scenes/*/ named <name>
#
# A scene whose asset directory is missing is skipped with a warning on stderr
# rather than silently dropped -- a missing blob shows up as an empty scene at
# runtime, which is a much harder symptom to trace back to here.
#
# Usage: list_scenes.sh [extracted-root]

set -e

ROOT="${1:-extracted/pal-1.0}"
BASEROM="$ROOT/baserom"
SCENES="$ROOT/assets/scenes"

[ -d "$BASEROM" ] || { echo "list_scenes.sh: no $BASEROM" >&2; exit 1; }

# One pass over baserom to count rooms per scene, and one over assets/scenes to
# map name -> subdir. Doing it with awk keeps this to two directory reads
# instead of ~100 `ls` subprocesses, since $(shell) re-runs it on every make.
ls "$BASEROM" > /tmp/.blobscenes_baserom.$$
ls -d "$SCENES"/*/*/ 2>/dev/null | sed "s#^$SCENES/##; s#/\$##" > /tmp/.blobscenes_dirs.$$

awk -v dirs="/tmp/.blobscenes_dirs.$$" '
BEGIN {
    while ((getline line < dirs) > 0) {
        n = line; sub(/^[^\/]*\//, "", n);   # "indoors/link_home" -> "link_home"
        subdir[n] = line;
    }
}
/_scene$/                { name = $0; sub(/_scene$/, "", name); scenes[name] = 1; next }
/_room_[0-9]+$/          { name = $0; sub(/_room_[0-9]+$/, "", name); rooms[name]++ }
END {
    for (s in scenes) {
        if (!(s in subdir)) { print "list_scenes.sh: no asset dir for " s ", skipped" > "/dev/stderr"; continue }
        r = (s in rooms) ? rooms[s] : 0;
        if (r == 0)         { print "list_scenes.sh: no rooms for " s ", skipped" > "/dev/stderr"; continue }
        print subdir[s] ":" s ":" r;
    }
}' /tmp/.blobscenes_baserom.$$ | sort

rm -f /tmp/.blobscenes_baserom.$$ /tmp/.blobscenes_dirs.$$
