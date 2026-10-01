# Offbeat

C23 music player (Linux/Wayland/OpenGL first).

## Goals
- Simple, performant, self-contained OpenGL 4.6 behind a replaceable renderer interface.
- Simple, performant UTF-8 strings, SDF fonts, shapes/images, VM allocation and input.

## Structure
Every new function in `src/<layer>/`, including static helpers, uses its layer prefix: `core_`, `platform_`, `game_`; avoid unrelated legacy renames.
- `src/core` -> reusable mechanisms; `src/game` -> portable Offbeat policy.
- `src/platform` -> ALL OS code, including VM, environment, file/tool/font discovery and native synchronization.
- `src/third_party` -> stb-style libraries; `src/main.c` -> thin layer wiring/frame loop.
- `tests/` -> unit tests; `tools/` -> development tools.
Keep backend-specific handles/shaders inside renderer backends where possible; do not spread GL assumptions into game APIs.

## Reuse first
- Before adding a helper, search core and existing callers. Use `python3 tools/list_functions.py` (JSON, layer filters and prefix checks available).
- Use existing core string, memory, math, hash, RNG and image APIs. Extend a missing contract instead of adding a local near-copy.
- If an app implementation is better, promote it to core and migrate duplicates. Preserve caller semantics, persistent hashes/cache keys and RNG sequences; measure performance claims.
- Keep domain policy and distinct access/ownership contracts local; do not force specialized readers, caches or queues into one generic abstraction.
- Use `Core_String` views for immutable text. Keep bounded mutable C strings for editing, worker ownership and external APIs; bridge with core helpers.
- Use core copying/formatting/comparison and UTF-8 boundaries; avoid byte-cut truncation and pure `snprintf(..., "%s", ...)` copies. NUL termination, capacity, malformed input and OOM must be explicit.

## Ownership and correctness
- Use tracked core allocation for app-owned buffers/objects, including platform objects. Check size arithmetic, alignment, allocations and growth before publishing new state; preserve the old allocation on realloc failure.
- Pair creation with complete teardown and partial-init cleanup: heap/arenas, file maps, threads, sync primitives, GPU resources and effects. Destroy GPU resources before their context.
- Bound file bytes and decoded dimensions/pixels before decoding. Account for overlapping snapshots, scratch, queued results and concurrent decodes, not just steady state.
- Shared mutable state needs the same lock on every access or an explicit atomic ownership protocol. Atomic indices alone do not make concurrently overwritten ring-buffer payloads safe.
- Keep blocking disk/process work outside UI/audio paths and long-held locks. File replacement must handle concurrent writers inside platform, not rely on caller-specific locks.
- Propagate shader/link, I/O, thread-start and initialization failures; do not return nominally valid handles after failure.
- Existing issues and further extraction candidates: `docs/architecture-audit.md`. Do not copy a known limitation into new code.

## Build and constraints
- `./build.sh [debug | test]`; defaults to `debug` (tcc). `./build.sh test` builds and runs unit tests with AddressSanitizer and UndefinedBehaviorSanitizer (gcc).
- Must stay sub 1 sec even at 100k lines, with new `.c` files picked up automatically; report actual build mode/timing.
- App-owned memory budget: <30 MB, excluding fixed GL driver cost. Tracked payload/arena counters alone do not prove total ownership or residency; include untracked overhead when evaluating the budget.
- No heavy dependencies (ffmpeg, SDL, ...) without asking the user.
- Run meaningful affected tests and normal builds; use sanitizers for memory/concurrency changes. Cached vendor objects may be uninstrumented; sanitizer success is not proof of race freedom.

## Visual verification
Use headless screenshots (mute) with isolated cache/config, rather than a live window:

    OFFBEAT_CACHE_DIR=/tmp/offbeat-cache OFFBEAT_CONFIG_DIR=/tmp/offbeat-config \
    OFFBEAT_SHOT=/tmp/x.png OFFBEAT_DEMO="find=bored;seek=60;tab=1;debug" ./build/offbeat

Inspect the result; asynchronous loading can differ between captures. Do not disable leak checks and then claim leak-free lifecycle.
