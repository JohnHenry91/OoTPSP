# OoT PSP Port

A homebrew port of *The Legend of Zelda: Ocarina of Time* to the PlayStation Portable,
built on top of the [zeldaret/oot](https://github.com/zeldaret/oot) decompilation. The
architecture follows [sm64-port-psp](https://github.com/blckbearx/sm64-port-psp): a
single native PSP loop drives the real decompiled engine directly, instead of emulating
the N64's libultra OS threads.

**No game assets or ROMs are distributed here.** This repository is source code only.
You need your own legally-obtained retail PAL 1.0 ROM to build and extract assets
locally; see [Building](#building) below.

Not affiliated with or endorsed by Nintendo.

## Status

Full session-by-session progress, architecture decisions and open bugs are tracked in
[CHANGELOG.md](CHANGELOG.md) — read it bottom-to-top for the current state. Short
version: the Market and Kakariko Graveyard render correctly, Link walks around, and the
first world actors (grass, crates, trees, a patrolling dog) are enabled and working.

## Repository layout

This repo mixes three kinds of code — here's what's what:

| Path | What it is |
|---|---|
| `psp/` | **This port's own code.** PSP main loop, libultra/libu64 shim, the F3DEX2-to-sceGu graphics backend, and the build tools under `psp/tools/` (scene blob packer, texture converters, ...). Everything new lives here. |
| `install.sh` | One-shot installer: checks the system, installs the toolchains, extracts the assets from your ROM and builds a copy-to-your-memory-stick folder. See [Building](#building). |
| `Makefile.psp` | This port's build entry point, kept deliberately separate from the decomp's own `Makefile` (see the comment at its top for why). |
| `src/`, `include/`, `assets/`, `data/`, `spec/` | The **zeldaret/oot decompilation** — reverse-engineered N64 source, unmodified except for the narrow `#if TARGET_PSP` hooks the port needs. |
| `baseroms/` | Only ROM **metadata** (checksums, segment layout) per supported game version — never the ROM itself. |
| `tools/`, `docs/` (top level) | The decomp project's own build tooling and documentation (compilers, asm-differ, asset docs, N64 build guide), unmodified. |
| `psp/docs/` | This port's own documentation: [`PORTING_PITFALLS.md`](psp/docs/PORTING_PITFALLS.md) (the accumulated N64→PSP bug list, read this before chasing a bug that smells familiar) and a mirror of the CloudModding OoT wiki. |
| `extracted/`, `build/`, `blobs/`, `EBOOT.PBP` | Build output. Gitignored, generated locally, never committed. |

## Building

### The easy way

There is a single script that does everything: it checks your system, installs the
toolchains, clones this repository, extracts the assets from your ROM and leaves you a
folder to copy onto your memory stick.

Download it, put your own ROM in the same folder, and run it:

```sh
curl -fLO https://raw.githubusercontent.com/JohnHenry91/OoTPSP/main/install.sh
chmod +x install.sh
./install.sh
```

It asks before it installs anything, and explains what each package is for. Expect it to
take half an hour or so: the PSP toolchain alone is a ~300 MB download, and pulling the
assets out of the ROM is genuinely slow.

When it finishes you get a `psp-ready/` folder. Copy the `PSP` folder from inside it onto
the root of your memory stick, let it merge with the one already there, and start
*OoT PSP* from the PSP's Game menu. It runs in [PPSSPP](https://www.ppsspp.org/) too —
just open the `EBOOT.PBP`.

**Requirements**

- A 64-bit Linux system. The package installer knows apt, dnf, pacman and zypper, so
  Debian/Ubuntu/Mint, Fedora/RHEL, Arch/Manjaro and openSUSE are all handled. On other
  distributions it tells you what to install and stops rather than guessing. Windows
  works through WSL2 (which is just Ubuntu as far as the script is concerned). macOS is
  not supported by the script — use the manual route below.
- Your own legally obtained cartridge dump, **PAL version 1.0** specifically. Other
  regions and revisions lay their data out differently and will not work. `.z64`, `.n64`
  and `.v64` are all accepted; byte-swapped dumps get converted automatically, and the
  result is checked against a known MD5 before anything else happens.

Nothing is downloaded except open-source toolchains and this repository. Your ROM stays
on your machine, and so does everything built from it.

Useful flags: `--yes` for an unattended run, `--rom PATH` to point straight at a ROM,
`-j N` to pick the number of build jobs, `--help` for the rest.

### The manual way

If you would rather drive it yourself, or you are on macOS:

Requires the [PSPSDK](https://github.com/pspdev/pspdev) toolchain (`psp-config` on
`PATH`), a MIPS binutils for the audio assets (`binutils-mips-linux-gnu` or similar —
no N64 C compiler is needed, this port never builds the N64 ROM), the decomp's usual
Python tooling, and your own PAL 1.0 ROM.

```sh
# 1. Place your ROM at baseroms/pal-1.0/baserom.z64, then extract assets
#    (standard zeldaret/oot step, only needs to run once):
make setup VERSION=pal-1.0

# 2. Build the PSP target:
make -f Makefile.psp -j8
```

This produces `EBOOT.PBP` plus a `blobs/` directory that must sit next to it on the PSP
(or in PPSSPP's game folder), along with the decompressed ROM as `oot-pal-1.0.z64`. All
of it is gitignored — do not commit build output.

## Credits

- [zeldaret/oot](https://github.com/zeldaret/oot) — the decompilation this port is
  built on.
- [sm64-port-psp](https://github.com/blckbearx/sm64-port-psp) — architectural blueprint
  for the single-loop PSP port style.
- [libultraship](https://github.com/HarbourMasters/libultraship) and
  [DaedalusX64](https://github.com/DaedalusX64/daedalus) — referenced for specific
  porting mechanisms (see `psp/docs/PORTING_PITFALLS.md` for details on what was
  learned from each).

The bulk of the PSP-side porting work (`psp/`, the `#if TARGET_PSP` hooks, and the
bug-hunting behind them) was done with heavy use of [Claude Code](https://claude.com/claude-code).
