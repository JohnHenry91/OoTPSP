# Porting N64 decomp games to PSP: pitfalls and fixes

This file exists so the *next* port (Majora's Mask, or anything else built on this same
architecture) does not re-discover the following the hard way. It is organized by **bug class**,
not chronologically — each class names the general trap first, then how it showed up here
concretely. If you are starting a new N64-decomp-to-PSP port from this codebase, read this file
before writing any renderer or libultra-shim code.

Session-by-session narrative detail (how each bug was actually found) lives in project memory
(`project_oot_psp_port_v2*.md`); this file is the distilled, reusable lessons only.

## Architecture decisions that turned out right (keep doing these)

- **Fork the game's own decomp and add a `TARGET_PSP` branch**, guarded with `#if TARGET_PSP` /
  `#if !TARGET_PSP` in the real source files, rather than maintaining a separate skeletal
  reimplementation. Keeps the N64 build byte-identical and lets bug fixes benefit both targets.
  A *separate* `Makefile.psp` (not an `ifeq` branch inside the game's own N64 Makefile) is safer —
  the N64 build's IDO/GCC dual-path spec-driven graph is usually too fragile to entangle with a
  from-scratch target.
- **Collapse the N64 libultra OS-thread topology into a single-loop, direct-call architecture.**
  N64 games run `Idle`/`Main`/`Graph`/`Sched`/`IrqMgr`/`AudioMgr`/`PadMgr` as real cooperating OS
  threads exchanging message queues; none of that is needed on PSP if nothing overlaps. Call each
  one's per-frame update function directly from a single PSP loop. **Exception: keep `DmaMgr` a
  real thread** if the game's asset streaming genuinely overlaps loads with gameplay (it does, for
  room/scene/skeleton streaming) — PSP has real preemptive threads for this.
- **Reuse the N64->PC Fast3D interpreter (`gfx_pc.c`/`gfx_cc.c`) from an existing decomp-to-PC/PSP
  port (e.g. sm64-port-psp) rather than writing a display-list interpreter from scratch.** It is
  microcode-generic, not game-specific — the game-specific work is entirely in `gfx_scegu.c`'s
  shader/combiner classification tables, which must be rebuilt empirically per game (see the
  Color Combiner section below).
- **Native-blob asset pipeline, not raw ROM + hand-written endian fixups** (see the Assets
  section). This is the single highest-leverage architecture decision in this whole project;
  adopt it from day one on the next port instead of rediscovering it after several endian-bug
  sessions.
- **Runtime-pokeable debug globals for every "is subsystem X responsible?" question.**
  A plain `int gDebugXyz` read every frame, poked live via the PPSSPP WebSocket debugger, turns an
  A/B test into one memory write instead of a rebuild+relaunch cycle. Cheap enough to leave dozens
  in the tree permanently.

## 1. Endianness: two DIFFERENT bugs, easy to conflate

N64 is big-endian, PSP is little-endian. This shows up as two genuinely separate problems that
need separate fixes — conflating them wastes time.

**1a. Struct/array field byte order (scalar fields).** Raw ROM data DMA'd at runtime needs every
multi-byte field byte-swapped. The naive fix is a per-struct hand-written swap function
(`PspFixup*Endian`) with every field offset listed by hand. **This does not scale**: it is 10
functions, ~80 hand-listed offsets, 22 call sites in this project, and any field nobody thought to
list stays silently wrong — never faults, just yields a garbage count/pointer/coordinate that
breaks something unrelated much later. This was this project's single largest source of
time-consuming bugs across many sessions. See the Assets section for the actual fix (avoid this
problem class entirely via compile-time native data).

**1b. Texture pixel data (`u64` literal storage, a completely different mechanism).** Decomp
texture arrays are declared as `u64 name[] = { 0x1234..., ... }`. A little-endian compiler stores
each 8-byte literal's bytes in *reverse* order relative to what the N64 (big-endian) intended —
this has nothing to do with runtime DMA byte-swapping; it happens at **compile time**, for
**compiled-in** data. Detecting "does this pixel buffer need an 8-byte unswap" and "is this asset
raw-ROM data needing struct-field fixups" are **the same underlying question** ("did a
little-endian compiler produce these bytes from u64 literals, or did this arrive as raw N64
bytes") — a session lost time maintaining two separate copies of this discriminator that drifted
apart. **Use one predicate for both** (see `PspStaticAssetIsStatic()` below).

**1c. Packed-matrix layout has TWO valid encodings, and they are incompatible.** The real N64 `Mtx`
format packs two adjacent fixed-point matrix elements into one `s32` word
(`(IPART(xx) << 16) | IPART(yx)`, from `gdSPDefMtx`/`gbi.h`). That packing is implicitly
big-endian-shaped: on a big-endian machine the two `u16` halves land in element order; on
little-endian PSP the halves come out **pairwise swapped**. Meanwhile the game's own
`guMtxF2L`/`Matrix_MtxFToMtx` conversion functions write a **plain row-major u16 layout** — a
different, incompatible packing that the renderer's `gfx_sp_matrix` correctly expects. Any matrix
built via the first path (typically `gdSPDefMtx`-style hand-packed constants, e.g. an identity
matrix literal) and consumed via the second is silently wrong — reads back as a real matrix
(e.g. a plausible-looking but wrong permutation), not garbage, so it does not obviously fault.
**Fix pattern: never hand-pack a second copy of a constant matrix. Derive it through the one true
conversion path** (e.g. build `gIdentityMtx` from `gIdentityMtxF` via `Matrix_MtxFToMtx`, don't
declare it as a separate `gdSPDefMtx` literal). Audit every `gdSPDefMtx` user in the tree when
porting a new game.

