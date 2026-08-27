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

If you would rather drive it yourself, or you are on macOS, here is everything
`install.sh` does, spelled out.

**1. System packages**

These build the host-side tools (asset extraction, blob packing, ROM
verification). Package names vary a bit by distro; the ones below are for
Debian/Ubuntu (`apt`), with Fedora (`dnf`), Arch (`pacman`) and openSUSE
(`zypper`) equivalents in brackets.


```sh
# Debian / Ubuntu / Mint:
sudo apt install git make build-essential python3 python3-venv python3-pil \
    curl tar coreutils libxml2-dev gawk fonts-dejavu-core

# Fedora / RHEL:
sudo dnf install git make gcc python3 python3-pillow curl tar coreutils \
    libxml2-devel gawk dejavu-sans-mono-fonts

# Arch / Manjaro:
sudo pacman -S --needed git make base-devel python python-pillow curl tar \
    coreutils libxml2 gawk ttf-dejavu
```

**2. MIPS binutils** — needed to link the audio assets (this port never
compiles a full N64 ROM, so no N64 C compiler is required, just the linker/
objcopy from a MIPS binutils):

```sh
sudo apt install binutils-mips-linux-gnu     # Debian / Ubuntu
sudo dnf install binutils-mips64-linux-gnu   # Fedora / RHEL
```
Arch/openSUSE don't package this; build it yourself or grab it from the
[decomp's own toolchain guide](docs/decompiling_tutorial.md).

**3. PSPSDK (pspdev)** — the actual PSP cross-compiler toolchain. `psp-config`
must end up on `PATH`. The easiest route is a prebuilt release, no building
from source required:

- Releases: https://github.com/pspdev/pspdev/releases (grab the
  `pspdev-ubuntu-latest-x86_64.tar.gz` asset, or the `-debian-`/`-fedora-`/
  `-arm64` variant that matches your system)
- Unpack it anywhere, e.g. `~/pspdev`, then add it to your `PATH`:
  ```sh
  curl -fLO https://github.com/pspdev/pspdev/releases/latest/download/pspdev-ubuntu-latest-x86_64.tar.gz
  tar -xzf pspdev-ubuntu-latest-x86_64.tar.gz -C ~
  export PSPDEV="$HOME/pspdev"
  export PATH="$PSPDEV/bin:$PATH"   # add this line to your shell rc file too
  ```
- On macOS, install it via Homebrew (`brew install pspdev/tap/pspdev` — see
  [pspdev/pspdev](https://github.com/pspdev/pspdev) for the current tap name)
  or build it from source per that repo's instructions.
- Verify with `psp-config --pspsdk-path` — it should print a path, not an
  error.

**4. Python packages** — the decomp's asset/build tooling, installed into a
venv so it doesn't touch your system Python:

```sh
python3 -m venv .venv
./.venv/bin/pip install --upgrade pip
./.venv/bin/pip install -r requirements.txt
```
[`requirements.txt`](requirements.txt) pulls in `crunch64`, `ipl3checksum`,
`pyyaml`, `pygfxd`, `mapfile-parser`, `pyelftools`, `rabbitizer`,
`spimdisasm` and a handful of asm-differ/decomp-permuter helpers.

**5. Your own ROM** — a legally obtained cartridge dump, **PAL version 1.0**
specifically (other regions/revisions lay out their data differently and will
not work). `.z64`, `.n64` and `.v64` are all fine.

**6. Build**

```sh
# Place your ROM at baseroms/pal-1.0/baserom.z64, then extract assets
# (standard zeldaret/oot step, only needs to run once):
make setup VERSION=pal-1.0

# Build the PSP target:
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
