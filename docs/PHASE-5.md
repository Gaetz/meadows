# Phase 5 — Multithreading architecture (DONE 2026-06-15)

> Journal of the Phase 5 implementation, same role as `docs/PHASE-1..4.md`.
> Read before touching the render seam (`game/SceneSubmit`), the job/queue
> primitives (`engine/core/`), or the async asset path (`game/TextureCache`).

Phase 5 is the first point where async is **forced** (it precedes streaming).
Its job is to lay the **seam**, not to build streaming: define the thread model,
the JobSystem↔flecs boundary, and the async asset-residency path. **No render
thread, no real cell streaming** (Phase 8). Everything was single-threaded before
this; gameplay randomness stays on the engine RNG (§8) so saves/replays remain
reproducible.

## Architecture decided with the dev

The load-bearing reframe: **decouple ≠ thread.** The CLAUDE.md pre-framing
assumed "render stays on the main thread" as an invariant; we replaced that with
a *policy*.

- **Strict render snapshot (extract phase).** The sim produces a **self-owning
  POD packet** (`RenderSnapshot`) each frame; the renderer reads **only** that
  packet and has **no access to the `World`**. `render::Sprite` already carries a
  resolved `rhi::TextureHandle` (POD, no live pointers into the ECS or assets), so
  the packet is passed **by value** — whether its consumer runs on the same
  thread, a render thread, or a pipeline of frames becomes a **scheduling
  decision, not a rewrite**. Same-thread stays the default (a render thread buys
  ~nothing for an instanced 2D renderer and costs state double-buffering + a frame
  of latency).
- **GL caveat.** The GL context is thread-affine: *if* a render thread is ever
  added it becomes *the* GL thread and asset uploads migrate to it. Uploads stay
  **behind the RHI** so this remains a backend detail (and so a Vulkan backend can
  use parallel command recording + a transfer queue instead — the render-thread
  model is a GL-era optim).
- **Main owns the ECS world and the GPU; a worker touches neither.** Workers
  produce results into a **non-blocking completion queue**, drained at a fixed
  point each frame; never `JobSystem::wait()` on the frame thread. Results are
  applied in a **deterministic order** so completion order never perturbs the RNG
  or saves (§8).
- **JobSystem owns task parallelism** (I/O, asset decode, later per-cell
  resolution); **flecs systems stay single-threaded** until profiling justifies
  parallel systems (likely Phase 10/11). These are **orthogonal axes** —
  heterogeneous-task parallelism (JobSystem) vs. intra-system parallelism over
  entities (flecs scheduler); not to be conflated.

## Bricks

### (5a) Strict render snapshot — DONE 2026-06-15
- `game/SceneSubmit` split from a single monolithic pass into **EXTRACT** +
  **SUBMIT**:
  - `RenderSnapshot` — a self-owning `vector<render::Sprite>` already in painter
    order (the contractual ECS↔renderer boundary).
  - `extractScene(world, textures) → RenderSnapshot` — the **only** step that
    reads the `World` (query + texture resolve + stable painter sort).
  - `submitSnapshot(snapshot, renderer)` — pure consumer: touches neither `World`
    nor `TextureCache`, only the packet and the renderer.
- The old `submitScene(world, textures, renderer)` (which re-coupled world +
  renderer) was **removed**; `WorldDemoScene::draw` now does extract→submit
  explicitly so the seam is visible at the call site.
- The procedural ground tiles in the demo still call `renderer.draw` directly —
  demo scaffolding, outside the ECS seam contract (noted, not in scope to fix).
- Validation: GL path exercised by running the game (established precedent);
  `spriteFor` mapping stays unit-tested (`tests/SceneSubmitTest.cpp`).

### (5b) Non-blocking completion queue — DONE 2026-06-15
- `engine/core/ConcurrentQueue.hpp` — header-only, mutex-guarded MPSC mailbox
  (§10: simplest thing; a lock-free ring buffer only if profiling shows the lock
  hot). `push` (any thread), `drain(fn)` (consumer: swaps the buffer out under the
  lock, then invokes `fn(T&&)` in FIFO order **outside** the lock — producers keep
  running, and `fn` may re-push without deadlock), `tryPop`/`empty`/`size`.
- **The point vs `JobSystem`:** it **never blocks the consumer**. `JobSystem::wait`
  stalls the caller until a batch finishes; draining this stalls the frame on
  nothing — apply what's ready this frame, pick the rest up next frame. Ordering
  is FIFO but cross-producer interleaving is timing-dependent, so **deterministic
  application is the consumer's job** (sort the drained batch, e.g. by GUID, §8).
- Tests `tests/ConcurrentQueueTest.cpp`: FIFO, drain-empties, re-push-from-
  callback without deadlock, and an 8-producer × 10 000 stress proving nothing is
  lost or duplicated.

