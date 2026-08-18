# OoT PSP Port

A homebrew port of *The Legend of Zelda: Ocarina of Time* to the PlayStation Portable,
built on top of the [zeldaret/oot](https://github.com/zeldaret/oot) decompilation. The
architecture follows [sm64-port-psp](https://github.com/blckbearx/sm64-port-psp): a
single native PSP loop drives the real decompiled engine directly, instead of emulating
the N64's libultra OS threads.

```diff
- WARNING! -

Work in progress. The world renders, Link walks and a handful of actors are enabled
(see the Changelog below), but most of the game is not wired up yet. Expect crashes,
missing actors, and gaps in audio.
```
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
| `Makefile.psp` | This port's build entry point, kept deliberately separate from the decomp's own `Makefile` (see the comment at its top for why). |
| `src/`, `include/`, `assets/`, `data/`, `spec/` | The **zeldaret/oot decompilation** — reverse-engineered N64 source, unmodified except for the narrow `#if TARGET_PSP` hooks the port needs. |
| `baseroms/` | Only ROM **metadata** (checksums, segment layout) per supported game version — never the ROM itself. |
| `tools/`, `docs/` (top level) | The decomp project's own build tooling and documentation (compilers, asm-differ, asset docs, N64 build guide), unmodified. |
| `psp/docs/` | This port's own documentation: [`PORTING_PITFALLS.md`](psp/docs/PORTING_PITFALLS.md) (the accumulated N64→PSP bug list, read this before chasing a bug that smells familiar) and a mirror of the CloudModding OoT wiki. |
| `extracted/`, `build/`, `blobs/`, `EBOOT.PBP` | Build output. Gitignored, generated locally, never committed. |

## Building

Requires:

- The [PSPSDK](https://github.com/pspdev/pspdev) toolchain (`psp-config` must be on
  `PATH`).
- The standard build dependencies for the underlying decomp (see [the decomp's own
  build guide](docs/decompiling_tutorial.md) if you need the N64 toolchain too — the
  PSP build does not need it, only PSPSDK).
- Your own PAL 1.0 OoT ROM.

```sh
# 1. Place your ROM at baseroms/pal-1.0/baserom.z64, then extract assets
#    (standard zeldaret/oot step, only needs to run once):
make setup VERSION=pal-1.0

# 2. Build the PSP target:
make -f Makefile.psp -j8
```

This produces `EBOOT.PBP` plus a `blobs/` directory that must sit next to it on the PSP
(or in PPSSPP's game folder). Both are gitignored — do not commit build output.

## Credits

- [zeldaret/oot](https://github.com/zeldaret/oot) — the decompilation this port is
  built on.
- [sm64-port-psp](https://github.com/blckbearx/sm64-port-psp) — architectural blueprint
  for the single-loop PSP port style.
- [libultraship](https://github.com/HarbourMasters/libultraship) and
  [DaedalusX64](https://github.com/DaedalusX64/daedalus) — referenced for specific
  porting mechanisms (see `psp/docs/PORTING_PITFALLS.md` for details on what was
  learned from each).
