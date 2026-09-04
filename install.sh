#!/usr/bin/env bash
#
# OoT-PSP one-shot installer / bootstrapper.
#
# Download this single file, drop your own legally-obtained PAL 1.0 Ocarina of
# Time ROM next to it, and run it. It clones the port, installs the toolchains,
# extracts the assets from your ROM and builds a folder you can copy straight
# onto a PSP memory stick.
#
#   ./install.sh                 interactive, asks before installing anything
#   ./install.sh --yes           assume yes to every prompt (unattended)
#   ./install.sh --help          all options
#
# No game assets are distributed by this script or by the repository it clones.
# Everything the PSP build needs is produced from YOUR ROM, on your machine.

set -Eeuo pipefail

# Arrays, [[ ]] and printf -v are used throughout; /bin/sh on Debian is dash
# and would fail obscurely halfway through instead of here.
if [ -z "${BASH_VERSION:-}" ]; then
    exec bash "$0" "$@"
fi

# Where the user was standing when they ran this script -- NOT necessarily
# where the script file itself lives (those differ if it's invoked by an
# absolute/relative path from another directory), and the one thing the
# finished psp-ready/ output must always land in regardless. Captured here,
# before anything below can `cd`: step 4 does `cd "$WORK_DIR"` for the rest of
# the run, so a bare `pwd` call from inside the packaging step would silently
# return the wrong directory.
INVOKE_DIR=$(pwd -P)

# ===========================================================================
#  Configuration
# ===========================================================================

# Overridable so the repo can be cloned from a local path during testing
# (see installer-test/run-test.sh); users never need to set this.
REPO_URL="${OOTPSP_REPO_URL:-https://github.com/SloppyHenry/SlopcarinaOfTime.git}"
REPO_DIR_NAME="SlopcarinaOfTime"
# Empty means "whatever the repository's default branch is", which is the
# normal case. Overridable for testing a branch before it is merged.
REPO_BRANCH="${OOTPSP_REPO_BRANCH:-}"
GAME_VERSION="pal-1.0"

# MD5 of the supported ROM, in both shapes it can legitimately have.
# "compressed" is the retail cartridge image; "decompressed" is what
# tools/decompress_baserom.py turns it into (and what the PSP build ships).
ROM_MD5_COMPRESSED="e040de91a74b61e3201db0e2323f768a"
ROM_MD5_DECOMPRESSED="f7e8dec14a2fbae90aafa838c801310f"

PSPDEV_PREFIX="${PSPDEV:-$HOME/pspdev}"
PSPDEV_RELEASE_API="https://api.github.com/repos/pspdev/pspdev/releases/latest"

OUT_DIR_NAME="psp-ready"
EBOOT_TITLE_DIR="OOTPSP"

ASSUME_YES=0
SKIP_DEPS=0
JOBS=""
ROM_ARG=""
TARGET_DIR_ARG=""

# ===========================================================================
#  Presentation
# ===========================================================================

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_RST=$'\033[0m';  C_B=$'\033[1m'
    C_GOLD=$'\033[38;5;220m'; C_GRN=$'\033[38;5;77m'
    C_RED=$'\033[38;5;203m';  C_YEL=$'\033[38;5;214m'
    C_CYA=$'\033[38;5;80m';   C_GRY=$'\033[38;5;244m'
    CLR_EOL=$'\033[K'
    TTY_UI=1
else
    C_RST=""; C_B=""; C_GOLD=""; C_GRN=""; C_RED=""
    C_YEL=""; C_CYA=""; C_GRY=""; CLR_EOL=""
    TTY_UI=0
fi

banner() {
    printf '%s' "$C_GOLD"
    cat <<'ART'
           /\
          /  \
         /    \
        /      \
       /        \
      /__________\
     /\          /\
    /  \        /  \
   /    \      /    \
  /      \    /      \
 /        \  /        \
/__________\/__________\

      O o T P S P
ART
    printf '%s\n' "$C_RST"
    printf '   %sThe Legend of Zelda: Ocarina of Time%s  ->  %sPlayStation Portable%s\n' \
        "$C_B" "$C_RST" "$C_B" "$C_RST"
    printf '   %sone-shot installer  ~  builds from your own ROM, on your machine%s\n\n' \
        "$C_GRY" "$C_RST"
}

hr()    { printf '   %s%s%s\n' "$C_GRY" "_______________________________________________________________" "$C_RST"; }
say()   { printf '   %s\n' "$*"; }
info()  { printf '   %s*%s %s\n'  "$C_CYA" "$C_RST" "$*"; }
ok()    { printf '   %sok%s %s\n' "$C_GRN" "$C_RST" "$*"; }
warn()  { printf '   %s!!%s %s\n' "$C_YEL" "$C_RST" "$*"; }

# Find and print the actual error line from a failed step, with a little
# context, instead of just the tail of the log -- a step's own recipe can
# print a lot of harmless output AFTER the real error (e.g. make cleaning up
# every partial object from a failed parallel build with one long `rm a b
# c ...` line), which has pushed the real error out of a plain "last 25
# lines" view for real. Scoped to lines after the last "== <step label> =="
# marker (written by run_step/run_step_watch), so a match cannot come from an
# earlier, already-successful step.
show_error_from_log() {
    local log=$1 step_start pattern hit lineno start end n text
    # `|| true` on both greps below is load-bearing, not defensive filler: with
    # `set -e -o pipefail` active for the whole script, a grep that matches
    # nothing makes the pipe's exit status non-zero even though the trailing
    # `tail`/`cut` succeeded, and an unguarded `var=$(...)` assignment with
    # that status would kill the ENTIRE script right here -- silently, with
    # no message at all, which is a strictly worse outcome than the plain
    # tail this function replaced. Caught by testing the "no marker yet"
    # case, which is real: e.g. `psp-config --pspsdk-path` failing as the
    # very first thing ever written to a fresh log, before any run_step call.
    step_start=$(grep -n '^== .* ==$' "$log" 2>/dev/null | tail -n1 | cut -d: -f1) || true
    step_start=${step_start:-1}

    pattern='[Ee]rror[: ]|ERROR:|fatal error|undefined reference|make(\[[0-9]+\])?: \*\*\*'
    pattern="$pattern"'|cannot open|cannot find|No such file or directory|No rule to make target'
    pattern="$pattern"'|Permission denied|command not found|ModuleNotFoundError|ImportError'
    pattern="$pattern"'|SyntaxError|Traceback \(most recent|collect2:|^ld: |ld returned'
    hit=$(tail -n "+$step_start" "$log" | grep -nE "$pattern" | tail -n1) || true

    if [ -z "$hit" ]; then
        printf '   %slast lines of %s:%s\n' "$C_GRY" "$log" "$C_RST"
        tail -n 25 "$log" | sed 's/^/       /'
        return
    fi

    # $hit's line number is relative to the tail'd slice -- translate back to
    # an absolute line number in the real file before using it with sed.
    lineno=$(( step_start - 1 + ${hit%%:*} ))
    start=$(( lineno > 5 ? lineno - 5 : 1 ))
    end=$((lineno + 3))

    printf '   %sthe error:%s\n' "$C_RED" "$C_RST"
    n=$start
    while [ "$n" -le "$end" ]; do
        text=$(sed -n "${n}p" "$log")
        if [ -n "$text" ]; then
            if [ "$n" -eq "$lineno" ]; then
                printf '     %s->%s %s\n' "$C_RED" "$C_RST" "$text"
            else
                printf '        %s\n' "$text"
            fi
        fi
        n=$((n + 1))
    done
}

