# Changelog — OoT PSP port

A PSP port built as a fork of the [zeldaret/oot](https://github.com/zeldaret/oot)
decompilation. Everything PSP-specific lives under `psp/`; the decomp's own sources are
edited in place only where the port genuinely needs it, always behind `#if TARGET_PSP`.

**No game assets are distributed here.** This repository contains source code only. You
supply your own retail ROM; the build extracts assets from it locally into `extracted/`
and `build/`, both of which are gitignored, as are the generated `blobs/` and
`EBOOT.PBP`. See "Building" below.

Versioning is `0.<phase>.<milestone>` — the port is pre-1.0 and the phase number tracks
the bring-up plan, not any notion of API stability.

---

## Unreleased — Phase 3: populating the world

Started once v0.2.0 proved the game renders and Link walks correctly. The goal is
actors: NPCs, props, wildlife -- the things that make the Market look inhabited
rather than a stage set.

### Working

- **The actor-enablement path is proven end to end.** `z_actor_dlftbls_psp.c`
  declares every `<Name>_Profile` as a weak alias to a no-op dummy actor; an actor
  whose real `.c` is added to `Makefile.psp` gets a strong symbol that the linker
  prefers automatically. Enabling an actor is therefore just that one line plus
  whatever objects/engine sources it references -- no table edits, no overlay DMA.
  The linker's undefined-reference list names exactly what is missing.
- **Four world actors enabled and confirmed rendering in the Market:**
  `En_Kusa` (cuttable grass, 8 in the Market -- can be cut, drops nothing yet),
  `Obj_Kibako2` (crates), `En_Wood02` (trees/bushes), `En_Dog` (a stray dog that
  walks its patrol path). `En_Dog` is the significant one: it exercises
  `z_skelanime` and the path system (`src/code/z_path.c`), both previously
  untested outside Link, and both work correctly. That narrows what stands
  between the port and `En_Hy` (the Market's 13 townspeople, the actor that would
  actually populate the streets) to the message system alone, not animation.
- Promoted `src/code/z_bg_item.c` (`DynaPolyActor_Init` and friends) from stub to
  real source, pulled in by the dyna-poly actors above.

### Known issues found this phase

- **Back Alley House (kakariko3) showed full-screen rainbow static once**, covering
  the walls entirely, on first warping in. Not reproducible on a second warp to the
  same scene (rendered correctly, including after toggling the fixed/pivot viewpoint
  back and forth). Investigated and ruled out by measurement rather than assumed
  fixed: `gPspSegAmbiguous8`/`gPspSegAmbiguous9` (the segment-8/9 pointer-collision
  counters from session 15) both read 0 while the corruption would have needed them
  non-zero, so that mechanism is not it. `SKYBOX_HOUSE_ALLEY` uses
  `SKYBOX_DRAW_256_3FACE`, the first 3-face skybox this port has exercised (Market and
  Link's House are both 4-face) -- worth another look if this recurs, but a one-shot,
  non-reproducible glitch on first load points more at a texture-cache/DMA timing
  issue than a structural bug in that code path. Left open rather than closed.
- **Zora's Fountain: the sky renders as a flat pale-yellow field**, not the
  expected sky/mountain skybox -- visible immediately on warping in, screenshot
  confirmed by the user. Not yet diagnosed. Given the session-16 skybox history,
  check the obvious suspects first: which `SKYBOX_*` id the scene declares, and
  whether this is the same texture-stride family of bug or something specific to
  this skybox's face count/layout (Zora's Fountain is an outdoor overworld scene,
  a different skybox shape than the Market's building skybox).

---

## v0.2.0 — Phase 2: the game renders and is playable to walk around in

First tagged state in which the port boots straight into a real scene, renders it
correctly, and lets you move Link around it.

### Working

- **Boot to gameplay.** Single-loop architecture modelled on `sm64-port-psp`, replacing
  the N64's scheduler/thread model. Boots directly to a chosen entrance
  (`PSP_BOOT_ENTRANCE`, default `ENTR_MARKET_DAY_0`).
- **Scene and room rendering** through a `gfx_pc`-style F3DEX2 interpreter on `sceGu`:
  geometry, textures (RGBA16/32, IA4/8/16, CI4/8), the RDP colour combiner reduced onto
  the GE's fixed-function texture environment, depth/alpha modes, scissor, viewport.
- **Native-endian asset blobs.** Scenes and rooms are converted to PSP-native data at
  build time (`psp/tools/make_scene_blob.sh`) instead of being byte-swapped at runtime —
  the single change that ended a long stream of endianness and segment bugs. 489 blob
  files covering the scene table.
- **Lighting.** Scene `EnvLightSettings`, ambient and directional lights, per-vertex
  shade.
- **Link.** Renders, is correctly textured and lit, animates, and walks. Skeleton
  vertices are transformed at `G_VTX` load time, matching N64 semantics.
- **Skybox** (`SKYBOX_DRAW_256_4FACE`) — which in this game supplies the Market's
  buildings and Link's House's interior walls, not just sky.
- **Pre-rendered background rooms** ("JPEG rooms"): decoded to `GU_PSM_5551` at build
  time, S2DEX replaced by a port-private blit opcode.
- **Cameras**, including the fixed / pivot pre-rendered viewpoints and C-Up toggling.
- **Collision** (`z_bgcheck`), actors, effects, `z_kankyo` environment update.
- **Frame pacing** tied to `cfb->updateRate`.
- **Scene warp menu** on SELECT — jump to any scene and room without rebuilding.

### Not implemented yet

- **Audio.** Nothing. No sequence player, no samples; `DmaMgr`'s audio paths are stubs.
- **Cutscenes.** `z_demo.c` is not in `Makefile.psp`. Note for whoever promotes it: the
  `CMD_*` macros in `include/cutscene_commands.h` were reversed for PSP byte order along
  with the scene commands, so check whether the interpreter reads those words as
  `u32`-and-shift rather than as byte fields.
- **HUD, pause menu, text boxes.** `z_parameter.c` / `z_message.c` are not compiled in.
- **Actor overlays.** Only `ovl_player_actor` is built; every other actor is stubbed.
- **Save/load** beyond the debug save used to reach a scene.
- **Fog.** `SHADER_OPT_FOG` reaches the combiner ID but the shader path is `#if 0`'d and
  the vertex-fog writes are commented out, so it cannot currently change a pixel.
- **`G_CULLDL`** is silently ignored. Deliberate: never culling is the safe direction,
  and a wrong frustum test would itself cause missing geometry.

### Known issues

- **Segment 8/9 disambiguation is a heuristic.** PSP pointers collide with N64 segment
  numbers 8 and 9; they are told apart by offset magnitude (native pointers seen at
  ≥343 KB, genuine segment references at ≤26 KB, threshold 64 KB). The collision-proof
  answer is libultraship's marker convention (bit 0 tags a segmented address), which
  needs tagging at blob-build time and at every `gSPSegment` site.
- **`besitu` crashes the game** when warped to. Not yet diagnosed.
- **Zora's Fountain's sky renders wrong** -- see the Phase 3 section above.
- **Two scenes fail to blob** (`bdan`, `bdan_boss`) — both need a private `gIdentityMtx`
  linked into the blob.
- The build has **no header dependency tracking**. After editing a `.h`, delete the
  objects and rebuild, or you will silently link stale code.

### Fixed in this release

Ordered roughly by how much time each cost.

- **`G_SETTIMG` stores `width - 1`** and the port kept the raw value, so `G_LOADTILE`'s
  row stride was 255 instead of 256 for a 256-wide image. Every source row was read one
  byte early — a one-texel-per-row shear, ~45° across a 32-row tile, applied identically
  to all 32 tiles of a skybox face. The Market panorama therefore looked *rotated*, and
  five sessions went into the transform chain before the shear was identified. The field
  decode was also one bit-width short (`C0(0, 10)` → `C0(0, 12)`).
- **Vertices were transformed at draw time, not at `G_VTX` load time.** OoT's skeleton
  display lists switch matrices between loading and drawing vertices, so limbs were
  transformed under a neighbouring limb's matrix — Link's sleeves floated and his chest
  landed on his chin.
- **`modelview_matrix_stack[-1]` aliased the tail of `rdp`.** The stack starts empty and
  OoT never pushes, so "the current modelview" and "the back half of the RDP state" were
  literally the same 64 bytes: every `gDPSetPrimColor` between a matrix load and its
  triangles overwrote that matrix.
- **Scene command byte order.** `CMD_BBBB` and friends pack a `u32` while the scene
  command structs read those same bytes as individual `u8`/`u16` fields — which only
  agree on big-endian. `envLightMode` was picking up its neighbour's value, so every
  scene asking for `LIGHT_MODE_SETTINGS` rendered black, Link included.
- **Segment 8/9 pointer collision** resolved native pointers as segmented addresses when
  their offset fell in an ambiguous band, crashing the pivot camera on a wild jump.
- **`G_QUAD` (0x07) had no case** in the display-list interpreter, and the switch has no
  `default`. The skybox is built entirely from `gSP1Quadrangle`, so it was silently
  dropped in its entirety.
- **`G_RDPSETOTHERMODE` was never handled**, so OoT's setup display lists were setting
  render modes into a void.
- **Player animation data is compiled in, not read from the ROM**, and its frames are
  big-endian — Link crumpled and bounced around the screen.
- **`buf_vbo` overflow** from an `==` bound that clipping could step straight over, and
  a `_clipped_vertices[18]` that is one triangle too small for a fully clipped polygon.
- **Colour combiner cycle 2** was dropped, leaving Link's tunic untinted white.
- **The game loop ran 1.81× too fast** because nothing consumed `cfb->updateRate`.
- **Per-frame debug file I/O** was a major stutter source and, once, a crash cause.

---

## Building

Requires the PSPSDK and a retail OoT PAL 1.0 ROM. The ROM is never committed and is not
distributed with this repository.

```sh
# place your own ROM where the decomp's extraction step expects it, then:
make -f Makefile.psp -j8
```

Produces `EBOOT.PBP` plus a `blobs/` directory that must sit next to it. Both are
gitignored — do not commit build output.

## Reference material

`psp/docs/PORTING_PITFALLS.md` is the accumulated N64→PSP pitfall list and is the first
thing to read before starting a comparable port or chasing a bug that smells familiar.
`psp/docs/reference/cloudmodding/` mirrors the CloudModding OoT wiki as raw wikitext.
