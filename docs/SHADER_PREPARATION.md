# Shader preparation: how the shader stutter was removed

This fork builds each game's shaders before you play, on a *Preparing shaders* page at the first start. This page
explains why that was needed, what was measured and tried along the way, what did not work, and what shipped in
0.3.6.30. It is written for players who want to know what the page does and for anyone continuing the work.

## The problem

The PS4 games come with shaders for AMD's GCN graphics chips. The port translates each one to SPIR-V for Vulkan the
first time a draw needs it, and your graphics driver then builds a *pipeline* (the shaders plus the draw's fixed
state) before the draw can run. A PS4 shader can need several translations (*variants*): the port bakes details of the
draw into the translated shader, such as vertex strides, buffer formats and texture types.

When a draw needs a variant or pipeline nobody built yet, the frame waits for it. The port already translated
shaders and built pipelines on worker threads ahead of the draw where it could, and kept them in a shader cache, so a
second visit to an area was smooth. The first visit was not.

### What was measured

A hitch catcher (`BB_HITCH_MS`) logs every frame over a threshold with what the threads were doing. Scripted routes
replay recorded controller input from fixed saves, so the same walk can be repeated with a cold or warm cache.

- **Fast PC (RTX 5080, 16-thread CPU), cold shader cache, warm graphics driver cache:** a few hitches over 25 ms
  per route, each with pipeline builds in it. A cold run translated ~530-610 shaders (~0.7 ms each) and built
  ~60 graphics pipelines on the GPU thread (~1.5 ms each, mostly found in the driver's own cache).
- **Simulated older processor** (8 threads at 60% clock): 7 hitches over 25 ms on the same route, the worst 125 ms,
  all pipeline builds the frame waited for.
- **Cold driver cache too** (first install, or after a driver update): 22 hitches on the route, the worst **874 ms**:
  compute pipelines built one after another on the GPU thread, each a true compile of 25-100+ ms.

So the stutter was real, worst on slower processors and after driver updates, and almost all of it was building
things the frame had to wait for.

## What was tried

### 1. Building compute pipelines for the async compute queues ahead (kept, off by default)
About 90% of compute dispatches come from the games' async compute queues, which the draw-preparation workers did not
look at, so their pipelines were always built on the GPU thread. Scanning those queues too and building new compute
pipelines on worker threads halved the compute stalls on a cold run (1.5 s to 0.7 s). Cold compiles still often take
longer than the head start the workers get. `BB_PREP_ASYNC_COMPUTE=1`.

### 2. Graphics pipeline libraries (not used)
`VK_EXT_graphics_pipeline_library` lets the driver build a pipeline's parts separately and link them quickly, the way
DXVK avoids stutter. Measured on the games' real caches: almost every pipeline is a new combination of shaders (3,526
pipelines, 3,397 distinct shader sets), so parts are reused only about twice. Parts built on demand did not help a
cold first run (the parts are the slow bit), and the split layout cost ~15% frame rate per draw. Combined with item 1
it gave the best cold result of this approach (15 route hitches instead of 20), but parts would still have to exist
before the draw. Kept in the code as an experiment (`BB_GPL=1`).

### 3. Guessing variants from the game files (does not work)
Every shader the games use sits in their package files. If the draw-time details of each variant could be guessed,
everything could be built up front. A test compared every stored variant with the most likely guess for each field:
**0% of full guesses were right**; even with half the inputs known, fragment shaders matched 25%, vertex shaders
0.5%. The details depend on the game's own material and mesh data, not on the shader.

### 4. Rebuilding a player's own cache from the game files (works; the basis of what shipped)
The shader cache now records each variant's translation *inputs*. From those alone, plus the player's own game files,
a variant can be translated again: in a test, all 564 variants of a cold route came out **identical** to the stored
ones, with no game code shipped and no translated shaders stored. Finding each shader in the game files took a
48-second scan, so a *shader location index* (which file and offset holds each shader, per game version) replaced it.

### 5. Predicting variants from the game's material data (research, not shipped)
The packages describe materials, render passes, blend states and vertex layouts. A prototype that read those tables
predicted **93%** of one Uncharted 3 route's real pipelines without playing it, and a cold first start with only
predicted pipelines stalled once on the route instead of 19 times. Tables learned in Uncharted 3 predicted 96% of the
pixel shaders of an Uncharted 2 level. The misses were the engines' own shaders (post effects, compute), and vertex
shader predictions were far too many to build (127,000 candidates for Uncharted 2). It needed deep knowledge of the
package format and was not shipped.

### 6. Recording every checkpoint (what fills the lists)
To record the variants the games really use without someone playing for days, a script walks each game's
checkpoints: it edits a save so it continues at the next checkpoint, starts the game, turns the camera round, and
moves on. That ran for all three games on the EU and US versions, plus second passes over areas with many new shaders,
caches from players, and clean-cache play sessions by hand.

## What shipped in 0.3.6.30

- **Variant lists per game** (`shaders/<game>_variants.txt`, `<game>_keys.txt`, `<game>_compute_keys.txt`): each
  line describes one variant or pipeline by its translation inputs and fixed state. They hold **no game code and no
  translated shaders**; your own game files supply the shader code.
- **Shader location indexes** per version (`shaders/shader_locations_<title id>.txt`): where each shader is in the
  game files, so nothing needs to be scanned.
- **The preparation page:** at the first start of each game (and after the cache is cleared or its format changes),
  the port translates every listed variant from your game files and builds every listed pipeline, on all but two of
  your processor's threads. The game waits behind the page, so it starts afterwards as if no time had passed.
- **The result:** in hand-played sessions on a clean cache, about 12 minutes of Uncharted 2 had one 35 ms compile
  hitch, Uncharted 3's chateau had no pipeline built on the GPU thread at all, and 15 minutes of Uncharted 1 had two
  small ones. The lists are about 3,700 variants for Uncharted 2, 12,600 for Uncharted 3 and 1,700 for Uncharted 1.
- **Timing** (empty caches, including your driver's): Uncharted 3 about 36 s on a 16-thread processor, about 70 s
  on a simulated older 4-core one; Uncharted 2 about 24 s and Uncharted 1 about 11 s on the slower setup. Most of
  that is the driver building pipelines.
- **A faster pipeline-cache lock** came with it: with many more variants in the cache, its old lock made the GPU thread
  and the preparation workers wait for each other during play (about 4% average and 10% in the worst moments in
  Uncharted 3's crowd). A Windows slim reader/writer lock removed that cost.

## Limits and what is left

- Tested on NVIDIA graphics. The lists describe inputs, not NVIDIA output, so they should work elsewhere, but AMD and
  Intel are untested.
- Anything the lists miss is still built the old way when it first appears (worker threads, else the GPU thread).
- One listed variant (`8e11813d`) cannot be built; the game builds its own when needed.
- **Never waiting at all** would need either drawing with a stand-in while a pipeline builds in the background (brief
  pop-in instead of a hitch), or translating shaders without baking draw details into them (one translation per PS4
  shader, everything built up front). Both are larger changes and not started.

## Switches

`BB_PREP_SCREEN=0/1` (page off / forced), `BB_PREP_FREEZE=0` (the game runs behind the page), the launcher's
*Shader preparation* section (before play, in the background, off), `BB_PREP_ASYNC_COMPUTE=1`, `BB_GPL=1`
(experiment), `BB_SRW_LOCKS=0` (old pipeline-cache lock). Developer diagnostics: `BB_HITCH_MS=<ms>`,
`BB_SPEC_DUMP=<file>`, `BB_KEY_DUMP=<file>`, `BB_VARIANT_REPLAY=1`.