# Plain fatal exit: prints the message and stops. This is what nearly every
# check in this script uses (missing ROM, unsupported OS, no MIPS assembler,
# ...) -- these are "we looked and it's not there" failures, not a command
# that just failed, so there is nothing relevant in the log to show. Showing
# it anyway is pure noise: a "last lines of the log" block displaying the
# PREVIOUS, perfectly successful step's routine output, which has nothing to
# do with why THIS check failed.
die() {
    printf '\n   %sxx  %s%s\n\n' "$C_RED" "$*" "$C_RST"
    exit 1
}

# Same, but for a genuinely failed command: additionally extracts and shows
# the real error from the log. Used only by run_step/run_step_watch's own
# failure path below, which is the one place a command actually just exited
# non-zero and the log might explain why.
die_from_log() {
    printf '\n   %sxx  %s%s\n\n' "$C_RED" "$*" "$C_RST"
    if [ -n "${LOG_FILE:-}" ] && [ -s "${LOG_FILE:-}" ]; then
        show_error_from_log "$LOG_FILE"
        printf '\n   %sfull log: %s%s\n\n' "$C_GRY" "$LOG_FILE" "$C_RST"
    fi
    exit 1
}

STEP_NO=0
STEP_TOTAL=9
step() {
    STEP_NO=$((STEP_NO + 1))
    printf '\n%s   [%d/%d]%s %s%s%s\n' \
        "$C_GOLD" "$STEP_NO" "$STEP_TOTAL" "$C_RST" "$C_B" "$*" "$C_RST"
}

# ---------------------------------------------------------------------------
#  Progress bar
#
#  Determinate where a meaningful unit count exists (compiled files, packed
#  scenes), indeterminate otherwise. Non-tty output degrades to one plain line
#  per step instead of redrawing, so piped logs stay readable.
# ---------------------------------------------------------------------------
BAR_WIDTH=40

draw_bar() {
    local pct=$1 label=$2 filled i bar="" empty=""
    [ "$TTY_UI" = 1 ] || return 0
    (( pct < 0 ))   && pct=0
    (( pct > 100 )) && pct=100
    filled=$(( pct * BAR_WIDTH / 100 ))
    for ((i = 0; i < filled; i++));         do bar+="#";   done
    for ((i = filled; i < BAR_WIDTH; i++)); do empty+="."; done
    printf '\r      %s[%s%s%s%s]%s %3d%%  %s%s%s' \
        "$C_GRY" "$C_GOLD" "$bar" "$C_GRY" "$empty" "$C_RST" \
        "$pct" "$C_GRY" "$label" "$CLR_EOL"
}

draw_spin() {
    local tick=$1 label=$2 pos i bar="" span
    [ "$TTY_UI" = 1 ] || return 0
    span=$(( (BAR_WIDTH - 3) * 2 ))
    pos=$(( tick % span ))
    (( pos >= BAR_WIDTH - 3 )) && pos=$(( span - pos ))
    for ((i = 0; i < BAR_WIDTH; i++)); do
        if (( i >= pos && i < pos + 3 )); then bar+="#"; else bar+="."; fi
    done
    printf '\r      %s[%s]%s  ..   %s%s%s' \
        "$C_GRY" "$bar" "$C_RST" "$C_GRY" "$label" "$CLR_EOL"
}

bar_done() {
    if [ "$TTY_UI" = 1 ]; then
        printf '\r      %sdone%s  %s%s\n' "$C_GRN" "$C_RST" "$1" "$CLR_EOL"
    else
        printf '      done  %s\n' "$1"
    fi
}

# If the wrapped command is `make ... -jN ...` and it fails, retry once with
# -j1 before giving up. This script drives several Makefiles it does not own
# (n64texconv's among them), and a from-scratch parallel build hitting a
# `mkdir -p` / `.d`-file race in one of them is a real, if intermittent,
# failure mode -- observed once for n64texconv on a fresh clone, not
# reproducible on a warm tree. A slower single-job rerun is a much better
# answer than failing the whole install over a build tool's own race.
run_with_retry() {
    "$@" && return 0
    local rc=$? cmd=("$@") i j_idx=-1
    if [ "${cmd[0]}" = "make" ]; then
        for i in "${!cmd[@]}"; do
            case "${cmd[$i]}" in -j*) j_idx=$i ;; esac
        done
        if [ "$j_idx" -ge 0 ] && [ "${cmd[$j_idx]}" != "-j1" ]; then
            printf '\n-- parallel build failed, retrying single-threaded --\n'
            cmd[$j_idx]="-j1"
            "${cmd[@]}" && return 0
        fi
    fi
    return "$rc"
}

# run_step <label> <expected_units> <count_regex> -- <command...>
#
# Runs the command with output captured to $LOG_FILE, animating a bar while it
# works. With <expected_units> = 0 the bar is indeterminate; otherwise fill is
# (log lines matching <count_regex>) / <expected_units>, capped at 99% until
# the command actually exits -- an estimate that jumps to "done" is less
# confusing than one that claims 100% and then sits there.
run_step() {
    local label=$1 total=$2 pat=$3
    shift 3
    [ "${1:-}" = "--" ] && shift

    local donefile rc tick=0 n pct
    donefile=$(mktemp "${TMPDIR:-/tmp}/ootpsp-step.XXXXXX")

    printf '\n== %s ==\n' "$label" >>"$LOG_FILE"
    [ "$TTY_UI" = 1 ] || printf '      .. %s\n' "$label"

    # The subshell writes its exit status to a file rather than relying on
    # `kill -0 $pid`: once bash reaps a finished child that can go either way,
    # and a spin loop that never terminates is a nasty way to fail.
    (
        run_with_retry "$@" >>"$LOG_FILE" 2>&1
        echo $? >"$donefile"
    ) &
    local pid=$!

    while [ ! -s "$donefile" ]; do
        if [ "$total" -gt 0 ]; then
            n=$(grep -cE "$pat" "$LOG_FILE" 2>/dev/null || true)
            pct=$(( ${n:-0} * 100 / total ))
            (( pct > 99 )) && pct=99
            draw_bar "$pct" "$label"
        else
            draw_spin "$tick" "$label"
        fi
        tick=$((tick + 1))
        sleep 0.15
    done

    wait "$pid" 2>/dev/null || true
    rc=$(cat "$donefile"); rm -f "$donefile"

    [ "$rc" = "0" ] || { printf '\r%s' "$CLR_EOL"; die_from_log "Step failed: $label  (exit $rc)"; }
    bar_done "$label"
}

# run_step_watch <label> <watch_dir> <expected_files> -- <command...>
#
# Like run_step, but reports on a directory the command is filling rather than
# on its output -- several of the extraction tools print one summary line at
# the end and nothing at all while they work, so the directory is the only
# honest source of progress.
#
# Shows the name of the file being written right now. Finding it costs two
# `ls` calls per tick regardless of how large the tree is: list the watch
# directory newest-first, and if the newest entry is itself a directory,
# descend into it (a directory's mtime moves whenever a file is added to it,
# so the newest one is the one being written). That matters -- the asset tree
# is ~43k files, and counting it repeatedly would slow down the very
# extraction it is meant to be reporting on.
#
# With expected_files > 0 the bar is determinate, counting entries directly in
# watch_dir; that is only meaningful for a flat output directory.
run_step_watch() {
    local label=$1 watch=$2 total=$3
    shift 3
    [ "${1:-}" = "--" ] && shift

    local donefile rc tick=0 n pct cur
    donefile=$(mktemp "${TMPDIR:-/tmp}/ootpsp-step.XXXXXX")

    printf '\n== %s ==\n' "$label" >>"$LOG_FILE"
    [ "$TTY_UI" = 1 ] || printf '      .. %s\n' "$label"

    (
        run_with_retry "$@" >>"$LOG_FILE" 2>&1
        echo $? >"$donefile"
    ) &
    local pid=$!

    while [ ! -s "$donefile" ]; do
        if [ "$TTY_UI" = 1 ]; then
            cur=$(newest_leaf "$watch")
            if [ "$total" -gt 0 ]; then
                n=$(ls -1 "$watch" 2>/dev/null | wc -l)
                pct=$(( n * 100 / total ))
                (( pct > 99 )) && pct=99
                draw_bar "$pct" "$label  $cur"
            else
                draw_spin "$tick" "$label  $cur"
            fi
        fi
        tick=$((tick + 1))
        sleep 0.08
    done

    wait "$pid" 2>/dev/null || true
    rc=$(cat "$donefile"); rm -f "$donefile"

    [ "$rc" = "0" ] || { printf '\r%s' "$CLR_EOL"; die_from_log "Step failed: $label  (exit $rc)"; }
    bar_done "$label"
}