## 2. Segmented addressing: PSP's own RAM aliases N64 segment numbers

N64's `SEGMENTED_TO_VIRTUAL`/`seg_addr()` resolves a display-list word as a segment-relative
reference when its top nibble names a populated segment slot (`gSegments[N] != 0`), and as a
literal virtual address otherwise. On real N64, native code pointers are KSEG0 (`0x80......`),
whose segment nibble reads as 0 — no ambiguity, since real pointers never look like segment
references.

**On PSP this assumption breaks structurally, not incidentally**: PSP user RAM is
`0x08800000..0x09FFFFFF`, so **every native pointer on this platform has 0x08 or 0x09 in its top
byte** — exactly the pattern of a segment-8 or segment-9 reference. If the game uses those
segments for anything (OoT does, for a couple of small Player textures), the instant a display
list sets that segment, every later native pointer that happens to start with the same byte is
silently reinterpreted as a segment offset and destroyed. This does not crash cleanly — it derails
display-list interpretation into whatever garbage a "resolved" bad pointer points at (in this
project: a bug-for-bug identical failure mode as bug class 5 below, non-terminating command
streams, because the corrupted pointer often lands in zeroed memory).

**Fix used here**: treat anything landing inside the actual RAM window as a native pointer,
*not* a segmented reference, regardless of what its top nibble looks like — with a magnitude-based
disambiguation (offset < ~1MB reads as a real segment reference — genuine segment references in
this game sat at low offsets like 0..26KB; native pointers via this segment were always > 1.6MB)
rather than a blanket range check, because a segment's *entire* legitimate reference range can
itself fall inside the RAM window (this happened for segment 9 here — every real segment-9
reference in the game is small, but the whole 24-bit offset range technically overlaps RAM).
**This exact discriminator must be applied in BOTH the display-list interpreter's `seg_addr()` AND
the game's own C-side `SEGMENTED_TO_VIRTUAL` macro** — they are logically the same operation and
will drift apart (and re-introduce the bug in whichever one wasn't fixed) if maintained twice.
Keep counters (`gPspSegAmbiguous8/9`) recording how often a value falls in the genuinely ambiguous
middle band, so the threshold's safety margin is measurable rather than assumed.

**Related trap**: the segment-slot table (`gSegments[]`) is written from TWO different conventions
depending on which code writes it — display-list `gSPSegment` commands store the raw pointer, C
code (`SEGMENTED_TO_VIRTUAL` callers) stores a KSEG0-relative offset (`ptr - 0x80000000`). On this
port both happen to work because of how PSP address masking cancels the offset, but do not "fix"
one convention without checking the other still agrees.

## 3. Stubbing an unimplemented function: two silent-failure shapes to check for

When a whole subsystem is out of scope and you write `void Foo(...) {}` as a placeholder, there
are exactly two ways this bites you later, and neither is `Foo` failing to link (a wrong signature
"links fine" — that is the trap, not evidence of safety):

- **The real function returns a value.** A no-op that should return something leaves `$v0`
  holding whatever the previous call left there — reliably non-zero garbage, not zero. Any caller
  that branches on the return value (`if (StubFunc(...) != 0) { ... }`) takes a essentially random
  branch, **every single call**. This class produced this project's single longest-standing bug: a
  camera-shake function stubbed as `void Quake_Update(void) {}` in place of the real
  `s16 Quake_Update(Camera*, ShakeInfo*)` left a garbage non-zero return that made the "apply
  screen shake" branch fire unconditionally, every frame, adding uninitialized stack data to the
  camera's look-at vector — presented as camera flicker/distortion across multiple sessions before
  being traced back to a stub signature mismatch.
- **The real function is void but writes through a pointer argument (an out-parameter).** A no-op
  body means the caller's struct is silently never populated — read as whatever was already there
  (often zero-initialized `.bss`, which LOOKS like a plausible "off" state and doesn't obviously
  break anything until the feature is expected to do something).

**Process to adopt from day one**: before stubbing anything, audit every no-op stub's REAL
prototype against what it's standing in for. Classify: void + no pointer args = genuinely safe;
returns a value = high risk (name every such stub, prioritize the ones on hot per-frame paths);
void + writable pointer args = also high risk, quieter (breaks a *feature*, not obviously a crash
or visible glitch). In this project, of 154 initially-cataloged no-op stubs, 29 returned a value
and 113 had writable pointer args — only 11 were actually safe no-ops. Always write a stub with
the REAL signature even when the body is trivial, specifically so this audit is possible later
(a `char x[64]` placeholder for a struct is the data-side version of the same trap — see below).

**Data-side twin of the same trap**: a `char placeholder[N]` standing in for a real typed global
(e.g. a struct or an `f32`) "links fine" the same way a wrong-signature function does — but a
write through the REAL type's smaller size (e.g. a `u16` field) corrupts up to `N` bytes of
unrelated storage around it, and the linker reports this as "multiple definition" (if the real
symbol also gets linked in later), not as a type error. Delete these the moment the real file they
placeholder for gets compiled in — don't leave them dangling "just in case."

**Stubbed display lists specifically: a zeroed buffer is not an empty display list.** A
`char gStubDL[64]` (`.bss`, all zeroes) decodes as an endless run of opcode `0x00`
(`G_NOOP`) with **no terminating `gsSPEndDisplayList`** — the interpreter never returns from it,
and recursion/command-count guards (see class 5) are the only thing that stops it from running
forever. If you must stub a display list, terminate it: overwrite the byte that decodes as the
`G_ENDDL` opcode (e.g. `#define STUB_ENDDL { [3] = (char)0xDF }` for a little-endian `u32` whose
top byte needs to read `0xDF`).

## 4. Segment-relative absolute address symbols are addresses, not storage

A symbol named like `D_0<segment>xxxxxx` in decomp source is the N64 spec's way of naming a
literal address that `SEGMENTED_TO_VIRTUAL` will resolve at runtime — it is NOT meant to have
storage allocated for it. If the linker is given a real definition for one (e.g. because a stub
generator naively creates a placeholder), every use of it silently gets a real (probably
zero-initialized or garbage) buffer instead of resolving through the segment table — for a matrix
address, that means every consumer silently gets an identity matrix or garbage transform instead
of the real segment-resolved one. Fix: supply it via a linker `--defsym` binding it to the literal
address instead of letting it get real storage (`-Wl,--defsym,D_0100xxxx=0x0100xxxx`). Generalizes
to any `D_0<seg>xxxxxx` symbol pattern in the source.

## 5. Non-terminating command/list streams: always add a bounded guard

Several unrelated root causes in this project (zeroed stub display lists, a segment-address
collision corrupting a jump target, a raw-ROM scene command list with an unresolved reference)
all produced the exact same *symptom*: an interpreter loop that never reaches its terminator and
runs until something else stops it (a stack overflow, a hard timeout, or in the worst case
successfully "runs" for hours reading zeroed memory as no-ops). **Any interpreter loop over
untrusted/DMA'd data should have a bounded command-count guard AND a bounded recursion-depth guard
from the start**, not added reactively after the first hang. Recording (not just capping) where
and how the guard tripped — the address, the raw word, the depth — is what actually let each of
these get root-caused instead of just contained; a guard that merely stops the hang without
recording turns a diagnosable bug into a recurring mystery.

## 6. Vertex/matrix transform TIMING, not just transform VALUES (the big one)

**This was the hardest bug in the whole project to find, because at every step the wrong thing
being checked measured as correct.** General lesson first, mechanism second.

On real N64 hardware, `G_VTX` transforms a vertex **immediately**, using whichever matrix is in
force at that exact moment in the display-list stream, and the transformed result is then fixed —
a later matrix change in the same list does not retroactively affect it. A PC/PSP port that
reuses a GPU's own transform hardware naturally wants to do the opposite for performance: store
raw object-space vertices, batch them, and let the GPU apply the "current" transform matrix once
at draw time. **These two are equivalent ONLY if every display list strictly follows "load matrix
-> load this object's vertices -> draw," with no other matrix load in between.**

Skeletal character rendering (`SkelAnime`-style limb hierarchies) does NOT follow that pattern —
it typically does "load matrix A (parent limb) -> load matrix A's vertices -> load matrix B
(child limb) -> ... -> eventually draw all the buffered geometry." Every vertex loaded under one
matrix but drawn after a *later* matrix load gets silently re-transformed under the wrong matrix.
The visual signature is distinctive and easy to misdiagnose: **pieces of one mesh appearing
pasted onto a neighboring limb** (in this project: a torso vertex, visible through a tunic's
V-neck, rendered near the head/chin) — plausible, rigid-looking transforms in the wrong place, not
exploded or missing geometry. It is also **strongly view-dependent**: a misplaced piece's
silhouette can coincide with its correct position from some camera angles and be obviously wrong
from others, which looks exactly like a self-occlusion (depth-test) bug and is easy to chase in
the wrong direction (this project spent real time checking depth-test enable conditions, alpha
test, and depth comparison direction before finding the actual cause).

**Why "check every value" doesn't find this**: every matrix involved measures as individually
correct (right position, right scale, right handedness/determinant) — the bug is purely in *when*
a correct matrix gets applied to *which* vertices, not in any matrix's content. Standard
"is the value right" instrumentation (dump the matrix, check its determinant, verify triangle
counts) will report everything fine, because it is.

**The diagnostic that actually finds it**: tag each transformed vertex, at LOAD time, with an
identifier for which matrix-load "slot" was current. At DRAW time (when the triangle is finally
submitted), compare that stored tag against the CURRENT matrix slot. Any mismatch is a vertex
whose transform is stale relative to when it will actually be drawn — count these
(`tri_stale_mtx` in this project's instrumentation) and, ideally, record a few concrete
(load_slot, draw_slot) pairs. A large, consistent count (this project: ~25% of a skeletal
character's triangles per frame, always the immediately-preceding matrix slot to the drawing one)
is close to unambiguous proof of this bug class.

**Fix**: transform vertices to world space in software at LOAD time (matching real hardware
`G_VTX` semantics) instead of leaving them in object space for the GPU to transform at draw time.
Concretely: apply the modelview matrix in the vertex-load function itself, store world-space
coordinates in the buffered vertex, and pin the GPU's own model-matrix register to identity
permanently (upload it once at init, never again) so the GPU only ever applies the
projection/view transform on top of already-world-space data. Cost: one extra 4x4 transform per
vertex (two matrix multiplies instead of one, since the composed model-view-projection can no
longer be precomputed once per matrix-load and reused for a whole batch).

**When to reach for this class of bug**: if every individually-measured value (matrix contents,
vertex data, triangle counts, positions) checks out correct across many rounds of instrumentation,
and the defect is a piece of geometry attached to the wrong place or the wrong nearby object,
stop looking for a wrong VALUE and start looking at the ORDER in which values are computed versus
consumed — especially any place a port introduced batching/deferral that the original engine's
"transform immediately" execution model didn't have.

## 7. Color combiner (RDP 2-cycle blend) reduction to a fixed-function GPU

The N64 RDP's color combiner is a fully general 2-cycle blend formula
(`(A-B)*C+D` per cycle, cycle 2 can feed on cycle 1's result). A fixed-function mobile GPU (PSP's
GE, and this applies equally to any similar fixed-function target) typically has only ONE texture
combine stage. There is no general algorithm that maps arbitrary RDP combines onto that — every
existing decomp-to-fixed-function-GPU port (this project, and independently confirmed via
DaedalusX64's PSP HLE renderer, which has ~200 hand-written per-combine-hash special cases) ends
up doing the same thing: reduce the combine to a small set of *recognized structural patterns*
(e.g. "flat color modulated by one texture," "two color registers multiplied together and then
added to a texture," etc.) and hand-write a fixed-function equivalent for each pattern as it's
found empirically by actually looking at what renders wrong. This is not a bug to eventually fix
generally — it is real, unavoidable, ongoing per-game work. Budget for it accordingly on the next
port rather than expecting a general solution.

**Concrete gotchas found doing this reduction here, likely to recur:**
- `*_ALPHA` combine-input selectors (`G_CCMUX_ENV_ALPHA` etc., "use this register's alpha channel
  as an RGB multiplier") have no equivalent in a reduced single-color-register model. Silently
  mapping them to `CC_0` (the reduction's "no contribution" sentinel) DISCARDS the entire term
  they're part of if the reduction algorithm collapses `X * CC_0` to nothing — approximate them by
  mapping to their parent register (`ENV_ALPHA -> CC_ENV`) instead, which is wrong but far less
  wrong than dropping the term.
- **Detect combine patterns structurally (compare the reduced operand pattern), not by numeric
  shader ID.** The numeric ID shifts whenever unrelated optimization flags (fog, alpha-test
  presence, etc.) change, so a hardcoded ID match silently stops matching the moment something
  else about the material changes. A structural pattern match survives that.
- **A 2-cycle combine of the shape `final = TEXEL0 * K1 + K2`** (where K1/K2 are derived from two
  flat color registers, common for "chrome"/fresnel-style shading) can be emulated exactly as a
  **two-pass draw**: pass 1 draws `TEXEL0 * K1` opaque via modulate, pass 2 re-draws the identical
  buffered geometry with texturing off, vertex color forced to flat K2, and additive blending.
  Both K1 and K2 need clamping back to a valid 0..1 (or 0..255) range before packing into a vertex
  color — the real formula can transiently exceed that range (measured up to ~2.0 here), and
  clamping loses a little dynamic range at the extremes but keeps hue correct through the bulk of
  the range, which is what actually mattered visually.
- When a two-pass (or any multi-draw) technique changes shared GPU state mid-technique (texture
  enable, blend mode, blend function) to do its two passes, **restore it from the actual cached
  state your renderer believes is current, not an assumed default** — directly calling the raw
  enable/disable API without going through whatever state-cache wrapper the rest of the renderer
  uses will desync that cache, and the NEXT unrelated draw that trusts the cache (skips
  re-enabling something because the cache says it's already on) will render wrong. Confirmed
  regression here: doing this made a later, unrelated draw come out solid white.
- **Multi-texture combines (two RDP tiles blended simultaneously) have no representation at all**
  on a single-TMU fixed-function pipeline. Pick whichever texture matters most for legibility
  (usually the one that changes per-draw, not a static overlay) and apply the same flat-color-pair
  approximation to it; a true simultaneous dual-texture blend is not achievable without shaders.
- **A single global per-tile texture-state struct, when the RDP addresses two tiles
  independently (`gDPLoadMultiBlock`, common for a texture + a static overlay/shine layer),
  causes tile 1 to silently reuse tile 0's stride/dimensions from whatever was loaded last** —
  this must be a `[2]`-indexed struct (or however many tiles the target microcode supports), not
  one shared instance, checked at every read AND write site (the per-tile texture import
  functions, the tile-size setters, and any per-tile sampler-parameter loop — this project had the
  bug independently in three places sharing the same root cause).
- Real N64 RDP intensity-only texture formats (`G_IM_FMT_I`, i.e. `I4`/`I8`) output **alpha equal
  to intensity**, not a constant opaque 255 — a naive texture-format importer that hardcodes alpha
  to 255 for these formats will be visibly wrong (too opaque / wrong blending) anywhere a combine
  samples that alpha channel (fading edges, masking, etc.).

## 8. Assets: compile-time native conversion beats runtime endian-fixup, decisively

This is the single highest-value strategic lesson from this project, worth internalizing before
writing a single line of asset-loading code on the next port.

A from-scratch N64 decomp already ships every scene/room/actor asset as ordinary **typed C
source** with **real C symbol references** between files (a room's display list can reference
`&SomeScene_VtxList[12]` directly, or `ARRAY_COUNT(...)` on a compile-time-sized array) — not
segment-relative runtime addresses. This means the asset-porting problem has a much better
solution available than it first appears to: **link each scene/room's assets at their own N64
segment base address (e.g. scene = `0x02000000`, room = `0x03000000`) and extract the linked
result as a flat binary.** Every internal pointer in that binary is then a genuine N64 segment
address, which the existing `SEGMENTED_TO_VIRTUAL` machinery already knows how to handle, and the
data is native-endian because the target compiler produced it. **Both major bug classes (endian
mismatches AND unresolved cross-references) disappear simultaneously**, because they were the same
underlying problem (raw ROM bytes with no linker pass over them) wearing two faces.

Load these pre-linked blobs from files shipped next to the executable, hooked at the single
lowest-level DMA read function every asset transfer funnels through (not at each call site — a
game has many call sites that request assets and it's easy to miss one; one hook, one guaranteed
coverage point). Key the blob registry on the same identifier (ROM file start offset) both the
compiled-in and DMA'd paths already use, so nothing else in the game needs to know or care which
path served a given asset.

**One predicate answers "is this native-endian, already-linked data or raw ROM bytes"** for BOTH
purposes at once (whether struct-field endian fixups should run, AND whether texture pixel data
needs the u64-literal byte unswap from class 1b) — an address-range test against the
statically-linked data segment (`[_ftext, __bss_start)` typically covers compile-time-initialized
data) covers compiled-in assets; anything blob-loaded should register its filled address range
with the same predicate. **Do not maintain two copies of this discriminator** — this project lost
time exactly that way once (the fixup-guard version and the texture-unswap version drifted apart
after being written separately, at different times, for what turned out to be the identical
question).

**Practical build-tooling gotchas that cost time getting the "link at segment base, extract flat
binary" approach working, likely to recur:**
- All-zero data (e.g. an empty spawn-point list) defaults into `.bss` (NOBITS — contributes zero
  bytes to a flat `objcopy -O binary` extraction), silently shortening the output and shifting
  every pointer after that point. Force zero-initialized data to actually occupy file space
  (e.g. GCC's `-fno-zero-initialized-in-bss`).
- The very first bytes of the linked output must be the asset's own entry point/command list,
  because the consuming game code jumps to "the start of this segment" unconditionally — ensure
  the linker places that specific symbol first (`-fdata-sections` + an explicit linker-script/
  `--section-start` style placement), or the extracted blob starts with unrelated data (e.g.
  texture bytes) and the game tries to interpret those as commands.
- If per-child assets (e.g. rooms) reference the parent's assets (e.g. a shared scene texture),
  link each child together WITH the parent for that specific link invocation, not standalone.
- A build environment with a space in its path breaks tools relying on `getcwd()`/`$(CURDIR)` for
  path resolution (seen in both a Python asset toolchain and Make) — a symlink to a space-free
  path does NOT fix this, since those APIs resolve to the real physical path, not the symlink.
  Either avoid spaces in the working tree location for this kind of project, or use relative `-I`
  paths with an explicit `cd` into a space-free root before invoking the affected tool, and always
  double-quote path variables that might contain a space.

## 9. libultra semantics that are easy to get subtly wrong on a non-N64 target

- **Degenerate-size cache/DMA operations are a real, expected calling pattern, not an error
  case to reject.** Real N64 code frequently computes a cache-invalidation or DMA size as
  `end - start` where the two can legitimately be equal or (due to a rounding/alignment quirk)
  briefly produce a negative or zero result — libultra's real implementation treats `size <= 0` as
  a no-op and `size >= total cache size` as "everything," and callers rely on that being safe. A
  naive forwarding shim that passes the size straight to the host OS's equivalent API without this
  clamping turns an intended no-op into `(unsigned)-N` — several GB of nonsense — which either
  faults or (worse) silently corrupts a huge unrelated memory range while "succeeding." Any
  N64-libultra-semantics shim over a host OS API needs this clamping from day one, and a counter
  for how often a degenerate size is actually seen (very common — 580 occurrences in one boot on
  this project) so it's visible this isn't a rare edge case.
- **A blocking message-queue receive that's waiting for a producer your port doesn't have
  (typically the RCP-completion signal a Scheduler thread posts on real hardware) will simply
  never return once your port removes that thread.** This class of hang has a very specific,
  recognizable signature worth pattern-matching immediately instead of re-investigating each time:
  the game becomes completely unresponsive, there's no crash log, no elevated CPU usage (the
  thread is genuinely blocked, not spinning), and it happens at a very consistent point (teardown,
  or per-frame at a fixed spot) — that's a blocked `osRecvMesg` on a queue nothing posts to
  anymore, not a logic bug. Find every `osRecvMesg` call in the ported source, cross off the ones
  already guarded, and check the rest.
- **A "livelock" (frames frozen, but the main thread's PC is genuinely moving between samples,
  cycling through the same few functions) is a different failure class from the above and needs a
  different diagnostic**: sample the PC repeatedly rather than once. If it's cycling through
  `osCreateMesgQueue -> osSendMesg -> osRecvMesg` repeatedly, suspect a queue-table eviction bug in
  a from-scratch libultra message-queue shim (a fixed-size queue table that evicts round-robin
  when full will evict a still-live queue if a caller creates many short-lived queues rapidly,
  e.g. one on its own stack per call — the evicted queue's later operations then silently fail,
  and a retrying caller spins forever).
- **A controller-pak/rumble error code is not the same thing as a controller-read error code.**
  If porting the pad/controller shim, check that "no controller connected" maps to whatever error
  value the game's own input-handling code actually switches on for a *read* failure — mapping it
  to the wrong error family (e.g. a pak-access error code instead of a channel-response error
  code) can hit an unhandled-default case in the game's own switch statement, which some games
  route straight into their crash handler. Looks exactly like every frame silently deadlocking.
- **Debug/crash-log files under a PSP homebrew app's virtual memory-stick path
  (`ms0:/whatever.txt`) are genuinely useful and worth checking FIRST whenever the game
  "hangs"** — a real port's `Fault_AddHungupAndCrash` equivalent is often a deliberate infinite
  loop after logging the assert location, not a real hang; the log names the exact source
  file:line immediately, faster than any live debugging. Confirm you have this logging wired up to
  something you can actually read on the host before spending time on live PC-sampling techniques.

## 9b. GBI command fields that store a BIASED value (`width - 1`, `len - 1`, ...)

Several RDP commands encode a *count minus one*, because the field has to hold the
maximum value in one fewer bit. `G_SETTIMG` is the one that bit this port:

```c
/* include/ultra64/gbi.h */
#define gSetImage(pkt, cmd, fmt, siz, width, i)     \
    _g->words.w0 = (_SHIFTL(cmd, 24, 8) | _SHIFTL(fmt, 21, 3) |
                    _SHIFTL(siz, 19, 2) | _SHIFTL((width) - 1, 0, 12));
```

Storing the raw field as "the width" makes a 256-texel-wide image 255 texels wide.
Nothing notices until something reads a **sub-rectangle** out of that image, because
only then does the width become a *row stride*:

- `G_LOADBLOCK` copies a contiguous run, so stride and row length are the same
  number and the bias cancels out. Everything the port drew for months used
  `G_LOADBLOCK`.
- `G_LOADTILE` pulls a `w x h` tile out of a wider image, so it must skip
  `stride - w` bytes per row. A stride one byte short shifts **every row one texel
  left** -- a shear of one texel per row, i.e. ~45 degrees across a 32-row tile.

That is the whole story of the "tilted skybox": OoT's `Skybox_CalculateFace256`
loads each 256x256 face as 32 `gDPLoadTextureTile` sub-rectangles, every one of
them sheared by the same 45 degrees, so the Market panorama looked *rotated*
rather than *sheared* and sent three sessions chasing the transform.

**Cross-check against DaedalusX64**, which solves this on the same hardware:
`DLParser_SetTImg` (`Source/HLEGraphics/DLParser.cpp:822`) is literally
`g_TI.Width = command.img.width + 1;`, and its `SetTImg` bitfield declares
`u32 width:12` -- note **12** bits, not 10. This port had both wrong.

### The general lesson, which is the reusable part

**A geometric symptom does not imply a geometric cause.** The skybox looked rolled
about the view axis, so five sessions' worth of work went into the transform chain:
the skybox matrix, the eye position, the vertex table, the clip-space/render-space
aspect mismatch, the near plane, the clipper itself. All of it measured *clean*, and
all of it was correct -- a sheared texture inside correctly-placed quads is
indistinguishable from rotated geometry when the quads cover the whole screen.

The measurement that finally split it apart was cheap and should have come first:
**dump the post-transform NDC coordinates of the offending vertices and see whether
the grid is axis-aligned.** It was, exactly, which excludes every transform in one
step and leaves only texturing. `gPspSkyVtxOut` in `gfx_pc.c` is that probe; the
companion `gDebugSkyFaceMask` (`z_vr_box_draw.c`) draws one face's display list at
a time, which is what proved the tilted face was the one *fully in front of the
camera* -- killing the "geometry straddling the eye is mis-clipped" theory that the
negative-`w` readings had made so attractive.

## 9c. Audio: the RSP does the synthesis, and three separate silences hide behind that

This one cost three sessions, because every layer above it looked healthy the whole time. The
general trap and its three concrete instances:

**The trap: an N64 game's audio "engine" is not a synthesizer.** In the decomp,
`AudioSynth_Update` (`src/audio/internal/synthesis.c`) takes an `s16* aiStart`, walks every
active voice, and reads exactly like a softsynth. It is not one. Every `a*()` call inside it is
an `abi.h` macro that packs two words into an `Acmd`; the finished list goes to the **RSP** as an
`M_AUDTASK`, and the RSP is what decodes ADPCM, resamples, envelope-mixes and writes the PCM.
Port it as-is and the AI buffers stay all zeroes forever — no error, no crash, no warning.

*Do this instead*, the way every N64-decomp port does: `#undef` the ABI macros and redefine them
to call C functions that execute immediately, so the same unmodified decomp file becomes a real
synthesizer. Take the implementations from an existing port rather than writing them — this is
bit-exact fixed-point DSP emulation and "cleaning it up" changes how the game sounds.
For OoT/MM specifically use libultraship's `mixer.c` (Ship of Harkinian), not SM64's: SM64's
microcode is missing about ten opcodes OoT uses (`aEnvSetup1/2`, `aS8Dec`, `aAddMixer`,
`aDuplicate`, `aResampleZoh`, `aFilter`, `aInterl`, ...). Here: `psp/src/audio/psp_audio_mixer.c`.

**Instance 2: audio table `romAddr`s are segment-RELATIVE, and something adds the base later.**
`AudioLoad_InitTable` walks every table entry once at init and does
`entries[i].romAddr += <Audioseq/Audiobank/Audiotable segment rom start>`. If your port
substitutes its own asset addresses (blob ids, virtual offsets, anything), they must be stored
*minus* that segment base or every audio DMA lands somewhere else. The general lesson: **before
substituting an address the engine will consume, grep for who else rewrites that field.**
The failure is silent in the worst way — the bogus address fell past the end of the ROM file,
`sceIoRead` returned zero bytes, and the destination kept the zeroes the audio heap had already
put there. A soundfont of zeroes relocates every instrument to `NULL` and plays perfect silence.

**Instance 3: the sample bank is deliberately never loaded, only DMA'd through.** `Audiotable`'s
table entry uses `cachePolicy 4` (`CACHE_LOAD_EITHER_NOSYNC`), which makes
`AudioLoad_TrySyncLoadSampleBank` hand back the raw cart address without loading anything; each
note then DMAs its own few-hundred-byte window out of the middle of it (`AudioLoad_DmaSampleData`).
That is how a 4 MB sample bank fits in a 229 KB audio heap. Any asset-serving layer built for
whole-file loads (this port's blob registry matched on an exact start address, which is all a
scene or room ever needs) therefore misses **every single sample read**. If your port intercepts
asset loads, check which assets are read *partially* before assuming file granularity — and keep
those files open, since the reads happen several times per audio frame on the audio thread.

**Instance 4, and the one that actually gated everything: `AudioCmd` is an
endian-dependent union.** It aliases `u32 opArgs` with
`{u8 op; u8 arg0; u8 arg1; u8 arg2;}`, and `AUDIO_MK_CMD` packs the opcode
into bits 31..24 — offset 0 on big-endian, offset **3** on little-endian. So
`cmd->op` read the low byte, which is 0 for every command the game sends, and
`AudioThread_ProcessCmds` dispatched all of them as `AUDIOCMD_OP_NOOP`. The
whole subsystem looked healthy from outside — commands queued, the ring
drained, the reset gate opened, the output backend ran every frame — while
doing literally nothing. Same trap one level down in the data union, because
`AudioThread_QueueCmdS8`/`QueueCmdU16` store their payload in the **high**
bits of the word (`data << 0x18`), so `asSbyte`/`asUShort` need explicit
padding on little-endian.

*The general lesson, which is the reusable half:* **a union that aliases a
wide integer with narrower fields is a byte-order decision, and the compiler
will not warn you.** When porting a big-endian codebase, grep every `union`
for mixed-width members and check each one, before debugging anything
downstream of it. The failure mode is uniquely misleading — no crash, no
warning, and every observable *around* the bug looks correct. Ship of
Harkinian's `z64audio.h` has exactly one such `#ifdef IS_BIGENDIAN` block,
and it is this struct; that is a useful cross-check for the next port.

**Instance 5: a relative asset path only works on the main thread.** PSP threads
created with `sceKernelCreateThread` do **not** inherit a current working
directory, so `sceIoOpen("blobs/x.bin")` returns `SCE_KERNEL_ERROR_NOCWD`
(0x8002032C) from any thread the game spawns itself. Scene and room loads ran on
the main thread and worked for months; the audio thread's loads all failed. And
because the engine's DMA path has no error channel, `sceIoRead` was simply never
reached and the destination kept the audio heap's zeroes — the load was then
marked `LOAD_STATUS_PERMANENTLY_LOADED`. Resolve asset paths against the game's
own directory (from `argv[0]`, captured on the main thread) rather than relying
on a cwd.

**Instance 6: a partially-linked object is not a payload.** The soundfont build
is compile → `ld -r` (partial link) → `sfpatch` → `objcopy -O binary`. A
partial link leaves relocations *unapplied*: every instrument/drum/sfx offset
and every `SampleBank_*_SAMPLE_*_Off` reference still reads as zero, and
`objcopy` happily writes out a correctly-sized file full of holes. The engine
loads it without complaint and relocates 92 instruments to `NULL`. The fix is a
real link (`ld -R samplebank.o -T soundfont.ld`) so the sample-bank symbols
resolve and the script places `.rodata` at 0 — which is exactly what the game's
own `AUDIO_BUILD_DEBUG` rule does, and that rule exists because it
byte-compares its output against the audiobank ripped from the ROM. *General
lesson:* when a port reuses a decomp's intermediate build artifacts, check
whether the real build does anything else to them before they reach the ROM;
and `objdump -h` showing `RELOC` on the section you are about to `objcopy` is
the tell.

Two more audio-specific traps already paid for in this port:

- **A hand-built stand-in for a generated table must reproduce its memory LAYOUT, not just its
  values.** `AudioTable` is a header immediately followed by its entries, because every access is
  `table->entries[i]` off one pointer. Declaring header and entries as two globals puts them in
  `.data` and `.bss`, hundreds of KB apart, and every lookup silently reads unrelated data.
- **A `size == 0` table entry is a pointer, not a blank.** `AudioLoad_GetRealTableIndex` redirects
  through `entries[romAddr]` as an *index* — but `medium`/`cachePolicy` are still read from the
  **original** id, so filler entries need real values there or you get `MEDIUM_RAM`(0) and a hang.

Full pipeline background, including the PSP `sceAudio` side and a diagnostic ladder:
`psp/docs/AUDIO_N64_VS_PSP.md`.

## 10. Development/debugging technique that paid off repeatedly

- **A three-generation ring of plain-global diagnostic counters (`prev2`/`prev`/`live`), read
  out via a read-only remote-memory-inspection channel (not file I/O — file I/O itself was a
  crash cause once on this project, and perturbs exactly the frame-timing-sensitive bugs you're
  trying to observe), beats interactive step-debugging for frame-periodic bugs.** A bug that
  alternates or cycles across frames is invisible if you only ever look at consecutive-frame pairs
  (a period-3 bug looks identical two frames out of three when sampled two-at-a-time) — bucket
  samples by an observable that distinguishes the frame "type" (e.g. a rejection count, a hash) and
  compare *across* buckets, not just adjacent frames, once a naive adjacent-frame comparison
  doesn't explain the symptom.
- **Give every diagnostic counter block a magic-number header field.** `.bss`-symbol link
  addresses shift on every rebuild that adds or removes a global; a stale hardcoded address in a
  debugging script reads *plausible-looking wrong values*, not obvious garbage, and has cost real
  time on this project by being briefly mistaken for the game actually being in a bad state. A
  magic word lets a reader script fail loudly ("wrong build is running") instead of silently
  reporting nonsense. Re-run the symbol-address lookup after every single rebuild, unconditionally.
- **When a diagnostic itself changes runtime behavior (a flag that disables a subsystem, forces a
  single resource pool instead of double-buffering, etc.), it must change that behavior
  CONSISTENTLY at every site that observes the changed state**, or the diagnostic produces a false
  positive that looks exactly like a real bug. (Concretely: pinning one code path to a single
  double-buffered resource pool while a *different* code path's own sanity check still assumed
  double-buffering produced a spurious assertion failure that was wrongly read as "the double
  buffering is load-bearing, don't remove it" for an entire session.)
- **A no-op stub's function signature mismatch is invisible at compile/link time and only shows
  up as behavior**, so a systematic audit (real prototype vs. current stub, for every stub) finds
  bugs that would otherwise require independently stumbling into each one at runtime. Do this
  audit once, early, rather than one bug at a time (see class 3).
- **When every individually-measured value checks out correct, the bug is very likely in ordering
  or timing, not in any single value — change what you're measuring, not how hard you're looking
  at the same measurement.** (See class 6 — the single most expensive lesson in this project,
  worth repeating as its own bullet here.)
- **A user's plain-language description of what's visually wrong ("that's not a separate part,
  that's his chest, just attached to his chin") can encode more diagnostic information than a
  session's worth of numeric instrumentation**, particularly for *which piece is attached to
  which other piece*, something that's easy to fail to ask for explicitly in an automated
  measurement. Ask for a description in the user's own words before assuming the next
  instrumentation pass is the fastest path to a diagnosis.
- **A cheap, structurally different rendering path (e.g. rendering a different in-game character
  model/skeleton through the same renderer) is a fast, decisive way to separate "bug in this
  specific asset's data" from "bug in the renderer itself."** If a second, unrelated asset through
  the same code breaks identically, every asset-specific theory is eliminated in one test.
