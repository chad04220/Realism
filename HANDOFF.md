# Realism — engineering handoff

**Audience:** the next engineer or AI agent taking this repository from a strong
simulation vertical slice to a game that can be sold.

**Repository:** `Realism` — a voxel physics sandbox in C++20/OpenGL 4.3, about
17k lines, no third-party runtime dependencies.

**Status of this document:** written after the hardening pass of 2026-10-22.
Everything marked *verified* was run in the Linux headless build in this
workspace. Everything else is an observation from reading the code and must be
re-verified before it is quoted externally.

Read this whole file before changing simulation behaviour. The invariants in
§4 are the load-bearing walls of the project.

---

## 0. The target, stated honestly

The owner's goal is a "near-AAA, market-ready, BBB-quality" game in which every
voxel behaves correctly. Two parts of that need to be separated:

1. **"Every voxel behaves as it should"** is a technical contract. It means a
   voxel is a control volume with mass, temperature, phase, damage, electrical
   state and structural attachment, and the simulation conserves mass, energy
   and momentum to a stated tolerance. This is achievable incrementally and is
   the right north star for the simulation.
2. **"AAA"** means a production scale (art, animation, audio, cinematics,
   localization, QA, certification) that this codebase cannot reach by adding
   features alone. The realistic target is a **premium indie sandbox** — call it
   "BBB": a coherent, polished, 8–20 hour game that a stranger can buy, install,
   understand without a wiki, play without a game-breaking bug, and recommend.
   That is a demanding but finite goal; this document plans for it.

Do not let "AAA" turn into an excuse to add breadth. The fastest route to BBB is
depth on a small number of verbs (take apart, build, heat, burn, electrify,
flood, drive) plus the product shell that makes them legible: save/load,
tutorial, audio, settings, controller, accessibility, stability and packaging.

### Definition of done for BBB

- A new player can install, launch, finish the first 15 minutes without external
  help, and save/reload without losing a build.
- The eight shipped machines, the workshop, fire, water, electricity and
  destruction each have a scripted acceptance test that runs headless.
- The game runs at 60 fps at 1080p on the reference laptop (i7-10750H / 16 GB /
  RTX 2060-class) at the default 2.5 cm voxel size, with no frame over 33 ms in
  a 10-minute soak.
- No known crash, save corruption, infinite loop or memory growth in 8 hours of
  soak; the blueprint and settings parsers survive fuzzing.
- Audio, keyboard remapping, controller, subtitles/captions, UI scaling and
  colour-blind-safe state colours exist; at least English text is externalized.
- Installer/portable build, version stamp, credits, third-party notices, crash
  log and a privacy-respecting opt-in report path exist.
- Store page assets, a 2-minute trailer capture mode and a press/streamer build
  exist.

---

## 1. Snapshot: what actually works

Verified by `Realism --selftest` in this workspace (19 checks, all passing at
both 0.025 m and 0.02 m voxels):

| System | State | Evidence / notes |
|---|---|---|
| Sparse voxel world | Working | 17.0 M solid voxels in 345 MB at 0.025 m; 33.2 M / 698 MB at 0.02 m |
| Sparse brick storage | Working | 16³ `Brick`, allocated on first matter; open air costs nothing |
| Rigid bodies + inertia | Working | Mass/COM/inertia recomputed from voxels; sphere-sample contacts |
| Sequential-impulse physics | Working | World and body-body contacts, friction, restitution, sleeping |
| Destruction | Working | Joint-strength detachment, cratering, Voronoi fracture, debris |
| Support / falling structures | Working | Bit-packed flood, now 16 MB scratch instead of 512 MB |
| Heat conduction / convection / radiation | Working | Faces, air coupling, 320-ray radiant budget |
| Fire / combustion | Working | Ignition, fuel, burnout, residue, water extinguishing |
| Phase change | Partial | Melt/freeze/boil exists; **no latent heat** and no volume change |
| Air (stable fluids) | Working | 20 cm grid, multigrid pressure, buoyancy, smoke/steam, wind, fans, blasts |
| Water | Working (world only) | Fill-level cells, pressure head, droplets, Torricelli jets, currents |
| Powders / molten flow | Working | Whole-voxel slumping on the world grid |
| Circuits | Working within one volume | Nets, loads, shorts, battery drain, solar output |
| Machines | Working | Motor→shaft→wheel/prop, rockets, charges, valves |
| Workshop | Working | Place/paint/erase/wire, parts, materials, undo/redo, blueprints |
| Content | Vertical slice | One yard scene, 8 assembly blueprints, ~50 materials, 9 tools |
| UI | Working | Custom SDF UI, HUD, menus, target cards, title screen |
| Renderer | Working (Windows/Linux GL) | Ray-marched smooth/blocky surfaces, shadows, bloom, volumetrics |
| Persistence | **Missing** | Only `settings.cfg` and `.rbp` blueprints; no world/body/game save |
| Audio | **Windows only** | `waveOut`; `audio.cpp` returns false elsewhere |
| Headless tests | Working | `--selftest`, no window/GL/X11; CTest and CI wired |
| Player→body collision | **Missing** | `Game::playerCollides` only samples `sim.world` |
| Cross-body circuits | **Missing** | Each body is solved alone, only if it owns a battery/solar |
| Tutorial / objectives | **Missing** | No onboarding, progression or goals |
| Controller / remap / localization | **Missing** | Hard-coded keys and English strings |

Baseline measured on the 2-vCPU cloud VM (not the reference laptop):

| Build | World | Voxels | World RAM | Peak RSS | Build time | Self-test |
|---|---|---|---|---|---|---|
| headless, 0.025 m | 16×8×16 m | 17.0 M | 345 MB | 442 MB | 2.6 s | 34 s |
| headless, 0.02 m | 16×8×16 m | 33.2 M | 698 MB | 814 MB | 3.9 s | 34 s |

GPU timings are not measured here (no GPU). The renderer is unverified in this
workspace; do not quote frame rates until they are measured on the reference
machine.

---

## 2. Build, run, verify

There are no package dependencies. CMake 3.20+, a C++20 compiler with AVX2, and
on Linux the X11 development headers for the windowed build.