# Name of the file most recently written under $1, descending at most three
# levels into whichever subdirectory is currently newest. Truncated so the
# progress line cannot wrap, which would leave debris on screen when the name
# after it is shorter.
newest_leaf() {
    local d=$1 entry depth=0
    while [ "$depth" -lt 3 ]; do
        entry=$(ls -1t "$d" 2>/dev/null | head -n1)
        [ -n "$entry" ] || { printf ''; return; }
        if [ -d "$d/$entry" ]; then
            d="$d/$entry"
            depth=$((depth + 1))
        else
            break
        fi
    done
    [ -n "${entry:-}" ] || { printf ''; return; }
    if [ "${#entry}" -gt 30 ]; then
        printf '%s...' "${entry:0:27}"
    else
        printf '%s' "$entry"
    fi
}

ask() {
    # ask <question> [Y|N]  ->  0 = yes
    local q=$1 def=${2:-Y} reply prompt
    if [ "$def" = "Y" ]; then prompt="[Y/n]"; else prompt="[y/N]"; fi
    if [ "$ASSUME_YES" = 1 ]; then
        printf '   %s?%s %s %s %s(--yes)%s\n' "$C_CYA" "$C_RST" "$q" "$prompt" "$C_GRY" "$C_RST"
        return 0
    fi
    if [ ! -t 0 ]; then
        # Piped in with no tty on stdin: take the default rather than hanging.
        printf '   %s?%s %s %s -> default\n' "$C_CYA" "$C_RST" "$q" "$prompt"
        [ "$def" = "Y" ] && return 0 || return 1
    fi
    while true; do
        printf '   %s?%s %s %s ' "$C_CYA" "$C_RST" "$q" "$prompt"
        read -r reply || reply=""
        reply=${reply:-$def}
        case "$reply" in
            [Yy]|[Yy][Ee][Ss]|[Jj]|[Jj][Aa]) return 0 ;;
            [Nn]|[Nn][Oo]|[Nn][Ee][Ii][Nn])  return 1 ;;
            *) warn "Please answer y or n." ;;
        esac
    done
}

# ===========================================================================
#  Arguments
# ===========================================================================

usage() {
    cat <<USAGE
OoT-PSP installer

  ./install.sh [options]

Options:
  -y, --yes             Assume "yes" for every prompt (unattended install).
      --rom PATH        Use this ROM file instead of searching the folder.
      --dir PATH        Clone/build in PATH (default: ./${REPO_DIR_NAME}).
  -j, --jobs N          Parallel build jobs (default: number of CPUs).
      --skip-deps       Do not check or install system packages.
  -h, --help            This text.

Environment:
  PSPDEV                Where the PSP toolchain lives / gets installed.
                        Default: \$HOME/pspdev
  MIPS_BINUTILS_PREFIX  Prefix of an existing N64 MIPS binutils, e.g.
                        "mips64-elf-". Auto-detected when unset.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        -y|--yes)     ASSUME_YES=1 ;;
        --skip-deps)  SKIP_DEPS=1 ;;
        --rom)        ROM_ARG=${2:?--rom needs a path}; shift ;;
        --dir)        TARGET_DIR_ARG=${2:?--dir needs a path}; shift ;;
        -j|--jobs)    JOBS=${2:?--jobs needs a number}; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) printf 'Unknown option: %s\n\n' "$1"; usage; exit 2 ;;
    esac
    shift
done

# ===========================================================================
#  Where are we, and where do we build?
# ===========================================================================

# When piped (curl | bash) there is no script file -- fall back to the current
# directory, which is where the user's ROM will be.
if [ -f "${BASH_SOURCE[0]:-}" ]; then
    SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
else
    SCRIPT_DIR=$(pwd -P)
fi

# If this script is sitting inside an already-cloned port, use that checkout
# rather than nesting a second one inside it.
if [ -f "$SCRIPT_DIR/Makefile.psp" ] && [ -d "$SCRIPT_DIR/psp" ]; then
    WORK_DIR="$SCRIPT_DIR"
    ALREADY_CLONED=1
else
    WORK_DIR="${TARGET_DIR_ARG:-$SCRIPT_DIR/$REPO_DIR_NAME}"
    ALREADY_CLONED=0
fi

LOG_DIR="$SCRIPT_DIR/ootpsp-install-logs"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/install-$(date +%Y%m%d-%H%M%S).log"
: >"$LOG_FILE"

# ===========================================================================
#  Intro
# ===========================================================================

clear 2>/dev/null || true
banner

printf '   %s%s PROTOTYPE  --  WORK IN PROGRESS %s\n' "$C_RED" "$C_B" "$C_RST"
cat <<'NOTICE'

   This is an early, unfinished port: a technical prototype, not a playable
   game. Right now the world renders, Link walks, the first few actors and
   the audio engine are alive -- and that is roughly it. Expect crashes,
   missing objects, graphical glitches, and silence where there should be
   sound. It does not work yet, on purpose.

   You need your own legally obtained cartridge dump of The Legend of Zelda:
   Ocarina of Time, PAL version 1.0. Nothing is downloaded except open-source
   toolchains and this port's own source code. Your ROM never leaves this
   machine.

NOTICE
hr
printf '\n'

ask "Understood -- continue?" Y || { say "Aborted. Nothing was changed."; exit 0; }

printf '\n'
say "${C_B}Before this goes any further:${C_RST} put your ROM file in"
printf '\n      %s%s%s\n\n' "$C_B" "$SCRIPT_DIR" "$C_RST"
say "Any of .z64 / .n64 / .v64 works -- the script sorts out byte order and"
say "verifies the version later. If it isn't there yet, this script will"
say "notice and offer to check again once you've added it."
printf '\n'
ask "Placed it there -- continue?" Y || { say "Aborted. Nothing was changed."; exit 0; }

# ===========================================================================
#  1. Privileges
# ===========================================================================

step "Checking privileges"

SUDO=""
IS_ROOT=0
CAN_INSTALL=0
if [ "$(id -u)" = "0" ]; then
    IS_ROOT=1
    CAN_INSTALL=1
    ok "Running as root -- system packages can be installed directly."
    warn "Everything built will be owned by root. That works, but if you would"
    warn "rather keep the files yours, abort and re-run as a normal user: the"
    warn "script then asks for sudo only where it genuinely needs it."
    ask "Continue as root?" Y || { say "Aborted."; exit 0; }
elif command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
    CAN_INSTALL=1
    ok "Not root -- 'sudo' is available, and is used only to install missing"
    say "   system packages. The build itself runs as $(id -un)."
else
    warn "Not root, and no 'sudo' found."
    warn "Missing system packages cannot be installed automatically; the script"
    warn "will list what you need and stop."
fi

# Package installs run inside run_step, whose output goes to the log file --
# so a sudo password prompt there would be invisible and look like a hang.
# Cache the credentials here, in the foreground, before that can happen.
ensure_sudo() {
    [ -n "$SUDO" ] || return 0
    sudo -n true 2>/dev/null && return 0
    say "sudo needs your password to install packages:"
    sudo -v || die "Could not obtain sudo privileges."
}