### (5c) Async asset residency + loading gate — DONE 2026-06-15
- `game/TextureCache` went from synchronous-blocking to async:
  - `resolve(guid)` (main, during `extractScene`) **never blocks**. A first
    sighting marks the entry `Pending`, returns a **placeholder** (a tiny built-in
    magenta checker — distinct from the white "missing" fallback), and enqueues a
    decode job. The worker runs `loadImageFile` (pure file IO + decode) and
    **pushes** the result into the `ConcurrentQueue`.
  - `pumpUploads()` (main, at a fixed frame point — top of `draw`) drains the
    queue, does the **`createTexture` GPU upload on the main thread** (GL is
    single-threaded), and flips the handle `Pending → Resident`.
  - **Cancellation / epoch:** `clear()` (mod re-resolution) bumps a `generation`;
    in-flight results carry the generation they were kicked under and are **dropped
    on arrival** if it no longer matches — also covers the double-job-per-guid
    case (a single `createTexture`, no leak).
- **Loading gate (§7) — no startup pop-in:**
  - `prewarmTextures(world, cache)` (in `SceneSubmit`) kicks the decode of every
    sprite asset in the world **without drawing**. Generalizes to streaming:
    preload the cells around the player before they are on screen.
  - `TextureCache::pendingCount()` (O(1) counter) lets a gate poll readiness.
  - `WorldDemoScene`: after `rebuild()`, prewarm + arm `loading`. At the top of
    `draw()` (after `pumpUploads`), while `pendingCount() > 0` it draws **only**
    `drawLoadingScreen` (a cover quad + progress bar, centred on the camera so it
    covers the view wherever it is) and returns — the world (and its placeholders)
    stays hidden until everything is resident, then reveals. Re-triggers on mod
    toggle (which routes through `rebuild()`).
- **Lifetime fix (teardown).** The decode job must not touch the cache after it
  dies: the completion queue lives in a heap `Shared` block owned by a
  `shared_ptr`; the worker captures the `shared_ptr` (not `this`), so an orphaned
  job pushes harmlessly and frees `Shared` when the last worker drops its ref. The
  destructor does **no `wait`** — this sidesteps the destroy-during-`notify_all`
  race intrinsic to `JobCounter` (a latent sharp edge in the primitive: any client
  that `wait`s then immediately destroys the counter can tear down its condition
  variable while a worker is mid-notify; left for a future `JobCounter` hardening).
- Validation: GL/residency path by running the game; `ConcurrentQueue` unit-tested.

## Postmortem — the "heap corruption" that wasn't a code bug

After the loading-gate brick the dev hit intermittent crashes (an STL
`deallocate` assert "null pointer cannot point to a block of non-zero size", then
an access violation inside `ImGui::BulletText` in `PluginScene::drawUi` reading a
`report` that should have been empty). Both were the **same heap-corruption
symptom surfacing at different next-allocations**.

Root cause: a **stale incremental object file**, not the code. Adding
`loading`/`loadTotal` members to `WorldDemoScene.hpp` changed the class layout;
ninja recompiled `WorldDemoScene.cpp` but **missed the transitive header
dependency of `DemoScenes.cpp`** (which defines the derived `PluginScene` etc.).
The running binary mixed the **new layout** (base TU) with the **old layout**
(derived TU) → derived code read `report`/`forms`/`model` at the wrong offsets →
corruption.

Diagnosis path worth remembering: a headless run with `SetUnhandledExceptionFilter`
+ `StackWalk64` (dbghelp) dumped a symbolized stack pinpointing the victim line;
object-file timestamps then showed `DemoScenes.cpp.obj` predating the header edit.
**Fix: clean rebuild** (delete our `*.obj`, keep `_deps`). The async/threading code
was correct throughout. **Lesson: after changing the layout of a shared type, do a
clean rebuild — this build tree's header-dependency tracking is not fully
reliable.**

---

**Phase 5 complete (2026-06-15).** 112 test cases / 803 assertions green; full
build clean; `true-adventurer.exe` runs (verified looping, not blocked). New
primitive `core::ConcurrentQueue`; the render seam is now a strict by-value
snapshot; assets stream in via worker-decode → main-upload with a no-pop-in
loading gate.

## Out of scope (Phase 8+ / deferred)
Real cell streaming (async load/unload, LOD, transitions) and **save = runtime
patch layer** → Phase 8 (the first true clients of this seam). A dedicated render
thread / frame pipelining → only when render cost justifies it (the strict
snapshot keeps it cheap). flecs multi-threaded systems → Phase 10/11 on profiling.
`JobCounter` destroy-during-notify hardening → when a second client needs the
wait-then-destroy pattern.