```bash
# Linux / CI, no window, no OpenGL:
cmake -S . -B build-headless -DCMAKE_BUILD_TYPE=Release -DREALISM_HEADLESS=ON
cmake --build build-headless -j
./build-headless/Realism --selftest          # exit 0 pass, 2 failure
ctest --test-dir build-headless --output-on-failure

# Linux windowed (needs libX11 + GL headers):
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/Realism

# Windows (MSVC 2022):
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\Realism.exe

# Windows cross-compile (MinGW-w64):
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake-mingw.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

Useful flags: `--selftest`, `--voxel 0.02|0.025`, `--script file`,
`--shot out.ppm --frames N`, `--size W H`, `--help`, `--version`.

The self-test deliberately runs the real scene, not a toy world. Keep it that
way. It is the cheapest regression net the project has.

`.github/workflows/build.yml` builds the headless target on Linux, runs the
self-test, and compiles the Windows build. Treat a red CI as a release blocker.

---

## 3. Architecture map

### 3.1 Ownership of files

| Area | Files | You touch this when… |
|---|---|---|
| Math / RNG | `src/core/math.h`, `src/core/jobs.*` | adding vector/quaternion helpers, changing job scheduling |
| Voxel storage | `src/sim/volume.h`, `src/sim/body.h` | changing the cell layout, brick storage, body fields |
| Materials | `src/sim/materials.*` | adding materials, phases, physical constants, derived per-voxel mass/heat capacity |
| Components | `src/sim/comps.*` | part types, joints, device power, per-component state |
| Orchestrator | `src/sim/sim.*` | step order, world edits, body lifecycle, raycasts, effects, support |
| Physics | `src/sim/physics.cpp` | contacts, impulses, friction, sleep, contact heat |
| Heat | `src/sim/thermal.cpp` | conduction, combustion, phase changes, radiation |
| Air | `src/sim/air.*` | stable fluids, buoyancy, smoke, blasts, air/body coupling |
| Water/fluids | `src/sim/fluids.cpp` | water fill, pressure, droplets, powders, molten flow |
| Circuits | `src/sim/electric.cpp` | nets, loads, shorts, motors, rockets, fuses |
| Damage | `src/sim/damage.cpp` | impacts, fracture, explosions, torch heat |
| Scene/content | `src/game/scene.cpp`, `assemblies.*`, `parts.*` | the yard, machines, part catalogue |
| Building | `src/game/build.*`, `workshop.*` | rasterizer, wire routing, workshop UI/editing |
| Game shell | `src/game/game.*`, `hud.cpp` | player, tools, menus, HUD, title, script commands |
| Settings | `src/game/settings.*` | persistence of options and validation |
| Audio | `src/game/audio.*` | synthesis, mixer, device backend |
| Render | `src/gfx/renderer.*`, `shaders.h`, `gl.*`, `shader.*` | GPU path, visual features |
| Platform | `src/platform/{win32,x11,headless}.cpp` | windowing, input, clock, clipboard, paths |
| UI | `src/ui/ui.*`, `font_data.h` | immediate-mode UI primitives and theme |

### 3.2 Step order (`Sim::step`)

```
Sim::step(dt)
  time += dt
  stepDevices(dt)          # lights, heaters, motors, rockets, fuses, shorts
  stepPhysics(dt)          # substeps, contacts, impacts, heat from friction
  stepDebris(dt)           # ballistic debris, deposition into the world
  stepFx(dtReal)           # fire/smoke/spark/steam particles
  thermal: every 1/20 s, dt * thermalWarp, early-out on empty hot sets
  fluids:  every 1/45 s
  electric: every 0.1 s
  air:      every 1/20 s (if initialised)
  processSupport()         # only when supportSeeds is non-empty
  rebuild changed bodies, split disconnected bodies, settle small rubble