# ===========================================================================
#  2. Which system is this?
# ===========================================================================

step "Detecting your system"

[ "$(uname -s)" = "Linux" ] || die "This installer supports Linux only (found: $(uname -s)).
       On macOS, install pspdev via Homebrew and follow the manual build
       steps in the repository README."

OS_NAME="unknown"; OS_ID=""; OS_LIKE=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    OS_NAME=${PRETTY_NAME:-${NAME:-unknown}}
    OS_ID=${ID:-}
    OS_LIKE=${ID_LIKE:-}
fi

if   command -v apt-get >/dev/null 2>&1; then PKG="apt"
elif command -v dnf     >/dev/null 2>&1; then PKG="dnf"
elif command -v pacman  >/dev/null 2>&1; then PKG="pacman"
elif command -v zypper  >/dev/null 2>&1; then PKG="zypper"
else PKG=""
fi

ok "System   : $OS_NAME"
ok "Arch     : $(uname -m)"
if [ -n "$PKG" ]; then
    ok "Packages : $PKG"
else
    warn "No supported package manager found (apt/dnf/pacman/zypper)."
    warn "Dependencies will have to be installed by hand."
    CAN_INSTALL=0
fi

case "$(uname -m)" in
    x86_64|aarch64) ;;
    *) warn "Untested CPU architecture -- a prebuilt PSP toolchain may not exist"
       warn "for it, and you may have to build pspdev from source." ;;
esac

# ===========================================================================
#  3. Build dependencies
# ===========================================================================

step "Checking build dependencies"

# Fields: <probe>|<apt>|<dnf>|<pacman>|<zypper>|<why>
#   cmd:NAME   satisfied when NAME is on PATH
#   py:MODULE  satisfied when the SYSTEM python3 can import MODULE. System,
#              not the venv, deliberately: the PSP Makefile's own asset rules
#              call a bare `python3`.
#   font:NAME  satisfied when a TTF of that name exists under /usr/share/fonts
DEPS=(
  "cmd:git|git|git|git|git|clone the port"
  "cmd:make|make|make|make|make|drive the build"
  "cmd:gcc|build-essential|gcc|base-devel|gcc|build the host-side asset tools"
  "cmd:python3|python3|python3|python|python3|asset extraction"
  "cmd:curl|curl|curl|curl|curl|fetch the PSP toolchain"
  "cmd:tar|tar|tar|tar|tar|unpack the PSP toolchain"
  "cmd:md5sum|coreutils|coreutils|coreutils|coreutils|verify your ROM"
  "cmd:xml2-config|libxml2-dev|libxml2-devel|libxml2|libxml2-devel|audio asset tools"
  "cmd:gawk|gawk|gawk|gawk|gawk|packing scene blobs (needs GNU awk, not mawk/POSIX awk)"
  "py:venv|python3-venv|python3|python|python3|isolated python environment"
  "py:PIL|python3-pil|python3-pillow|python-pillow|python3-Pillow|texture and font conversion"
  "font:DejaVuSansMono|fonts-dejavu-core|dejavu-sans-mono-fonts|ttf-dejavu|dejavu-fonts|on-screen debug font"
)

dep_field() { printf '%s' "$1" | cut -d'|' -f"$2"; }

pkg_for() {
    case "$PKG" in
        dnf)    dep_field "$1" 3 ;;
        pacman) dep_field "$1" 4 ;;
        zypper) dep_field "$1" 5 ;;
        *)      dep_field "$1" 2 ;;
    esac
}

probe_ok() {
    local kind=${1%%:*} target=${1#*:}
    case "$kind" in
        cmd)  command -v "$target" >/dev/null 2>&1 ;;
        py)   python3 -c "import $target" >/dev/null 2>&1 ;;
        font) [ -n "$(find /usr/share/fonts -name "${target}*.ttf" -print -quit 2>/dev/null)" ] ;;
        *)    return 1 ;;
    esac
}

collect_missing() {
    MISSING_PKGS=(); MISSING_DESC=()
    local d probe why p
    for d in "${DEPS[@]}"; do
        probe=$(dep_field "$d" 1)
        why=$(dep_field "$d" 6)
        if probe_ok "$probe"; then
            [ "${1:-verbose}" = "quiet" ] || \
                printf '   %sok%s %-16s %s(%s)%s\n' "$C_GRN" "$C_RST" "${probe#*:}" "$C_GRY" "$why" "$C_RST"
        else
            p=$(pkg_for "$d")
            [ "${1:-verbose}" = "quiet" ] || \
                printf '   %s!!%s %-16s missing -> package %s  %s(%s)%s\n' \
                    "$C_YEL" "$C_RST" "${probe#*:}" "'$p'" "$C_GRY" "$why" "$C_RST"
            MISSING_PKGS+=("$p")
            MISSING_DESC+=("${probe#*:}")
        fi
    done
}

install_packages() {
    [ $# -gt 0 ] || return 0
    ensure_sudo
    case "$PKG" in
        apt)
            run_step "apt-get update" 0 "" -- $SUDO apt-get update -y
            run_step "installing $# package(s)" 0 "" -- \
                env DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y "$@" ;;
        dnf)
            run_step "installing $# package(s)" 0 "" -- $SUDO dnf install -y "$@" ;;
        pacman)
            # Deliberately NOT -Sy: syncing the database without upgrading is a
            # partial upgrade, which Arch explicitly warns can break a system.
            # If the local database is too stale for a plain -S, that is the
            # user's call to make, not an installer's.
            run_step "installing $# package(s)" 0 "" -- $SUDO pacman -S --needed --noconfirm "$@" ;;
        zypper)
            run_step "installing $# package(s)" 0 "" -- $SUDO zypper --non-interactive install "$@" ;;
        *)  return 1 ;;
    esac
}

# Best-effort: install each package on its own and shrug off the ones this
# distro release does not have under that exact name. Used for the prebuilt PSP
# toolchain's optional runtime libraries, where an exact name match across
# distro releases is not realistic and a hard failure would be worse than a
# missing optional library.
install_packages_soft() {
    local p
    ensure_sudo
    for p in "$@"; do
        case "$PKG" in
            apt)    $SUDO apt-get install -y "$p"               >>"$LOG_FILE" 2>&1 || true ;;
            dnf)    $SUDO dnf install -y "$p"                   >>"$LOG_FILE" 2>&1 || true ;;
            pacman) $SUDO pacman -S --needed --noconfirm "$p"   >>"$LOG_FILE" 2>&1 || true ;;
            zypper) $SUDO zypper --non-interactive install "$p" >>"$LOG_FILE" 2>&1 || true ;;
        esac
    done
}

MISSING_PKGS=(); MISSING_DESC=()
if [ "$SKIP_DEPS" = 1 ]; then
    warn "--skip-deps given: not checking system packages."
else
    collect_missing verbose
fi

if [ ${#MISSING_PKGS[@]} -gt 0 ]; then
    printf '\n'
    say "Missing:       ${C_B}${MISSING_DESC[*]}${C_RST}"
    say "Would install: ${C_GRY}${MISSING_PKGS[*]}${C_RST}"
    printf '\n'
    [ "$CAN_INSTALL" = 1 ] || die "Cannot install packages automatically (no package manager, or no
       root/sudo). Please install the packages listed above by hand, then
       run this script again."
    ask "Install these packages now?" Y || die "Cannot continue without these packages."
    install_packages "${MISSING_PKGS[@]}" || die "Package installation failed -- see $LOG_FILE"
    # Re-probe: a package that installed under a slightly different name than
    # expected still deserves a fair second chance.
    collect_missing quiet
    [ ${#MISSING_PKGS[@]} -eq 0 ] || die "Still missing after install: ${MISSING_DESC[*]}
       Install them by hand and run this script again."
    ok "All build dependencies present."
elif [ "$SKIP_DEPS" = 0 ]; then
    ok "All build dependencies present."
fi

# ---------------------------------------------------------------------------
#  3b. Cross toolchains
# ---------------------------------------------------------------------------

printf '\n'
say "${C_B}Cross toolchains${C_RST}"

# --- PSP (pspdev) ----------------------------------------------------------
find_pspdev() {
    command -v psp-config >/dev/null 2>&1 && return 0
    if [ -x "$PSPDEV_PREFIX/bin/psp-config" ]; then
        export PSPDEV="$PSPDEV_PREFIX"
        export PATH="$PSPDEV_PREFIX/bin:$PATH"
        return 0
    fi
    return 1
}

install_pspdev() {
    local asset_url tarball arch pattern parent
    arch=$(uname -m)
    case "$arch" in
        x86_64)  pattern='pspdev-ubuntu-latest-x86_64' ;;
        aarch64) pattern='pspdev-ubuntu-.*arm64' ;;
        *)       pattern='pspdev-ubuntu' ;;
    esac
    # Prefer the asset matching this distro; upstream also ships plain Debian
    # and Fedora builds. Only for x86_64 -- the ARM release is Ubuntu-only.
    #
    # Matched on ID before ID_LIKE, deliberately: Ubuntu carries
    # ID_LIKE=debian, and a bare *debian* match would hand every Ubuntu (and
    # Mint, and Pop!_OS) the Debian tarball instead of the Ubuntu build
    # upstream tests most widely.
    if [ "$arch" = "x86_64" ]; then
        case "$OS_ID" in
            debian)
                pattern='pspdev-debian-latest' ;;
            fedora|rhel|centos|rocky|almalinux)
                pattern='pspdev-fedora-latest' ;;
            ubuntu|linuxmint|pop|elementary|zorin|neon)
                pattern='pspdev-ubuntu-latest-x86_64' ;;
            *)
                case "$OS_LIKE" in
                    *fedora*|*rhel*)   pattern='pspdev-fedora-latest' ;;
                    *ubuntu*|*debian*) pattern='pspdev-ubuntu-latest-x86_64' ;;
                esac ;;
        esac
    fi

    asset_url=$(curl -fsSL "$PSPDEV_RELEASE_API" \
        | grep -o '"browser_download_url": *"[^"]*"' | cut -d'"' -f4 \
        | grep -E "$pattern" | head -n1) || true
    if [ -z "$asset_url" ]; then
        asset_url=$(curl -fsSL "$PSPDEV_RELEASE_API" \
            | grep -o '"browser_download_url": *"[^"]*"' | cut -d'"' -f4 \
            | grep -E 'ubuntu.*x86_64' | head -n1) || true
    fi
    [ -n "$asset_url" ] || die "Could not find a prebuilt PSP toolchain for this system.
       Install it manually from https://github.com/pspdev/pspdev and re-run."

    tarball="$LOG_DIR/$(basename "$asset_url")"
    run_step "downloading PSP toolchain (~300 MB)" 0 "" -- \
        curl -fL --retry 3 -o "$tarball" "$asset_url"

    # The tarball's top-level directory is "pspdev/", so it unpacks into the
    # PARENT of the wanted prefix.
    parent=$(dirname "$PSPDEV_PREFIX")
    mkdir -p "$parent"
    run_step "unpacking PSP toolchain" 0 "" -- tar -xzf "$tarball" -C "$parent"
    rm -f "$tarball"

    export PSPDEV="$PSPDEV_PREFIX"
    export PATH="$PSPDEV_PREFIX/bin:$PATH"

    # The prebuilt binaries link against ordinary host libraries. A missing one
    # surfaces much later as "cc1: error while loading shared libraries", which
    # is very hard to connect back to here -- so pull them in up front.
    if [ "$CAN_INSTALL" = 1 ]; then
        case "$PKG" in
            apt)    install_packages_soft libmpc3 libmpfr6 libgmp10 libarchive13 \
                                          libusb-1.0-0 libreadline8 libncurses6 zlib1g ;;
            dnf)    install_packages_soft libmpc mpfr gmp libarchive libusb1 readline ncurses-libs zlib ;;
            pacman) install_packages_soft libmpc mpfr gmp libarchive libusb readline ncurses zlib ;;
            zypper) install_packages_soft libmpc3 libmpfr6 libgmp10 libarchive13 libusb-1_0-0 readline zlib ;;
        esac
    fi
}

if find_pspdev; then
    ok "PSP toolchain : $(command -v psp-config)"
else
    warn "PSP toolchain (pspdev) not found."
    say  "   It provides psp-gcc/psp-ld and the PSPSDK this port links against."
    say  "   The prebuilt release is roughly 300 MB and installs into"
    say  "   ${C_B}$PSPDEV_PREFIX${C_RST}, touching nothing else on your system."
    ask "Download and install pspdev now?" Y || die "Cannot build without the PSP toolchain."
    install_pspdev
    find_pspdev || die "pspdev installed, but psp-config is still not in $PSPDEV_PREFIX/bin"
    ok "PSP toolchain : $(command -v psp-config)"
fi

psp-config --pspsdk-path >>"$LOG_FILE" 2>&1 || die "psp-config is on PATH but does not work.
       Usually a missing host library. Try running it directly:
         psp-config --pspsdk-path"

# --- N64 MIPS binutils -----------------------------------------------------
#
# Only the assembler is needed, and only for audio: the game's music sequences
# are assembled with it before the port repacks them, and the sequence-to-
# soundfont table is generated by reading those objects back. No N64 C compiler
# is involved -- this port never builds the N64 ROM.
detect_mips_prefix() {
    local p
    if [ -n "${MIPS_BINUTILS_PREFIX:-}" ]; then
        command -v "${MIPS_BINUTILS_PREFIX}as" >/dev/null 2>&1 && return 0
        [ -x "${MIPS_BINUTILS_PREFIX}as" ] && return 0
        warn "MIPS_BINUTILS_PREFIX=$MIPS_BINUTILS_PREFIX is set, but ${MIPS_BINUTILS_PREFIX}as was not found."
    fi
    for p in mips-linux-gnu- mips64-linux-gnu- mips64-elf- mips64-ultra-elf- mips64-; do
        command -v "${p}as" >/dev/null 2>&1 && { MIPS_BINUTILS_PREFIX=$p; return 0; }
    done
    # Toolchains people commonly unpack rather than install.
    for p in "${N64_INST:-/opt/libdragon}/bin/mips64-elf-" \
             "${N64_GCCPREFIX:-/nonexistent}/bin/mips64-elf-"; do
        [ -x "${p}as" ] && { MIPS_BINUTILS_PREFIX=$p; return 0; }
    done
    return 1
}