```

`Game::frame` calls `sim.step` only when the game is not paused or in a menu,
then updates hover again because bodies may have been created/removed.

### 3.3 Data model and units

- **Metres** everywhere in gameplay and physics. `Sim::vs` is metres per voxel.
  Voxel coordinates are integer cell indices (`ivec3`), cell centres at
  `(i + 0.5) * vs`.
- **`Volume`** is a sparse grid of 16³ `Brick`s. `Brick` is a flat 4096-cell
  array of material/flags/hp/aux/component/temperature/fuel. `Volume::set` and
  `put` maintain brick counts, dirty lists, shape-change lists and `version`.
- **`Body`** owns a `Volume`, a COM in voxel coordinates, a world transform
  (`pos`, `rot`), velocity, inverse mass/inertia, collision samples, rotating
  parts and a hot-brick set. `frozen` bodies (workshop jig) have `invMass = 0`.
- **`Component`** is a named part instance in a global `std::deque`; id 0 means
  none. Components may be shared by several voxels and can own electrical state,
  torque, fuel, charge and a fuse. `Component` references must survive growth:
  that is why `comps()` is a deque, not a vector.
- **`Sim`** owns the world, bodies, debris, effects, events, air, radiators,
  fluid/support/special sets, statistics and the thermal/fluid/circuit clocks.
- **`Game`** owns the player, camera, settings, renderer, UI, audio, workshop,
  current scene and all input/tool logic. It is the only place that knows about
  game modes.
- **`SimSettings`**: voxel size, world extent, thermal warp, thread count.
  Content is authored in metres and rasterized at the current voxel size.

### 3.4 Threading

`Jobs::get().parallelFor` is used for contact generation, some thermal brick
batches, explosion scans, air rows and radiant raycasts. Rules:

- Never mutate `Volume`, `Body`, `comps()` or `Sim` containers inside a parallel
  body unless the code demonstrably writes disjoint indices and no allocation
  occurs. Today's code collects results locally and merges them after the loop.
- `rng()` is `thread_local` and seeded from the thread-local address. Do not
  assume deterministic results across runs or thread counts. See §7.3.
- The audio mixer is a separate thread; it only reads its own bank and atomics.

---

## 4. Invariants — do not break these

1. **One voxel, one cell.** Material, flags, hp, aux, component, temperature
   and fuel live together in `Brick`; no side tables keyed by position for cell
   state.
2. **World edits go through `Sim::setWorld`, `putWorld` or `removeWorld`.** They
   update specials, fluid activity, support seeds, hot sets and the air grid.
   Direct `world.set` is only for internal bulk operations that explicitly
   repair that bookkeeping (water movement, detach, settle, scene build before
   air init).
3. **6-connectivity matters.** Wires, support, heat and structure all use
   `DIRS6`. A diagonal-only connection is not a connection.
4. **Water fill encoding:** `fuel == 0` means a full cell; `> 0` is the fill in
   `(0, 1]`; a value above 1 is overfill and must be redistributed by the water
   step. Never store a negative fill.
5. **`Cell::hp` is 255 for intact; 0 is never valid** (clamp to 1 after damage).
6. **`aux` is overloaded by material**: molten remembers its original material;
   a valve gate uses `aux == 1`. Do not add a third meaning without renaming it
   or adding a field.
7. **Component id 0 is "none".** Never create a part with id 0; never assume
   `comp()` references stay valid across `comps()` growth unless using the deque.
8. **Body ids are not indices.** `bodies` can be reordered/erased; use
   `bodyById`/`bodyIndex` around any operation that can add or remove bodies.
9. **World and body volumes are different coordinate systems.** `Body::toWorld`
   / `toLocalVoxel` convert; COM is in voxel coordinates, not metres.
10. **`Sim::raycast` returns the nearest world or body hit.** `h.body == -1` is
    the world; any other value is an index into `bodies` at the time of the
    call, not a stable id.
11. **The air grid is coarser than the voxel grid** (`cellVox` voxels per cell).
    Never assume a 1:1 mapping; use `AirSim::openCellAt`, `velocity`,
    `temperature`, `smoke`.
12. **Thermal time is warped** by `cfg.thermalWarp`. Any new heat source must
    multiply power by the same warp the existing code uses, or it will behave
    differently when the setting changes.
13. **Joules are joules.** Toughness, explosion energy, joint energy, battery
    Wh and combustion power all use SI. `Material::voxMass`, `heatCap`, `tough`
    and `power` are derived per voxel at `initMaterials(vs)`.
14. **`Brick` is a POD bulk array.** Keep it trivially copyable and dense; no
    per-cell `std::string`, no virtuals, no pointers.
15. **Bodies with zero voxels are removed** (except frozen jigs). Do not leave
    empty bodies in the vector.
16. **`Sim::init` is a full reset.** New per-run state must be cleared there;
    "New world" calls it.
17. **The headless build must never require a window, GL context, audio device
    or X11.** Any new platform call needs a `headless.cpp` path.
18. **The self-test must stay a scene-level test.** Do not replace it with a toy
    fixture to make a change pass.
19. **No new runtime dependency without explicit product approval.** The
    dependency-free build is a feature (single static `.exe`, easy CI).
20. **Do not silently change the supported voxel range.** Content is authored
    at 2–2.5 cm; the UI, settings clamp and README must agree.

---

## 5. Performance baseline and budgets

Measured in this workspace (2 vCPU, no GPU, headless self-test):

| Metric | 0.025 m | 0.02 m |
|---|---|---|
| World build (scene + 90 settle steps) | 2.6 s | 3.9 s |
| Self-test wall | 34 s | 34 s |
| Peak RSS | 442 MB | 814 MB |
| World data | 345 MB | 698 MB |
| Solid voxels | 17.0 M | 33.2 M |
| Support flood scratch | 16 MB (bit per cell) | 33 MB |

Target budgets for the reference laptop at 1080p, default world:

- Frame: 16.6 ms; simulation ≤ 8 ms; render ≤ 12 ms (overlap where possible).
- Air step ≤ 3 ms; thermal ≤ 2 ms; physics ≤ 3 ms; fluids ≤ 1 ms; circuits
  ≤ 0.5 ms. These are aspirational until measured; add a benchmark mode first.
- Memory: ≤ 1.5 GB RSS and ≤ 1.5 GB VRAM at 0.025 m with a busy scene.
- No allocation in the per-frame hot path except bounded FX/debris/event deques.
- Startup to interactive title ≤ 5 s on the reference machine.

The project currently allocates a `sizeof(Brick)` (~160 KB) brick whenever a
single voxel enters a 16³ region. That is acceptable for content but hostile to
fine voxel sizes and to many small bodies; a future allocator (compressed
bricks, per-brick material palette) is a lever for 4× memory reduction.

---

## 6. Recent fixes — do not regress

These were made in the hardening pass and are covered by the self-test unless
noted:

1. **Headless target.** `REALISM_HEADLESS`/`src/platform/headless.cpp` lets the
   simulator build and test without X11/GL. `--selftest` no longer opens a
   window or creates a GL context.
2. **CLI validation.** `--help`, `--version`, unknown-option failure, numeric
   ranges, `--shot` frames default, `--headless` only with `--selftest`.
3. **Settings parser.** Inline comments, boolean words, full-token numeric
   parsing, invalid-line fallback, `sanitize()` clamps, world-size allocation
   guard. Covered by three self-test checks.
4. **Force integration.** External forces (thrust, drag, fans) used to be
   applied once per physics substep, multiplying them by 2–4. They are now
   applied once per frame. Covered by a unit-force test.
5. **Detonation flood.** The charge kernel used a linear `std::find` inside a
   flood, O(n²) at the 200k-cell cap. It is now an `unordered_set`.
6. **Support flood memory.** Replaced a 4-byte-per-world-cell stamp
   (512 MB at default) with a visited bitset (16 MB) and explicit bit clearing.
   This is the biggest memory win in the pass.
7. **`Sim::init` reset.** Time, statistics, events, radiators, flow fields,
   air state and clocks are cleared on "new world". Covered by a test.
8. **World-charge arming.** A charge fixed in the world is armed once for the
   remote detonator and no longer receives a fuse that cannot fire.
9. **`resetWorld` cleanup.** Hover/target caches, hit state, menus, pause and
   toast state are reset.
10. **Blueprint loader.** Size/count/string bounds, material/component
    validation, path-traversal rejection, resample cap. Joint names now live in
    a `std::deque`, fixing dangling `Joint::name` pointers across loads.
11. **Content at coarse voxels.** Template rasterization pads and trims; battery
    terminals sit at opposite ends; motor tabs touch the windings; bulb leads are
    at least one voxel long. This fixed self-test failures at 5 cm and made the
    supported range explicit (0.02–0.025 m).
12. **HUD/CLI range.** `--voxel`, `settings.cfg` and `sanitize()` all agree on
    the supported range; `--voxel 0.03` fails with a clear message.

The self-test's newest checks: settings round-trip/parse/clamp, unit force,
`Sim::init` reset.

---

## 7. Known defects and gaps

### 7.1 Correctness / simulation

| # | Issue | Where | Impact | Suggested fix |
|---|---|---|---|---|
| C1 | No latent heat; melt/boil/freeze are instantaneous threshold events | `thermal.cpp` | Ice/water and metals change phase with no energy plateau; energy is not conserved at transitions | Add `latentFusion`/`latentVapor` to `Material`, consume/release heat in `T_MELT`/`T_BOIL`/`T_FREEZE`, keep the cell in a "changing" state until the budget is paid |
| C2 | No volume/mass change on phase change | `thermal.cpp` | Molten metal occupies the same voxel as solid; water vapour disappears rather than displacing air | Decide a conservation policy (mass to air moisture/smoke or new gas voxels) and test it |
| C3 | Buoyancy uses solid voxel volume, not displaced volume | `physics.cpp` | A sealed hollow boat sinks; a solid steel block floats too strongly/weakly depending on shape | Compute submerged volume from the body's occupancy against water fill, or integrate pressure over samples |
| C4 | Water exists only in the world grid | `fluids.cpp` | Water inside a body never flows, pours or finds a level; a bucket of water is static | Either forbid water in bodies at creation or promote it to the world when a body opens; add a test |
| C5 | Bodies do not displace water | `physics.cpp`, `fluids.cpp` | A body entering water does not raise the level or push a current | Add a body-mask to the water step and transfer volume at the free surface |
| C6 | Circuits are per-volume and need a local source | `electric.cpp` | A bulb in a separate body from its battery never lights; split lamp/solar assemblies demonstrate this | Build a global net across touching bodies (or across components in contact), or keep assemblies rigid and forbid electrical splits |
| C7 | No structural stress, only connectivity + joint strength | `sim.cpp`, `damage.cpp` | A 10 m cantilever is as stable as a 10 cm one; thermal expansion does nothing | Add a coarse stress pass on the support component (load per joint, buckling), with a test beam |
| C8 | Radiant rays can create energy | `thermal.cpp` | Emission is proportional to surface temperature but hit energy is not normalized by the sampled solid angle/visibility; enclosed spaces can gain heat | Importance-sample emitters with a power-conserving estimator; add a two-plate equilibrium test |
| C9 | Combustion ignores oxygen consumption and products | `thermal.cpp` | Fire burns the same in a sealed box with one open face; no CO/soot mass; no char layer | Add `O2`/`CO2`/`CO` fields to `AirSim`, consume oxygen per mole, produce smoke/heat; test a sealed room |
| C10 | Water mass can leak | `fluids.cpp` | Debris cap converts droplets to dust, droplet life expires, `settleBody`/`spawnDebris` can drop water | Return a success flag from `spawnDebris`, recycle water into the world, and assert conservation in a soak |
| C11 | Support flood re-walks components | `sim.cpp` | Each seed re-floods its whole connected component; a large structure after one cut can take a long time; >4 M cells are assumed supported | Add a per-component "supported" cache invalidated on cuts, or a coarse chunk graph |
| C12 | No continuous collision for bodies | `physics.cpp` | Fast small bodies can tunnel through thin walls; only debris ray-marches | Add swept sphere/substep CCD for bodies above a speed threshold |
| C13 | Body-body contacts are sample spheres | `physics.cpp` | Thin plates and sharp shapes collide as blobs | Add per-sample shape/radius or a small box/plane proxy; keep the sample budget |
| C14 | Friction is isotropic and contact heat is a simple split | `physics.cpp` | Wood slides equally in all directions; no stick-slip | Add a friction direction frame and a proper work integral; optional |
| C15 | No thermal expansion or thermal stress | everywhere | Heated beams do not buckle, glass does not crack from gradients | Coupled to C7; start with a scalar expansion and a stress threshold |
| C16 | Air can be heated past the clamp and solids past 3600 °C | `air.cpp`, `thermal.cpp` | Values clamp rather than fail; a runaway can hide a bug | Add invariants/assertions in debug builds and a diagnostics overlay |
| C17 | `T_BOIL` removes the cell immediately | `thermal.cpp` | Boiling is not a phase change with a budget; steam mass is only visual | Coupled to C1/C2 |
| C18 | Water pressure is a column scan, not a pressure solve | `fluids.cpp` | Horizontal pressure waves and pipe networks are approximate; disconnected pockets can equalize incorrectly | Replace with a shallow-water/pressure projection on the water cells, or document the approximation |
| C19 | Gravity is a constant vector | `sim.h` | Fine for one yard; not a planet/space game | If the game stays local, keep it and say so |

### 7.2 Presentation / product

| # | Gap | Impact |
|---|---|---|
| P1 | **No save/load of the world, bodies, components or player** | The game cannot be a game; every session resets. This is the top blocker. |
| P2 | No tutorial, goals or progression | A new player bounces off the sandbox; the title screen explains nothing |
| P3 | Audio is silent outside Windows | Linux/macOS builds have no sound; the mixer/backend is not abstracted |
| P4 | Player walks through bodies | Grabbing/standing on machines is impossible; breaks the physical fantasy |
| P5 | No controller, remapping, localization or accessibility | Excludes a large share of players and store requirements |
| P6 | No installer, versioning, crash handler or telemetry | Cannot support customers or diagnose field failures |
| P7 | Only one yard; 8 blueprints; no campaign/scenarios | Content depth is thin for a commercial sandbox |
| P8 | Renderer is one GL path; no quality presets or upscaling | Cannot scale from integrated GPUs to high-end; no photo mode |
| P9 | UI text is hard-coded; no IME; no text scaling beyond UI scale | Localization and accessibility are blocked |
| P10 | Script command language is undocumented in-game | QA and content automation depend on a developer-only tool |
| P11 | No in-game undo/redo outside the workshop | Destructive play is unforgiving; no "oops" recovery |
| P12 | No blueprint browser metadata (thumbnail, author, tags) | Sharing and discovery are impossible |

### 7.3 Engineering / process

- **Determinism:** `rng()` is seeded from a thread-local address, and parallel
  thermal/explosion event merges are order-dependent. Replays, reliable
  regression tests and netcode need a deterministic seed and a stable merge
  order. Add `SimSettings::seed`, a per-thread seed derived from it, and sort
  merged events by cell key.
- **No unit tests:** the self-test is a 34 s integration test. Add a small
  zero-dependency harness (`tests/` with `assert`-style checks) for pure
  functions (settings, wire routing, `packKey`, materials, water fill math).
- **No sanitizer CI:** add ASan/UBSan and TSan jobs for the headless target;
  run the self-test under ASan (allow a longer timeout).
- **No fuzzing:** the `.rbp` parser and settings parser are now bounded but
  un-fuzzed. Add libFuzzer/AFL targets or a deterministic corpus test.
- **No performance regression gate:** add a benchmark mode (`--benchmark N`
  frames, fixed scene) with a JSON/CSV line and a CI budget.
- **No GPU tests:** the renderer is untested here. Add an Xvfb + Mesa software
  smoke test that captures one frame and checks non-black pixels.
- **No crash dumps:** Windows minidump handler, Linux signal handler writing a
  log, and a "copy diagnostics" button.
- **No packaging:** no installer/portable zip, no code signing, no store
  metadata.
- **Content scale:** templates are metres-authored but rasterized by
  center-sampling. Features thinner than a voxel can vanish at coarse sizes;
  the supported range is clamped to 0.02–0.025. The long-term fix is a
  minimum-feature rule plus a content linter that reports clipped features at
  the supported sizes.
- **Memory:** one `Brick` per touched 16³ region. The 0.02 m world is 698 MB.
  Fine for desktop, hostile to 8 GB machines and future 1 cm worlds.
- **Bugs found but not fixed:** `Sim::settleBody` silently drops voxels if no
  free cell is found in 3 cells above; `spawnDebris` silently drops water/ash at
  the 30k cap; `T_FREEZE` is ignored for body-owned molten cells; blueprint
  `RBP1` does not persist battery charge.

---

## 8. Roadmap to BBB

Sizes are rough: **S** ≤ 1 day, **M** 2–4 days, **L** 1–2 weeks, **XL** 2+ weeks
for one experienced engineer. An AI agent should treat these as sequencing
hints, not commitments.

### M0 — Stabilize (mostly done)

Exit criteria: headless self-test + CI green; settings/world validation; no
known crash on the shipped scene; documented supported range.

### M1 — Make it a game you can leave and come back to (blocker)

1. **Save/load v1** (L): versioned binary or JSON schema for world bricks,
   bodies, components, temperatures, flags, player, time, camera/tool, workshop
   jig and blueprint references. Atomic write (temp + rename), checksum,
   backward-compatible version field, corruption error UI.
2. **Autosave and crash recovery** (M): save on quit and every N minutes, keep
   a rotating slot; load the last good save if the newest is corrupt.
3. **Pause/menu correctness** (S): save/load pause the sim; settings apply live
   where possible; `settings.cfg` world size requires restart (document).
4. **Player collision with bodies** (M): capsule vs body samples; standing on a
   moving body; step-up; no tunnelling at 60 m/s.
5. **Cross-body electrical nets** (L): solve nets across touching bodies; at
   minimum, keep a lamp's bulb and battery in one body. Test the detached solar
   lamp and a bulb dropped on a wire.
6. **Onboarding** (M): a short interactive sequence that teaches look, grab,
   hammer, wrench, torch, switch, workshop; contextual prompts; skip.
7. **Deterministic seed + replay recording** (M): seed `SimSettings`; record
   input/commands; replay for QA and bug reports.

Exit: a new player can play, quit, relaunch, reload, and continue; two QA
replays produce identical world hashes.

### M2 — Make every voxel behave (fidelity)

Order matters; each item needs a conservation test before the next.

1. **Latent heat and mass-conserving phase change** (L): `latentFusion`,
   `latentVapor`, changing-state cells; ice/water/steam and metal melt tests.
2. **Buoyancy from displaced volume** (M): hollow boat floats, sealed steel
   sinks, water level rises when a body enters.
3. **Fluid–body coupling** (L): body mask in water, volume transfer, pressure on
   samples, currents; test a crate in a tub.
4. **Structural stress and progressive collapse** (L): load path, joint stress,
   buckling, thermal expansion; test a cantilever and a heated beam.
5. **Combustion with oxygen/products** (L): O2/CO2/CO in the air, char layer,
   soot, sealed-room test.
6. **Electrical resistance/voltage** (L): resistivity, gauge, voltage drop,
   fuses, internal resistance, series/parallel; lamp brightness vs wire length.
7. **Radiative exchange conservation** (M): power-conserving estimator,
   two-plate equilibrium test, emissivity by material.
8. **Contact fidelity** (L): CCD, box/plane proxies, anisotropic friction,
   better sleep; no tunnelling at terminal velocity.
9. **Voxel-size independence** (M): author features in metres with a minimum
   feature linter; scale all radii; run the self-test at 0.02/0.025/0.05 and
   widen the supported band only when green.
10. **Water pressure** (L): decide shallow-water vs pressure projection; document
    the model and test a pipe network.

Exit: the "every voxel" contract has tests for mass, energy, momentum and
charge; known approximations are listed in-game or in the manual.

### M3 — Make it run everywhere

1. Performance: benchmark mode, per-system timings, budget CI; optimize air
   (biggest), thermal active set, body-body pairs, support cache.
2. Renderer: quality presets, MSAA/TAA, upscaling (FSR1/2 or simple spatial),
   SSAO, better shadows, photo mode.
3. Memory: compressed bricks/palette; streamed world regions; VRAM budget.
4. Platform: Linux audio (ALSA/miniaudio), Wayland or XWayland testing, macOS
   (Metal or MoltenVK) only if the product needs it.
5. Packaging: portable zip and installer, version stamp, code signing, crash
   handler, logs.

### M4 — Make it deep

- Campaign/scenarios with goals that teach the systems (repair the pump, heat
  the house, launch the sled, demolish the wall without flattening the shed).
- 30+ blueprints with thumbnails, tags, search; copy/paste/mirror/layers in the
  workshop; blueprint sharing via a file and (later) a service.
- More materials and parts: springs, hinges, ropes, belts, gears, valves,
  pipes, pumps, sensors, logic gates, fuses, relays.
- A second and third environment (workshop interior, quarry, water tower yard)
  with different hazards.

### M5 — Make it feel finished

- Audio: full synthesis/SFX pass, music/ambience, mixer buses, occlusion,
  Doppler for fast bodies, UI sounds, subtitles/captions.
- UI/UX: controller, remapping, accessibility (colour-blind palettes, text
  scale, motion reduction, hold/toggle), localization-ready strings.
- Animation: first-person tool animation polish, grab poses, machine motion.
- Onboarding/polish: tooltips, tool wheel, goals, photo mode, replay viewer.

### M6 — Make it shippable

- QA matrix (Windows 10/11, integrated/discrete GPU, 8/16 GB RAM, 1080p/1440p),
  8-hour soak, save-corruption tests, blueprint fuzz, settings fuzz.
- Store page, trailer capture, press build, EULA, credits, third-party notices,
  privacy policy, opt-in telemetry, crash reporting.
- Release checklist (§13) signed off.

---

## 9. Concrete backlog

Use IDs in commits and issues. Each task should add or extend a self-test check
unless it is pure presentation.

| ID | Pri | Size | Task | Files | Done when |
|---|---|---|---|---|---|
| S1 | P0 | L | Save/load world+bodies+components+player | new `sim/save.cpp`, `game/game.cpp`, `sim/sim.h` | Byte-identical world hash after round-trip; corrupt file rejected; self-test |
| S2 | P0 | M | Atomic autosave + recovery slot | `game/game.cpp` | Kill -9 during save leaves a loadable previous slot |
| S3 | P0 | M | Player capsule vs body collision | `game/game.cpp` | Player stands on a crate and cannot walk through a rover |
| S4 | P0 | L | Cross-body electrical nets | `sim/electric.cpp` | Detached bulb and battery still light when wired across a seam |
| S5 | P0 | M | Deterministic seed/replay | `sim/sim.*`, `core/math.h` | Two runs with same seed/inputs produce equal state hash |
| S6 | P1 | L | Latent heat + mass-conserving phases | `sim/materials.*`, `sim/thermal.cpp` | Ice melts at 0 °C with a plateau; mass/energy budget passes |
| S7 | P1 | M | Displaced-volume buoyancy | `sim/physics.cpp` | Hollow boat floats, solid block sinks, level rises |
| S8 | P1 | L | Water–body coupling | `sim/fluids.cpp`, `sim/physics.cpp` | Crate entering a tub raises the level and gets pushed |
| S9 | P1 | L | Structural stress/collapse | new `sim/structure.cpp` | Cantilever fails at the predicted load; heat buckles a beam |
| S10 | P1 | L | Combustion O2/products | `sim/air.*`, `sim/thermal.cpp` | Sealed room extinguishes; CO/soot produced |
| S11 | P1 | L | Electrical resistance/voltage/fuses | `sim/electric.cpp`, `sim/materials.*` | Lamp dims with wire length; fuse blows at rated current |
| S12 | P1 | M | Power-conserving radiation | `sim/thermal.cpp` | Two-plate test converges to the analytic equilibrium |
| S13 | P1 | L | CCD for bodies | `sim/physics.cpp` | 60 m/s projectile cannot cross a 1-voxel wall |
| S14 | P1 | M | Benchmark mode + budget CI | `main.cpp`, `game/game.cpp`, CI | `--benchmark` reports JSON; CI fails on >10% regression |
| S15 | P2 | M | Tutorial/onboarding | `game/hud.cpp`, `game/game.cpp` | New save reaches "workshop" in <10 min without wiki |
| S16 | P2 | L | Campaign/scenario framework | `game/game.*`, new `game/scenarios.*` | 3 scenarios with win/lose conditions and saves |
| S17 | P2 | M | Input remap + controller | `platform/*`, `game/game.cpp` | Bindings persist; controller can play the tutorial |
| S18 | P2 | M | Localization-ready strings | `ui/*`, all UI call sites | No user-facing literal outside a string table |
| S19 | P2 | M | Accessibility pack | `ui/ui.*`, settings | Colour-blind palette, text scale, motion reduction, captions |
| S20 | P2 | L | Audio backend + mix | `game/audio.*` | Sound on Windows and Linux; buses, occlusion, captions |
| S21 | P2 | L | Renderer quality presets/upscaling | `gfx/*` | 60 fps at 1080p on reference; visual A/B screenshots |
| S22 | P3 | M | Blueprint metadata/browser | `game/workshop.*` | Thumbnail, author, tags, search; not just filenames |
| S23 | P3 | M | Play-mode undo/redo | `game/game.cpp`, `sim/sim.*` | 20-step undo of build/destroy, memory bounded |
| S24 | P3 | M | Crash handler + diagnostics | `platform/*`, `main.cpp` | Crash writes a log/minidump and offers a copy path |
| S25 | P3 | M | Installer/portable packaging + signing | new `packaging/` | Clean machine install/run/uninstall; version visible |
| S26 | P3 | M | ASan/UBSan/TSan + fuzz CI | `.github/workflows/` | Sanitizer self-test + fuzz corpus green |
| S27 | P3 | M | Content linter (min feature size) | `tools/`, `game/parts.cpp` | Reports any part feature clipped at 0.02/0.025 |
| S28 | P3 | L | Compressed bricks / palette | `sim/volume.h` | ≥2× memory reduction with identical self-test |
| S29 | P3 | M | Water conservation soak | `sim/fluids.cpp` | 10-minute random water test, mass drift <0.1% |
| S30 | P3 | S | Fix settle/debris loss paths | `sim/sim.cpp`, `damage.cpp` | No voxel/water vanishes in a targeted test |

---

## 10. Testing strategy

The current self-test is the anchor. Build outward:

### 10.1 Layers

1. **Pure unit tests** (`tests/unit`, no dependency): `packKey` round-trip,
   settings parsing/sanitize, `Settings` round-trip, `Material` derived values,
   water fill encoding, blueprint record bounds, wire route on a synthetic grid,
   quaternion/matrix inverse, `Volume` set/put/get invariants.
2. **System tests** (extend `--selftest`): one test per shipped machine, per
   material phase, per conservation law. Keep the real scene.
3. **Scenario/golden tests**: load a fixed save/seed, run N steps, compare a
   state hash and a handful of physical quantities to tolerances. Put hashes in
   `tests/golden/`.
4. **Soak tests**: 10-minute random interactions (scripted) with assertions on
   body/debris/FX counts, water mass, NaN, memory growth.
5. **Fuzz tests**: `.rbp` loader, settings loader, script parser; ASan/UBSan.
6. **GPU smoke test**: Xvfb + Mesa `llvmpipe`, capture a frame, assert variance
   and that key pixels are not black; run one scripted explosion.
7. **Performance tests**: fixed benchmark scene, per-system timings, CI budget.
8. **Compatibility matrix**: Windows 10/11, Linux X11, integrated + discrete
   GPU, 8 GB and 16 GB RAM, 1080p/1440p, controller.

### 10.2 Environment switches

- `REALISM_DEBUG_ELEC=1` — circuit nets, sources, loads.
- `REALISM_DEBUG_AIR=1` — multigrid residual and profile.
- `REALISM_DEBUG_CONTACTS=1` — contact list (very noisy).
- `REALISM_DEBUG_WIRE=1` — `routeWire` failure endpoints.
- `REALISM_DEBUG_SIM` — *proposed*: per-step invariants (NaN, bounds,
  conservation) and state hashing. Add this early; it pays for itself.

### 10.3 Acceptance examples

Write these as executable checks, not prose:

- A 1 kg steel cube dropped 2 m reaches ~6.26 m/s before impact (within 5%).
- A 1000 W heater raises 1 L of water by ~14.3 K in 60 s (within 10%).
- Ice at -10 °C in a 20 °C room reaches 0 °C and holds a plateau proportional to
  its latent heat before melting.
- A sealed 1 m³ steel box floats in water; the same box filled with water sinks.
- A 10 m oak cantilever with a 100 kg tip load fails at the joint predicted by
  the stress model.
- 10 minutes of random demolition loses <0.1% water mass and zero bodies to NaN.
- Save at t=60 s, load, run 60 s: state hash matches the uninterrupted run within
  fixed-point tolerance.

---

## 11. Content authoring rules

These rules keep parts behaving at every supported voxel size:

1. **Metres only.** All shape positions/sizes are metres relative to the part's
   local origin; the rasterizer converts to voxels.
2. **Minimum feature ≥ 2 voxels** at the coarsest supported size (0.025 m → 5 cm
   minimum for anything that must survive). Terminals, leads, tabs and windings
   are the usual offenders; the current templates special-case them.
3. **Electrical contact is face-adjacent.** A wire must share a face with a
   conductor; a gap of one cell is an open circuit. Terminals of opposite
   polarity must never be face-adjacent.
4. **Wires are 6-connected** and must not touch any conductor except their two
   endpoints; `routeWire` enforces this at build time, but hand-edited
   blueprints can violate it.
5. **A part is one rigid body.** Keep its voxels face-connected; splitting a
   body across a seam also splits its circuit and its heat path.
6. **Templates are rasterized at the current voxel size**; if you add a shape
   thinner than a voxel, it will silently disappear at coarse sizes. Run the
   content linter (S27) and the self-test at 0.02/0.025.
7. **Blueprint files are `RBP1`**: magic, voxel size, component records
   (name/object/type/joint/axis/reversed/capacity/fuel), records (x,y,z,mat,comp
   index). Any format change needs a version bump and a migration path.
8. **Joint choice is gameplay**: `welded`/`cast` parts survive a hammer,
   `clipped`/`glued` parts pop off. Pick deliberately.
9. **Thermal mass matters**: a thin steel shade heats in seconds; a brick wall
   should take minutes at 20× warp. Check with the scanner.
10. **No magic numbers in scene code**: use the material library and the part
    catalogue; if a value is missing, add a material property rather than a
    per-object fudge.

---

## 12. Guardrails for the next AI agent

- **Read this file and the README first.** Then read `sim.h`, `volume.h`,
  `body.h`, `comps.h` before touching simulation code.
- **Do not add dependencies** (SDL, GLFW, glm, stb, EnTT, ImGui, etc.) without
  explicit approval. The custom platform/UI/math stack is part of the product's
  portability and build simplicity.
- **Do not weaken tests or thresholds to make a change pass.** If a test fails,
  find the cause; if the test is wrong, explain why in the commit and add a
  better one.
- **Never edit `.env`, secrets, credentials or user data.** Settings live in
  `plat::userDataDir()`; blueprints under `blueprints/`.
- **Keep units explicit** in names and comments. A variable called `radius` must
  say whether it is metres or voxels.
- **Use `Sim::setWorld/putWorld/removeWorld`** for world edits. If you must go
  direct, repair support/fluid/special/hot/air bookkeeping in the same change.
- **Threading:** no allocation or container mutation inside `parallelFor` unless
  proven disjoint and pre-sized; merge results on the main thread.
- **Body/component ids:** revalidate with `bodyById`/`bodyIndex` after any call
  that can create/remove bodies; never hold a `Body*` across `impact`,
  `explode`, `detachComponent`, `splitBody` or `removeBody`.
- **`Brick` layout:** keep it POD, dense, trivially copyable. If you need a
  field, measure the memory cost at 131 M world cells first.
- **Platforms:** every new platform call needs a `headless.cpp` fallback and a
  non-Windows path, or the headless build breaks.
- **CI:** run `--selftest` at the default and 0.02 m; add a check for every bug
  fix. Keep the Windows compile job green.
- **Docs:** update `README.md` and this file when behaviour, settings, file
  formats or the supported voxel range change.
- **Scope:** this document is the plan. Do not start M3/M4 polish before M1
  save/load and player-body collision are done; those unlock everything else.

---

## 13. Release checklist

- [ ] Save/load round-trip and autosave recovery verified on Windows and Linux.
- [ ] Player collision with bodies; no falling through moving machines.
- [ ] Cross-body circuits or a documented restriction with no broken shipped
      content.
- [ ] Latent heat, displaced-volume buoyancy and water conservation tests green.
- [ ] 10-minute soak: no crash, no NaN, no unbounded memory, 60 fps target on the
      reference machine.
- [ ] Blueprint/settings fuzz and corrupt-file UI paths tested.
- [ ] Audio on all shipping platforms; captions/subtitles; volume/mute persists.
- [ ] Controller, remapping, accessibility and localization pass.
- [ ] Tutorial completes on a clean save; first 15 minutes playtested by someone
      who has not seen the game.
- [ ] Installer/portable build, version stamp, credits, notices, privacy policy.
- [ ] Crash handler writes diagnostics; opt-in report path works.
- [ ] Store assets, trailer, press build, QA matrix, known-issues list.
- [ ] `README`, `HANDOFF`, `THIRD_PARTY_NOTICES` and in-game help agree.

---

## 14. Debug switches and tools

| Switch | Effect |
|---|---|
| `--selftest` | Scene-level integration test, no window/GL, exit 0/2 |
| `--voxel 0.02\|0.025` | Voxel size override; outside range rejected |
| `--script file` | Deterministic command script (needs windowed build) |
| `--shot file --frames N` | Render and save a PPM |
| `--size W H` | Window size |
| `--help`, `--version` | CLI help / version |
| `REALISM_DEBUG_ELEC=1` | Circuit nets/sources/loads |
| `REALISM_DEBUG_AIR=1` | Multigrid residual + air profile |
| `REALISM_DEBUG_CONTACTS=1` | Contact dump |
| `REALISM_DEBUG_WIRE=1` | Wire-route failure endpoints |
| `F3` in game | Live system timings and counts |
| `--script` commands | `pos`, `wait`, `tool`, `press`, `click`, `holdclick`, `release`, `mouse`, `shot`, `title`, `menu`, `workshop`, `play`, `blueprint`, `launch`, `explode`, `toggle`, `switch`, `smooth`, `log`, `probe`, `quit` |

Use `log` and `probe` when reproducing physics/air bugs; they are the only
non-visual observability in the windowed build today.

---

## 15. Open product decisions

1. **Voxel range:** keep 0.02–0.025 (honest, content-limited) or invest in
   voxel-size independence and widen to 0.01–0.05? The latter is an M2 item, not
   a quick fix.
2. **Save format:** binary (compact, fast, harder to debug) vs JSON (human
   readable, larger). Recommendation: binary payload with a JSON header, or
   MessagePack-like if a dependency is ever approved. Version from day one.
3. **Platforms:** Windows only, Windows+Linux, or macOS too? Audio and GPU
   abstraction costs scale with this.
4. **Online:** blueprint sharing only, or multiplayer? Multiplayer is a
   different physics/netcode project; do not promise it casually.
5. **Art direction:** keep the current clean-material look or move to authored
   textures/PBR? The renderer is voxel-ray-marched; textures change the shader
   and content pipeline.
6. **Monetization:** premium one-off vs early access vs DLC. It affects scope,
   tutorial and QA, not the simulation.
7. **Narrative:** pure sandbox, scenario-based, or survival? Each implies a
   different save model and content pipeline.
8. **Determinism:** is replay/netcode a requirement? If yes, seed and ordering
   work moves to P0.

---

## Appendix A — file map (quick)

```
src/main.cpp                 CLI, Windows entry
src/core/math.h              vec2/3/4, ivec3, mat3/4, quat, Rng, hash3
src/core/jobs.*              thread pool, parallelFor
src/platform/platform.h      window/input/time/paths/message box interface
src/platform/win32.cpp       Win32 + WGL
src/platform/x11.cpp         X11 + GLX
src/platform/headless.cpp    no-op platform for self-test/CI
src/gfx/gl.*                 GL 4.3 loader
src/gfx/shader.*             shader compile/link/uniforms
src/gfx/renderer.*           ray-marched world/bodies, shadows, bloom, volumes
src/gfx/shaders.h            GLSL sources (world, body, fx, composite, volume)
src/ui/ui.*                  immediate-mode 2D UI, SDF text
src/ui/font_data.h           generated font atlas (do not hand-edit)
src/sim/volume.h             Brick/Volume, packKey, DIRS6, glow
src/sim/materials.*          material library + derived per-voxel values
src/sim/body.h               Body, Sample, RotPart
src/sim/comps.*              Component registry, joints, rated power
src/sim/sim.*                Sim orchestration, world edits, support, bodies, rays
src/sim/physics.cpp          contacts, impulses, sleep, friction heat
src/sim/thermal.cpp          conduction, fire, phases, radiation
src/sim/air.*                stable-fluids air grid
src/sim/fluids.cpp           water fill/pressure, powders/molten, debris
src/sim/electric.cpp         nets, sources, loads, motors, rockets, fuses
src/sim/damage.cpp           impact, fracture, explosion, addHeat
src/game/scene.cpp           the yard and its machines
src/game/assemblies.*        assembly catalogue + builders
src/game/parts.*             part templates, rotations, stamping, capture
src/game/build.*             rasterizer, routeWire, spawnBody
src/game/workshop.*          build jig, editing, analysis, blueprints
src/game/game.*              player, tools, modes, script, reset
src/game/hud.cpp             HUD, menus, title, self-test
src/game/audio.*             procedural synthesis + mixer
src/game/settings.*          settings load/save/sanitize
tools/gen_font.py            font atlas generator
```

## Appendix B — material phases and derived values

`Mat` enum in `materials.h`: AIR, BEDROCK, STONE, DIRT, GRASS, SAND, WATER, OAK,
PINE, LEAVES, BRICK, MORTAR, CONCRETE, STEEL, IRON, COPPER, ALU, GLASS, BULBGLASS,
RUBBER, PLASTIC, LIION, TERM_POS, TERM_NEG, FILAMENT, MOTOR, SHAFT, SWITCH_ON,
SWITCH_OFF, NICHROME, SOLAR, CHARGE, PROPELLANT, ASH, MOLTEN, TILE, CHARCOAL,
BRASS, GRAVEL, BASALT, CANVAS, BARK, MAGNET, GOLD, PAINT_RED, PAINT_BLUE,
PAINT_WHITE, CARBON, ICE, STEAM.

Phases: `GAS`, `SOLID`, `POWDER`, `LIQUID`. `isFluidish` = liquid or powder.
`isSolid` is phase SOLID. Fire uses `ignite`/`burnTime`/`burnPower`/`flameTemp`/
`residue`. Melting uses `melt`/`solidifyTo`; boiling uses `boil`. Electrical
conductors set `wire`; devices are special materials handled in `electric.cpp`.
Per-voxel mass/heat capacity/toughness/power are derived from density, cp,
toughness, burnPower and the current voxel volume in `initMaterials`.

## Appendix C — controls

Movement WASD/Space/Shift/Ctrl, mouse look, E use, F flashlight, N fly, T slow
motion, P pause, V smooth/blocky, Tab workshop, F1 help, F2 HUD, F3 stats,
F11 fullscreen, Esc menu, 1–9 tools, G detonate remotes, Q build material,
Z build size, X salvaged part, R/F rotate parts in workshop, M mirror,
Ctrl+Z/Y workshop undo/redo, 1–5 workshop tools (place/erase/paint/pick/wire),
[/] brush size, Enter launch, wheel zoom (orbit camera in workshop).