if ! detect_mips_prefix; then
    warn "No N64 MIPS assembler found."
    say  "   The game's music sequences are assembled with it. Without it the"
    say  "   build cannot complete, so this one is required."
    MIPS_PKG=""
    case "$PKG" in
        apt)    MIPS_PKG="binutils-mips-linux-gnu" ;;
        dnf)    MIPS_PKG="binutils-mips64-linux-gnu" ;;
        zypper) MIPS_PKG="cross-mips-binutils" ;;
        # Not in Arch's official repositories -- only the AUR has it, so this
        # needs an AUR helper, handled separately below.
        pacman) MIPS_PKG="" ;;
    esac
    if [ -n "$MIPS_PKG" ] && [ "$CAN_INSTALL" = 1 ] && ask "Install '$MIPS_PKG'?" Y; then
        install_packages "$MIPS_PKG" || true
    elif [ "$PKG" = "pacman" ]; then
        # AUR helpers refuse to run as root and do their own privilege
        # escalation, so this one is invoked directly rather than via $SUDO.
        AUR_HELPER=""
        for h in paru yay; do
            command -v "$h" >/dev/null 2>&1 && { AUR_HELPER=$h; break; }
        done
        if [ -n "$AUR_HELPER" ] && [ "$IS_ROOT" = 0 ] \
           && ask "Install 'mips64-elf-binutils' from the AUR using $AUR_HELPER?" Y; then
            run_step "building mips64-elf-binutils from the AUR" 0 "" -- \
                "$AUR_HELPER" -S --needed --noconfirm mips64-elf-binutils || true
        fi
    fi
    detect_mips_prefix || die "A MIPS assembler is required and could not be installed automatically.

       Debian / Ubuntu :  sudo apt install binutils-mips-linux-gnu
       Fedora          :  sudo dnf install binutils-mips64-linux-gnu
       Arch            :  install 'mips64-elf-binutils' from the AUR
       Anywhere        :  the libdragon toolchain, from
                          https://github.com/DragonMinded/libdragon

       If you already have one somewhere unusual, point at it:
         MIPS_BINUTILS_PREFIX=/path/to/mips64-elf- ./install.sh"
fi
export MIPS_BINUTILS_PREFIX
ok "N64 assembler : ${MIPS_BINUTILS_PREFIX}as"

[ -n "$JOBS" ] || JOBS=$(nproc 2>/dev/null || echo 4)
ok "Parallel jobs : $JOBS"

# ===========================================================================
#  4. Get the source
# ===========================================================================

step "Getting the port's source code"

if [ "$ALREADY_CLONED" = 1 ]; then
    ok "Using the checkout this script lives in:"
    say "   $WORK_DIR"
elif [ -d "$WORK_DIR/.git" ]; then
    ok "Existing checkout found: $WORK_DIR"
    ask "Update it (git pull)?" Y && \
        run_step "git pull" 0 "" -- git -C "$WORK_DIR" pull --ff-only
else
    say "Cloning ${C_B}$REPO_URL${C_RST}"
    say "     to ${C_B}$WORK_DIR${C_RST}"
    if [ -n "$REPO_BRANCH" ]; then
        run_step "cloning the repository (branch $REPO_BRANCH)" 0 "" -- \
            git clone --depth 1 --branch "$REPO_BRANCH" "$REPO_URL" "$WORK_DIR"
    else
        run_step "cloning the repository" 0 "" -- \
            git clone --depth 1 "$REPO_URL" "$WORK_DIR"
    fi
fi

# The decompilation's Makefiles and python tooling resolve paths through
# $(CURDIR) and os.getcwd(), neither of which survives an unquoted space. A
# symlink does not help -- both resolve to the real physical path. Better to
# say so here than to fail cryptically four minutes into asset extraction.
case "$WORK_DIR" in
    *\ *|*$'\t'*)
        die "The build path contains a space:

           $WORK_DIR

       The underlying decompilation's build system cannot handle that. Move
       this script somewhere without spaces in the path (or pass --dir with
       a space-free path) and run it again." ;;
esac

cd "$WORK_DIR"
[ -f Makefile.psp ] || die "$WORK_DIR does not look like the port (no Makefile.psp)."

# ===========================================================================
#  5. Find and verify the ROM
# ===========================================================================

step "Looking for your Ocarina of Time ROM"

# Byte-swapped dumps (.v64 16-bit swapped, .n64 32-bit little endian) are
# common and trivially convertible, so accept them rather than sending people
# away to find another tool.
normalize_rom() {
    python3 - "$1" "$2" <<'PY'
import sys, struct
src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, 'rb').read())
if len(data) < 0x1000:
    print('NOT_A_ROM'); sys.exit(0)
magic = struct.unpack('>I', data[:4])[0]
if magic == 0x80371240:
    order = 'z64, big endian'
elif magic == 0x37804012:
    data[0::2], data[1::2] = data[1::2], data[0::2]
    order = 'v64, byte-swapped -> converted'
elif magic == 0x40123780:
    for i in range(0, len(data), 4):
        data[i:i+4] = data[i:i+4][::-1]
    order = 'n64, little endian -> converted'
else:
    print('NOT_A_ROM'); sys.exit(0)
open(dst, 'wb').write(bytes(data))
print(order)
PY
}

rom_info() {
    python3 - "$1" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read(0x40)
name = d[0x20:0x34].decode('ascii', 'replace').strip()
country = chr(d[0x3E])
ver = d[0x3F]
region = {'P': 'PAL/Europe', 'E': 'NTSC/USA', 'J': 'NTSC/Japan'}.get(country, country)
print(f"{name}, {region}, version 1.{ver}")
PY
}

BASEROM_DIR="$WORK_DIR/baseroms/$GAME_VERSION"
BASEROM="$BASEROM_DIR/baserom.z64"
BASEROM_DEC="$BASEROM_DIR/baserom-decompressed.z64"
mkdir -p "$BASEROM_DIR"

md5_of() { md5sum "$1" | cut -d' ' -f1; }

ROM_READY=0
if [ -f "$BASEROM" ] && [ "$(md5_of "$BASEROM")" = "$ROM_MD5_COMPRESSED" ]; then
    ok "Verified ROM already in place: baseroms/$GAME_VERSION/baserom.z64"
    ROM_READY=1
elif [ -f "$BASEROM_DEC" ] && [ "$(md5_of "$BASEROM_DEC")" = "$ROM_MD5_DECOMPRESSED" ]; then
    ok "Verified decompressed ROM already in place."
    ROM_READY=1
fi

if [ "$ROM_READY" = 0 ]; then
    while :; do
        CANDIDATES=()
        if [ -n "$ROM_ARG" ]; then
            if [ ! -f "$ROM_ARG" ]; then
                warn "--rom: no such file: $ROM_ARG"
                printf '\n'
                if ask "Fix the file (or the path) and try again?" Y; then
                    continue
                fi
                die "Cannot continue without a ROM."
            fi
            CANDIDATES=("$ROM_ARG")
        else
            # Next to the script first -- that is where the instructions say to put
            # it -- then inside the checkout.
            for dir in "$SCRIPT_DIR" "$WORK_DIR" "$BASEROM_DIR"; do
                for f in "$dir"/*.z64 "$dir"/*.n64 "$dir"/*.v64 \
                         "$dir"/*.Z64 "$dir"/*.N64 "$dir"/*.V64; do
                    [ -f "$f" ] && CANDIDATES+=("$f")
                done
            done
        fi

        if [ ${#CANDIDATES[@]} -eq 0 ]; then
            warn "No ROM found. Looks like it wasn't placed there after all -- put"
            warn "your own Ocarina of Time ROM in:"
            printf '\n      %s%s%s\n\n' "$C_B" "$SCRIPT_DIR" "$C_RST"
            say "   It must be the PAL 1.0 (Europe) release, as .z64, .n64 or .v64,"
            say "   with this MD5 once converted to .z64:"
            say "       ${C_GRY}$ROM_MD5_COMPRESSED${C_RST}"
            printf '\n'
            if ask "Added it now -- should I look again?" Y; then
                continue
            fi
            die "Cannot continue without a ROM.
       Run this script again once it's there, or point straight at it:
           ./install.sh --rom /path/to/rom.z64"
        fi

        say "Checking ${#CANDIDATES[@]} candidate file(s):"
        PICKED=""
        for c in "${CANDIDATES[@]}"; do
            tmp="$LOG_DIR/rom-normalized.z64"
            res=$(normalize_rom "$c" "$tmp" 2>>"$LOG_FILE") || res="NOT_A_ROM"
            if [ "$res" = "NOT_A_ROM" ]; then
                warn "$(basename "$c") -- not an N64 ROM image"
                rm -f "$tmp"; continue
            fi
            sum=$(md5_of "$tmp")
            if [ "$sum" = "$ROM_MD5_COMPRESSED" ]; then
                ok "$(basename "$c") -- PAL 1.0 ($res)"
                mv -f "$tmp" "$BASEROM"
                PICKED=1; break
            elif [ "$sum" = "$ROM_MD5_DECOMPRESSED" ]; then
                ok "$(basename "$c") -- PAL 1.0, already decompressed ($res)"
                mv -f "$tmp" "$BASEROM_DEC"
                PICKED=1; break
            else
                warn "$(basename "$c") -- wrong version: $(rom_info "$tmp")"
                rm -f "$tmp"
            fi
        done

        if [ -z "$PICKED" ]; then
            warn "None of the files found is the supported ROM."
            say "   This port needs PAL version 1.0 specifically (MD5 $ROM_MD5_COMPRESSED)."
            say "   Other regions and revisions lay their data out differently and"
            say "   will not work."
            printf '\n'
            if ask "Put the right ROM in $SCRIPT_DIR and look again?" Y; then
                continue
            fi
            die "Cannot continue without the correct ROM."
        fi

        break
    done
    ok "ROM installed into baseroms/$GAME_VERSION/"
fi

# ===========================================================================
#  6. Asset toolchain
# ===========================================================================

step "Preparing the asset toolchain"

# Built directly rather than through the decomp's own `make venv`, because that
# target is not in the exemption list on the `include $(SEGMENTS_DIR)/Makefile`
# line in its Makefile (only clean/assetclean/distclean/setup are). Asking make
# for `venv` therefore drags in makefile-remaking, which needs tools built
# below, which is a circle -- and `python3 -m venv` is all that recipe does
# anyway.
if [ -x .venv/bin/python3 ]; then
    ok "python environment already present (.venv)"
else
    run_step "creating python environment" 0 "" -- python3 -m venv .venv
    run_step "installing python packages (~15 of them)" 0 "" -- \
        ./.venv/bin/python3 -m pip install -q --upgrade pip
    run_step "installing python packages" 0 "" -- \
        ./.venv/bin/python3 -m pip install -q --upgrade -r requirements.txt
fi
PY_VENV="$WORK_DIR/.venv/bin/python3"
[ -x "$PY_VENV" ] || die "The python environment was not created (.venv)."

# The decomp's own small C helpers. Needed even though this port never links an
# N64 ROM: any `make VERSION=...` invocation that is not one of the exempted
# goals above includes $(SEGMENTS_DIR)/Makefile, which mkspecrules generates --
# so without these, the audio-asset step below dies with "No such file or
# directory" long before it reaches anything to do with audio.
#
# Named individually rather than via `make -C tools`: that default target also
# downloads the IDO recompiler and an EGCS cross-compiler, several hundred MB
# that exist purely to rebuild the byte-perfect N64 ROM.
run_step "building host build tools" 0 "" -- \
    make -C tools -j"$JOBS" bin2c mkdmadata mkldscript mkspecrules preprocess_pragmas vtxdis

# The audio asset tools (sfc/sbc/atblgen/sfpatch/afile_sizes/sampleconv), which
# turn the extracted XML and WAV descriptors into soundfonts and sample banks.
run_step "building host asset tools" 0 "" -- make -C tools/audio -j"$JOBS"

# n64texconv (+ build_from_png/build_jfif, which link against it): the actual
# asset EXTRACTION step below imports it directly via ctypes as a .so, not
# just the later PSP build -- tools/assets/extract's texture/skeleton/display-
# list decoders all go through it. Its own Makefile is plain C/gcc, no IDO or
# EGCS involved, so this is safe to build the same targeted way as above.
run_step "building host texture-conversion tools" 0 "" -- make -C tools/assets -j"$JOBS"

# ===========================================================================
#  7. Extract the assets from your ROM
# ===========================================================================

step "Extracting assets from your ROM"

say "${C_GRY}The slow part -- several minutes. Everything comes out of your own"
say "ROM; nothing is downloaded.${C_RST}"

E="extracted/$GAME_VERSION"

if [ -f "$BASEROM_DEC" ]; then
    ok "ROM already decompressed"
else
    run_step "decompressing the ROM" 0 "" -- \
        "$PY_VENV" tools/decompress_baserom.py "$GAME_VERSION"
fi

if [ -d "$E/baserom" ]; then
    ok "ROM already split into files"
else
    # One file per DMA segment, and baseroms/<version>/segments.csv lists every
    # one of them (plus a header row) -- so the total is known up front rather
    # than guessed, and the bar is honest.
    SEG_TOTAL=$(( $(wc -l < "baseroms/$GAME_VERSION/segments.csv") - 1 ))
    mkdir -p "$E/baserom"
    run_step_watch "splitting the ROM into $SEG_TOTAL files" "$E/baserom" "$SEG_TOTAL" -- \
        "$PY_VENV" tools/extract_baserom.py "$BASEROM_DEC" "$E/baserom" -v "$GAME_VERSION"
fi

if [ -d "$E/assets/objects" ]; then
    ok "assets already extracted"
else
    mkdir -p "$E/assets"
    run_step_watch "extracting assets: textures, scenes, models" "$E/assets" 0 -- \
        "$PY_VENV" -m tools.assets.extract "$E/baserom" "$E" -v "$GAME_VERSION" -j"$JOBS"
fi

if [ -d "$E/incbin" ]; then
    ok "binary blobs already extracted"
else
    run_step "extracting binary blobs" 0 "" -- \
        "$PY_VENV" tools/extract_incbins.py "$E/baserom" "$E/incbin" -v "$GAME_VERSION"
fi

if [ -d "$E/text" ]; then
    ok "text already extracted"
else
    run_step "extracting text" 0 "" -- \
        "$PY_VENV" tools/extract_text.py "$E/baserom" "$E/text" -v "$GAME_VERSION"
fi

if [ -d "$E/assets/audio/soundfonts" ]; then
    ok "audio already extracted"
else
    mkdir -p "$E/assets/audio"
    run_step_watch "extracting audio: samples, soundfonts, sequences" "$E/assets/audio" 0 -- \
        "$PY_VENV" tools/extract_audio.py -b "$E/baserom" -o "$E" -v "$GAME_VERSION" --read-xml
fi

ok "Assets extracted."

# ===========================================================================
#  8. Build
# ===========================================================================

step "Building the PSP executable"

# --- 8a. Audio intermediates ------------------------------------------------
#
# The port repacks the game's audio itself, but the step that turns extracted
# XML/WAV descriptors into soundfonts, sample banks and assembled sequences is
# the decompilation's own and lives in its N64 Makefile. Only a handful of its
# targets are needed here -- notably not the N64 ROM.
AUDIO_TARGETS=(
    "build/$GAME_VERSION/assets/audio/samplebank_table.h"
    "build/$GAME_VERSION/assets/audio/soundfont_table.h"
    "build/$GAME_VERSION/assets/audio/sequence_font_table.s"
)
for x in "$E"/assets/audio/samplebanks/*.xml; do
    [ -f "$x" ] || continue
    AUDIO_TARGETS+=("build/$GAME_VERSION/assets/audio/samplebanks/$(basename "$x" .xml).s")
done
run_step "preparing audio assets" 0 "" -- \
    make VERSION="$GAME_VERSION" -j"$JOBS" "${AUDIO_TARGETS[@]}"

# --- 8b. The PSP target -----------------------------------------------------
#
# Progress estimate: one unit per compiled translation unit, per packed scene
# blob and per audio blob. The build prints one line per unit, so counting
# them against this total gives a real percentage -- as long as the total
# itself is right, which took a real miss to get here: weighting scenes by
# COUNT ALONE (~101) badly undercounted psp/tools/make_scene_blob.sh's actual
# output, because scenes carry wildly different room counts (388 rooms total
# across those 101 scenes, and each room is its own compile+link+objcopy), so
# the bar sat pinned at its 99% cap for most of a real build instead of
# tracking it. Weighted by scene AND room count below instead.
N_C=$(grep -c '^PSP_C_FILES +=' Makefile.psp || echo 250)

# make_scene_blob.sh emits 4 log-matched commands per scene (compile scene.c,
# compile the shared identity-matrix stub, link, objcopy) plus 3 per room
# (compile, link, objcopy) -- see its "Scene blob" / "One blob per room"
# sections. `|| true` guards below: with pipefail+errexit active script-wide,
# an awk/grep that matches nothing would otherwise kill the whole install
# right here rather than just leaving a rough fallback number in place.
SCENE_ROOM_TOTALS=$(sh psp/tools/list_scenes.sh "$E" 2>/dev/null \
    | awk -F: '{scenes++; rooms+=$3} END{print scenes+0, rooms+0}') || true
N_SCENES=${SCENE_ROOM_TOTALS%% *}
N_ROOMS=${SCENE_ROOM_TOTALS##* }
[ -n "$N_SCENES" ] && [ "$N_SCENES" -gt 0 ] 2>/dev/null || N_SCENES=100
[ -n "$N_ROOMS" ]  && [ "$N_ROOMS"  -gt 0 ] 2>/dev/null || N_ROOMS=380

# Audio blobs: 3 log-matched commands per soundfont (compile, partial-link,
# objcopy -- sfpatch in between does not print a matched line), 2 each for
# sample banks and sequences (assembled then objcopy'd straight, no separate
# link step). Same --list-blobs call Makefile.psp's own AUDIO_BLOB_NAMES uses,
# so this cannot drift out of step with what actually gets built.
AUDIO_BLOB_LIST=$(python3 psp/tools/gen_audio_tables.py \
    --seq-table include/tables/sequence_table.h \
    --font-table "build/$GAME_VERSION/assets/audio/soundfont_table.h" \
    --bank-table "build/$GAME_VERSION/assets/audio/samplebank_table.h" \
    --audio-build-dir "build/$GAME_VERSION/assets/audio" --list-blobs 2>/dev/null) || true
N_SOUNDFONTS=$(printf '%s\n' "$AUDIO_BLOB_LIST" | grep -c '^Soundfont_') || true
N_SAMPLEBANKS=$(printf '%s\n' "$AUDIO_BLOB_LIST" | grep -c '^SampleBank_') || true
N_SEQ=$(printf '%s\n' "$AUDIO_BLOB_LIST" | grep -c '^seq') || true
N_SOUNDFONTS=${N_SOUNDFONTS:-38}
N_SAMPLEBANKS=${N_SAMPLEBANKS:-6}
N_SEQ=${N_SEQ:-109}

BUILD_UNITS=$(( N_C + 10 + N_SCENES*4 + N_ROOMS*3 \
              + N_SOUNDFONTS*3 + (N_SAMPLEBANKS + N_SEQ)*2 ))

run_step "compiling (~$BUILD_UNITS units, this takes a while)" \
    "$BUILD_UNITS" '^(psp-gcc|psp-g\+\+|psp-as|psp-ld|psp-objcopy)|^  BLOB' -- \
    make -f Makefile.psp -j"$JOBS"

[ -f EBOOT.PBP ] || die "The build finished but produced no EBOOT.PBP -- see $LOG_FILE"
[ -n "$(find blobs -name '*.bin' -print -quit 2>/dev/null)" ] \
    || die "The build produced no asset blobs -- see $LOG_FILE"
ok "EBOOT.PBP built ($(du -h EBOOT.PBP | cut -f1))"

# ===========================================================================
#  9. Package it up
# ===========================================================================

step "Packaging for your PSP"

OUT_ROOT="$INVOKE_DIR/$OUT_DIR_NAME"
OUT_GAME="$OUT_ROOT/PSP/GAME/$EBOOT_TITLE_DIR"
rm -rf "$OUT_ROOT"
mkdir -p "$OUT_GAME/blobs"

cp -f EBOOT.PBP "$OUT_GAME/"
cp -f blobs/*.bin "$OUT_GAME/blobs/"
# The engine's DMA layer still reads some things straight out of the ROM image,
# so the decompressed ROM ships next to the EBOOT under exactly the name
# psp/src/main.c passes to PspRom_Init().
cp -f "$BASEROM_DEC" "$OUT_GAME/oot-pal-1.0.z64"

REV=$(git -C "$WORK_DIR" rev-parse --short HEAD 2>/dev/null || echo "unknown")
cat >"$OUT_ROOT/README.txt" <<README
The Legend of Zelda: Ocarina of Time -- PSP port (PROTOTYPE)
===========================================================

WHAT THIS IS
    An unfinished, experimental port. The world renders and Link walks
    around. Most of the game does not exist yet. Crashes and graphical
    glitches are the normal state.

HOW TO INSTALL
    Connect your PSP in USB mode (or take the memory stick out) and copy the
    "PSP" folder from in here onto the root of the memory stick, letting it
    merge with the PSP folder already there.

    You should end up with:
        <memory stick>/PSP/GAME/$EBOOT_TITLE_DIR/EBOOT.PBP
        <memory stick>/PSP/GAME/$EBOOT_TITLE_DIR/oot-pal-1.0.z64
        <memory stick>/PSP/GAME/$EBOOT_TITLE_DIR/blobs/...

    Then start it from the PSP's Game menu. It also runs in PPSSPP: just
    open the EBOOT.PBP.

WHY IS THE .z64 THERE TOO?
    Because the port is not finished converting the game's data. Scenes,
    rooms and audio have already been repacked into blobs/ in a format the
    PSP reads natively -- but the ~380 object files (actor models, Link's
    model, gameplay_keep) have not, and the engine still reads those out of
    the ROM image while playing. Every asset transfer asks blobs/ first and
    only falls back to the .z64 for what is not there yet.

    So it is not a licence check or a copy of the game to run alongside: it
    is the part of the conversion that is still outstanding. Once objects
    are packed like scenes already are, this file goes away and the folder
    drops from ~85 MB to ~30 MB.

CONTROLS
    Analog stick    move
    SELECT          scene warp menu (jump to any area in the game)

A NOTE ON CONTENT
    Everything in blobs/ and the .z64 file was produced from YOUR OWN ROM,
    on your own machine. Do not redistribute this folder.

Built $(date -u '+%Y-%m-%d %H:%M UTC') from revision $REV
README

ok "Packaged into $OUT_ROOT ($(du -sh "$OUT_ROOT" | cut -f1))"

# ===========================================================================
#  Done
# ===========================================================================

printf '\n'; hr; printf '\n'
printf '   %s%sBuild complete.%s\n\n' "$C_GRN" "$C_B" "$C_RST"
say "Your ready-to-copy folder:"
printf '\n      %s%s%s\n\n' "$C_B" "$OUT_ROOT" "$C_RST"
say "Copy the ${C_B}PSP${C_RST} folder inside it onto the root of your memory stick,"
say "merging with the one already there, then launch ${C_B}OoT PSP${C_RST} from the"
say "PSP's Game menu."
printf '\n'
say "To try it on this computer instead:"
say "   ${C_GRY}ppsspp \"$OUT_GAME/EBOOT.PBP\"${C_RST}"
printf '\n'
say "${C_GRY}Full log: $LOG_FILE${C_RST}"
say "${C_GRY}Remember: this is a prototype. Bugs are the normal state.${C_RST}"
printf '\n'
