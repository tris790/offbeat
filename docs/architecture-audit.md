# Architecture and reuse audit

Audit date: 2026-09-30. Scope: all first-party C sources and headers, build/test organization, third-party integration, and relevant tooling. The discussion distinguishes changes made during this audit from further extraction opportunities and correctness issues. Performance comparisons describe algorithms; they are not benchmark results.

## Structure and dependency map

The basic organization is sound: OS implementations are concentrated in `platform/`, drawing/text/allocation primitives in `core/`, and music/application policy in `game/`. First-party code was about 17,125 lines at the start of the audit. `app.c` alone was 3,605 lines. The main architectural pressure comes from reusable mechanisms accumulating inside large application modules, rather than widespread direct Wayland/ALSA calls from application code.

| Module | Responsibilities | Dependencies and reuse assessment |
| --- | --- | --- |
| `src/main.c` | Startup, frame pacing, window state, screenshot/recording output | Calls platform, renderer, app, and stb PNG writer. Larger than the stated very thin entry-point goal; capture and window-state helpers could become separate modules. |
| `core/types.h`, `core/math.h` | Numeric types, vectors, matrices, geometry, colors | Portable foundation; many 3D helpers are currently useful library surface rather than app necessities. |
| `core/memory.*` | VM arenas, temporary marks, tracked heap | Appropriate core policy; native reservation/commit operations belong behind platform hooks. |
| `core/string.*` | UTF-8 views, copying, codec, arena builder | Right representation for application text; original surface was too small to prevent local comparison/copy/format helpers. |
| `core/hash.h`, `core/random.h`, `core/image.*` | Stable identity hashes/mixers, explicit RNG state, allocation-free linear-light square resizing | Added by this audit to consolidate previously duplicated mechanisms. |
| `core/gl.*`, `core/renderer_gl.c` | GL loading, instanced analytic shapes, texture batching, effects | Direct GL calls are confined to this backend. Loader table and GLSL programs remain backend-specific. |
| `core/renderer.h` | Renderer-facing shape, texture, effect API | Drawing is abstracted; creation and programmable effects still expose OpenGL assumptions. |
| `core/font.*` | Lazy mapped faces, fallback chains, glyph/kerning caches, shelf atlas | Reusable text layer. Actual glyph rendering uses coverage bitmaps and three subpixel phases, not SDF glyphs. |
| `platform/platform.h` | Window/input/timing/files/threads/process/audio facade | Correct boundary for OS capabilities, but broad transitive includes and missing teardown primitives reduce reuse. |
| `platform_linux_wayland.c` | Wayland/EGL window, input, cursors, headless context | Appropriate platform placement. |
| `platform_linux_alsa.c` | Blocking PCM sink and muted real-time sink | Appropriate platform placement. |
| `platform_posix.c`, `platform_memory_posix.c` | File/path/process/thread/timing/memory-info and native VM implementation | POSIX/native details stay here; some application-specific directory policy is embedded in the facade. `platform/memory.h` supplies the narrow core allocator boundary. |
| `game/app.*` | Lifecycle, playback queue/likes/session, library adoption, navigation, views, shortcuts, demos | The central integration point also contains state containers, input editing, list scrolling, and widgets that could be separated. |
| `game/library.*` | MP3/FLAC/Ogg metadata parsing, snapshot indices, binary cache, incremental background scan | Music parsing belongs in game; sorting, interning, binary readers, and bounded file buffering are generic candidates. |
| `game/player.*` | Codec selection, exact incremental MP3 seeking, PCM mixing/fades, audio command mailbox, visualization sample history | Audio/music engine can be reused as a separate subsystem. Command/state machine is specialized and should remain distinct from general worker queues. |
| `game/spectrum.c` | Hann window, real FFT, logarithmic bands, smoothing, onset detection | Generic DSP embedded in game, with process-global mutable tables. |
| `game/covers.*` | Background image decode, decode budgeting, crop/resample, thumbnail atlas/LRU, disk cache, palettes | Music cover lookup and cache naming are domain-specific; image processing and parts of atlas/cache machinery are reusable. |
| `game/ytthumbs.*` | Remote thumbnail fetch/cache, worker handoff, crop/resize, atlas/LRU | Shares image and cache mechanisms with covers, but has different keys and request policy. |
| `game/download.*` | Song identity normalization, parsing tool output, persistent jobs, retries/concurrency, search/enrichment, ffmpeg tagging | Music/download policy belongs in game; process discovery and some string/queue plumbing are generic. Existing subprocess integration invokes external tools rather than adding a linked ffmpeg dependency. |
| `game/download_view.*` | Download page, status/dock, result/job lists, selection, query editing | Reimplements scrolling, window controls, animation/color helpers, and editing already present elsewhere. |
| `game/ui.*` | Immediate interaction, stable IDs, animation cache, text alignment, particles/ripples, icons | Mostly reusable UI foundation mixed with Offbeat palette/fonts/icons. Could separate mechanisms from product theme. |
| `game/search.*`, `game/filter.*` | Ranked multi-field matching, KMP substring plus subsequence scoring | Good sharing of one compiled matcher between filtering/search. Fold ownership originally created a circular module dependency; moving Latin folding to core removes it. |
| `game/settings.*` | Settings text format, themes, hue/saturation matrix | Settings schema and theme catalog are domain policy; generic color-matrix application now uses core math. |
| `game/queue.*` | Remove every occurrence of a track from queue/original arrays | Much of the queue state and all shuffle/insert/remove/play orchestration remains in app. Expand this domain module before inventing a generic collection framework. |
| `third_party/impl.c` | One optimized implementation unit, tracked allocation hooks | Good central allocation integration. Keep vendor edits out of generic extraction work. |
| `tests/*.c` | Standalone unit programs including implementation files | Fast and simple, but inclusion can mask missing includes/link dependencies and creates static-symbol collision pressure. |
| `tools/` | Demo recording, fake download tool, Python music synchronization | Appropriate development/integration tooling. The Python sync workflow is separate from the application C abstraction surface. |

The dependency graph is effectively `main -> game + platform + core`, `game -> platform + core + third_party`, and `platform -> core types/interfaces`. Core's required platform facilities should use narrow capability headers rather than the whole window/audio facade.

### Source evidence index

Line numbers below identify the reviewed code after the initial consolidations; function names remain the reliable locator if later edits shift lines.

| Finding | Evidence |
| --- | --- |
| Shared scrolling candidate | `src/game/app.c:1471` (`list_begin`), `:1522` (`list_end`); `src/game/download_view.c:302` (`scroll_update`), `:323` (`scroll_bar`) |
| Shared editing candidate | `src/game/app.c:1841` (`game_panel_filter_input`); `src/game/download_view.c:1019` (`edit_query`) |
| Duplicate window controls | `src/game/app.c:1727`; `src/game/download_view.c:984` |
| Generic sort/interning | `src/game/library.c:771` (`lib_sort`), `:963` (`lib_blob_put`), `:1236` (`lib_intern`) |
| Heap byte builder | `src/game/covers.c:412` (`cov_mem_write`) |
| Thumbnail cancellation/decode/startup | `src/game/ytthumbs.c:56` (`fetch_and_decode`), `:113` (`ythumbs_create`), `:127` (`ythumbs_destroy`) |
| Visualization ring producer/consumer | `src/game/player.c:627` (`player_fill`), `:996` (`player_vis_samples`) |
| Teardown omissions | `src/game/app.c:867` (`app_shutdown`); `src/main.c:164` onward; `src/core/renderer_gl.c:325` (`core_renderer_destroy`) |
| Renderer GL-facing interface | `src/core/renderer.h:43` (`Core_GlLoadProc`), `:171` (`core_effect_create`); `src/game/app.c:884` (`BACKGROUND_FX`) |
| Shader failure handling | `src/core/renderer_gl.c:239` (`compile_shader`), `:253` (`link_program`) |
| Font map ownership | `src/core/font.c:67` (`face_load`) |
| Native discovery/environment leakage | `src/game/app.c:805` (font fallbacks), `src/game/download.c:624` (`find_tool`), `src/main.c:75` (`setenv`) |
| Atomic file write / caller serialization | `src/platform/platform_posix.c:170`; `src/game/covers.c:142` (`write_mutex`) |
| Disk write under download mutex | `src/game/download.c:610` (`save_locked`) |
| Mutable DSP layout | `src/game/spectrum.c:53` (`spectrum_layout`) |


## Consolidations selected during this audit

These concrete duplication findings are addressed by the implementation work accompanying this report. Consult the final diff for exact APIs and callers.

1. **Strings and UTF-8.** Library-local NUL-terminated arena copying and validation are stronger requirements than the original core copy/codec provided. Shared bounded C-string copies, ASCII case-insensitive comparison, formatting, folding, and valid UTF-8 primitives belong in core. C-string buffers remain appropriate at OS/codec/process boundaries and for fixed-size worker jobs; converting every `char *` into an allocated view would increase work without improving ownership.
2. **Image resampling.** The original `covers.c::cov_resample` was materially more capable than `ytthumbs.c::to_slot`: exact fractional area averaging when reducing, bilinear interpolation when enlarging, linear-light RGB averaging, and grayscale/alpha channel support. Promote the covers implementation to core image processing and use it for both sources. This improves quality and avoids maintaining two algorithms; it does **not** establish a CPU speedup. Preserve bounded caller-owned scratch and initialize immutable conversion tables before starting workers.
3. **Randomness.** `app.c::rng_next`, `download.c::rng_next`, and `ui.c::frand` repeated xorshift32. Share the state transition while retaining independent state/seed policy; the download instance state was better for reuse than a process-global app/UI generator. Keep existing sequences where possible.
4. **Color/vector math.** `covers.c::cov_hsv_to_rgb` duplicated `core_hsv_to_rgb`; `ui.c::ui_mix` duplicated component interpolation. They now use core. The cover implementation's `h - floorf(h)` normalization replaces `fmodf` in core without a measured performance claim. Core also now supplies RGB-to-HSV, weighted RGB brightness, and row-major clamped 3x3 color transforms shared by theme preview and renderer colors. App/download cubic easing and normalized clamping use core helpers. Identical violet hash-derived placeholder colors share `game_ui_hash_color`; that product palette remains in game/UI.
5. **Arena/VM boundary.** Native virtual-memory operations originally lived directly in core. Native reservation/commit/release now live behind `platform/memory.h`; reusable allocation policy stays in core, with stronger overflow/alignment handling.
6. **Stable hashes.** Repeated FNV-1a byte/C-string hashing, the font's Murmur finalizer, and covers' SplitMix finalizer now live in `core/hash.h`. Keep the different finalizers distinct: replacing persistent cover hashing with another mixer would invalidate caches or alter palettes.

The string work also gives the arena builder a sticky failure flag and safe failure returns, and extends the last arena allocation in place instead of retaining abandoned geometric growth buffers. Download-query input now truncates at a complete UTF-8 sequence; thumbnail object/output allocations gained checks during resampler migration, and the thumbnail cancellation flag is now atomic. These small correctness improvements do not resolve the broader lifecycle or decode-budget concerns below.

## String ownership and missed core use

Eight duplicate copy helpers were replaced: library arena/heap copies, app frame/bounded copies, downloader bounded copy, download-view copy, player path copy, and POSIX arena copy. The library's NUL-terminated, OOM-aware copy was the better implementation and is now the core copy contract. The downloader's complete-codepoint truncation behavior is now shared by the formerly byte-cut app/player/settings/font/UI copies. All first-party pure `snprintf(..., "%s", ...)` copies now use core bounded copying; ordinary formatting remains appropriate where format processing is needed.

ASCII inspection/trim/comparison and UTF-8 previous/next boundary operations are shared. Search's reusable Latin fold moved into core, breaking filter's dependency on search. Formatting retains the app's 512-byte stack fast path but allocates complete longer results instead of truncating them. Core's decoder now rejects overlong encodings, surrogate scalars and values above U+10FFFF; the old library validator depended on that permissive decoder and therefore did not repair it.

`Core_String` remains the right immutable metadata/view representation. Editable settings paths, search text, bounded worker jobs and subprocess protocol lines retain stable mutable C-string buffers. OS, decoder and font-library interfaces also require C strings. Core copy/view bridges make those boundaries explicit; mechanically replacing every `char *`, `strcmp`, `strchr` or `memcpy` would not improve them. Checked path joining and a shared text-edit state remain useful opportunities. No duplicate JSON parser was found in first-party C sources.

Platform thread/process/window objects now use the tracked core heap instead of raw `calloc/free`, so their payloads appear in owned-memory statistics. Those statistics still omit allocator headers, native-library allocations, thread stack residency and some GPU resources; they should not be described as exact total app-owned physical memory. Reserved virtual ranges and committed arena pages also differ from actual resident pages.

## Remaining extraction opportunities

### High value: shared UI mechanisms

**Scroll/list model.** `app.c::list_begin/list_end` and `download_view.c::scroll_update/scroll_bar` repeat wheel handling, precise-scroll policy, exponential interpolation, bounds, scrollbar geometry, inactivity fade, and clipping/visible-range calculation. The app variant additionally supports dragging and virtualization. Extract a reusable `Core` or shared UI scroll state with update/geometry functions, then let app views handle `queue_follow` and drawing style. Adopting the richer app behavior would give downloads scrollbar dragging without a second implementation. Retain caller-supplied IDs/content dimensions, and test wheel/drag/bound changes before changing visual behavior.

**Text editing.** `app.c::game_panel_filter_input` has caret movement, Home/End, Delete, select-all, insertion, and UTF-8-aware navigation. `download_view.c::edit_query` and the command palette implement simpler append/backspace variants. A fixed-buffer UTF-8 editing state is reusable; focus, search submission, escaping, and playback shortcut routing are application policy. Prefer the richer panel implementation, but specify byte offsets and complete-codepoint capacity checks. The original download editor appended arbitrary byte counts and could split a multibyte input at capacity; that truncation bug is addressed by the shared UTF-8 prefix helper, while the broader editing-state duplication remains.

**Window controls.** `app.c::draw_window_buttons` and `download_view.c::draw_window_buttons` have nearly identical three-button interaction/drawing/action loops. Share a widget accepting an ID namespace, window, placement, and opacity. Window move/resize hit testing can share geometry while leaving actual actions behind platform.

**UI foundation/theme split.** Interaction and animation in `ui.c` are not music-specific. Move generic state/interaction/animation/text alignment into a reusable UI module only when making the palette/fonts/styles caller-supplied. Keep product icons, global Offbeat colors, and album/player presentation outside that module. The existing `S(v)` macro depends on a local variable named `ui`, which is convenient but makes helpers less explicit to reuse.

**Scalar animation.** Normalized clamping, cubic easing, and placeholder-color duplication are consolidated. `ui_ease`, scrolling, spectrum smoothing, and theme interpolation still repeat exponential approach factors. A shared pure approach helper is a future candidate; keep settle thresholds and busy-state bookkeeping at the caller. Do not replace `ui_mouse_in` blindly with `rect2_contains`: UI uses half-open maximum bounds and checks enabled/mouse-inside state, while the core rectangle helper includes the maximum edge.

### High value: compact reusable data algorithms

**Indexed stable sort.** `library.c::lib_sort` is a portable bottom-up stable merge sort using insertion-sorted runs and caller-provided scratch/context. Move this as an indexed `u32` sorting helper into core when another caller needs it. It offers predictable `O(n log n)` behavior and no hidden allocation. `download_view.c::sync_lib_keys` uses `qsort` on key/track records: an indexed sort could avoid moving records, but this is a distinct representation, and any speed claim requires measuring library sizes and comparator overhead.

**String interning.** `library.c::Lib_Intern/lib_intern` reuses equal artist/album/genre strings in a snapshot arena. `Lib_Blob/lib_blob_put` deduplicates by pointer+length during serialization. The first is a candidate core string interner; the second is identity-based serialization bookkeeping, not an equivalent content interner. Preserve bounded load factor and explicit fallback/capacity behavior. Changing cache blob deduplication to content-based hashing trades more CPU for potentially less duplicate output.

**Hash/container primitives.** Library lookup, cover lookup/removal, app likes, font glyph/kerning caches, and UI animation storage all use specialized open addressing. Shared mixing/hash algorithms are easy to centralize; one universal map abstraction is less clearly justified. Covers has backward-shift deletion; library is immutable and stores indices; UI is a bounded approximate cache with stale eviction; glyph caching uses reset policy. Their invariants and failure behavior differ. Extract a typed `u64 -> index` primitive only with a clearly specified empty-key/load/deletion contract, preserving narrow entry types where memory matters.

**Byte readers and encoded text.** `library.c` defines big/little-endian loads, ID3 syncsafe decoding, Latin-1/UTF-16 conversion, and a bounded buffered positional reader. Core endian/UTF conversion helpers are reusable and allocation-free; ID3 frame interpretation stays in library. `Lib_Reader` supplies short-lived views into a 16 KB positional-read window, whereas `Cov_Stream` supplies sequential callback reads/skips in a bounded file slice and bypasses copying for large reads. They are useful related abstractions but not interchangeable implementations. A reader interface should explicitly state view invalidation and short-read behavior.

**Binary/heap buffer builder.** `covers.c::Cov_Mem/cov_mem_write` uses tracked realloc, geometric growth, and a sticky failure flag for encoded JPEG output. The arena string builder now extends in place when it owns the arena tail; interleaved allocations still require replacement buffers, and the arena retains those older buffers until reset. Its original missing allocation-failure reporting is fixed by this audit. A heap-backed byte builder with explicit failure and destruction would be more suitable for codec output; it should coexist with an arena builder for cheap temporary text, not replace all arena usage. Binary output must not assume a terminating NUL.

### Medium value: audio and image subsystems

**DSP.** `spectrum.c` can become a reusable core spectrum/FFT module, separating FFT/window math from logarithmic-band/beat display policy. Its mutable process-global sample-rate layout prevents independent simultaneous analyzers with different rates and makes concurrent calls unsafe. Use per-instance plans/layouts or clearly document a single-thread/single-rate constraint. Keep the optimized real-FFT split and caller-owned sample buffers; do not introduce a heavy dependency.

**Audio decoder wrapper.** `Player_Decoder`, codec fallback, channel conversion, and sample-frame seeking are reusable independently of Offbeat queue/session policy. The incremental MP3 seek scanner is a deliberate improvement over the vendor seek-table path: its source comment explains one-pass scanning, delayed work, reservoir resynchronization, and exact seek handling. Preserve it and its existing tests. Do not replace it with a generic whole-file decode or vendor convenience seek-table builder merely to shorten code.

**Atlas/cache building blocks.** Covers and remote thumbnails both own GPU atlases, LRU slots, request/done queues, missing states, and bounded uploads. Covers uses hash lookup, a fixed pixel pool, worker decode budgeting, and separate pending entries/ready slots; remote thumbnails use a small linear slot scan and heap-owned completion buffers. Some covers mechanisms are more scalable, but the remote cache has only 132 slots and a single worker, so a more general cache can cost more state/complexity than it saves. Extract image resampling first; keep URL/file/tag lookup and cancellation policy local. A reusable atlas tile allocator or upload queue is a smaller next step than merging entire subsystems.

**Palette policy.** RGB-to-HSV, weighted brightness, sRGB/linear tables, and 3x3 color-matrix application are now in core. `settings_theme_apply` and `renderer_gl.c::setcol` share matrix multiplication/clamping. Album-palette k-means and Offbeat's boosted four-color scene remain policy unless another product needs exactly that result. `core_rgb_luma` deliberately preserves the cover algorithm's weighted brightness; physical luminance requires linear RGB input, and this scalar helper does not perform transfer-function conversion.

## Layering and portability gaps

1. **Renderer interface is only partly backend-neutral.** `renderer.h` exposes `Core_GlLoadProc` in creation and accepts GLSL 4.6 strings for custom effects. `app.c::BACKGROUND_FX` is a GL program embedded in game. Texture handles expose a backend numeric ID, which is workable as an opaque token, but callers should not interpret it. Keep the GL implementation in core's backend; a future backend can provide a descriptor/capability interface and backend-specific effect modules. A shader-language abstraction is unnecessary until another backend is real, but the current limitations should be documented accurately.
2. **GL loader uses a process-global table.** `core_gl` is global, unlike renderer instance state. This is fine for the current single-context application; independently reusable renderers with different proc tables/context requirements would need instance loader state or an explicit single-backend contract.
3. **Native/application directory policy leaks.** App font fallback paths explicitly name `/usr/share/fonts/...`; `download.c::find_tool` enumerates `/usr/local/bin`, `/usr/bin`, `/opt/homebrew/bin`, `/snap/bin`, `$HOME/.local/bin`, and Nix paths. Move OS font/executable discovery behind platform callbacks while leaving the choice of font/tool in game. `main.c` formerly called POSIX `setenv` for mute; it now uses `platform_env_set`. ASCII comparisons through core remove the `strcasecmp/strncasecmp` portability dependency from player/download code.
4. **App-specific well-known directories.** The platform facade returns cache/config paths named for Offbeat, even though the platform layer is otherwise reusable. Passing an application ID to directory helpers or constructing application suffixes in game would make the division clearer. This is policy placement, not a runtime bug.
5. **Prefix convention is inconsistent.** The requested layer prefixes are not consistently used: most existing game public APIs are `app_`, `covers_`, `library_`, `player_`, `search_`, `settings_`, `ui_`, `ythumbs_`, `downloads_`, or `dlv_`; core math also uses `vec2_`, `mat4_`, etc. This makes function inventory/naming checks useful. Renaming the whole repository is a broad compatibility/style migration with little runtime value; do it separately if strict compliance is desired.
6. **Build selection is Linux-specific.** Automatic discovery adds all first-party `.c` files and links all Linux dependencies. It meets the automatic-file-discovery goal today, but a Windows backend added beside Linux files would require platform guards or filtered backend discovery. Keep the sub-second debug constraint when adding abstraction files; avoid complicated code generation or heavy library dependencies.

## Correctness and lifecycle concerns, separate from extraction

These should be considered independently of whether code moves into core. Source function names identify the evidence even if consolidation changes line numbers.

| Priority | Finding and evidence | Consequence / next step |
| --- | --- | --- |
| High | `platform_posix.c::platform_process_kill` writes ordinary `killed_at`, while `platform_process_read_line` reads and updates it from another thread; the API promises concurrent kill/read safety. | Synchronize the cancellation timestamp as well as process-handle lifetime; the fixed thumbnail `quit` flag does not resolve this separate process API race. |
| High | `player.c::player_vis_samples` reads ordinary float ring storage after loading atomic publication indices. `player_fill` writes that storage without the reader mutex. | Publication orders initial writes, but if the UI reader is delayed long enough for the writer to wrap, concurrent overwrite/read is possible. The reserved one-chunk safety margin assumes a bounded reader duration; it is not a general C synchronization proof. Specify/enforce an ownership protocol or use atomic sample/snapshot storage, and measure audio-thread impact. |
| Medium | `app.c::app_shutdown` tears down workers/subsystems but originally does not free library/retired snapshots, queue/likes/session allocations, or release frame/browse arenas. `main` does not destroy renderer/release root arena or recording pixels. `core/font` has no destroy/unmap counterpart; custom effects have no destroy API. | OS process exit currently reclaims resources. Repeated create/destroy or embedding the app/text/renderer would retain resources. Add complete teardown before promising reusable lifecycle; destroy GPU resources while the context still exists. |
| Medium | `platform.h` offers mutex/condition initialization but no destroy functions; subsystem destructors free storage containing initialized native primitives. | Current process teardown masks this on Linux, but reusable resource ownership is incomplete. Add matching teardown and failure reporting. |
| Medium | `renderer_gl.c::compile_shader/link_program` log failure but return shader/program handles; creation dereferences unchecked arena results. | Failed shaders can produce a nominally successful renderer/effect. Return explicit failures, unwind created resources, and check main/app initialization results. |
| Medium | `font.c::face_load` maps a file through a loader callback without storing size/mapping ownership or an unmap callback; failed fonts also lack cleanup. | Maps persist for process lifetime. Good lazy paging behavior should be preserved while adding ownership/destruction semantics. Font path buffers are fixed at 256 bytes and truncate silently. |
| Medium | Library cache serialization copies native structs, including pointer-shaped offset fields, and relies on layout/version/size guards. | Efficient for a local disposable cache, but not a portable file format across ABI/endian changes. If sharing the cache across architectures matters, use explicitly encoded integer records; do not add serialization machinery just for a local cache. |
| Medium | Native file replacement required covers' extra write mutex because the platform writer used a predictable per-process temporary name. | A supposedly reusable atomic-write facility should handle concurrent callers itself. Prefer exclusive unique temp creation and documented durability semantics; avoid teaching each subsystem this implementation detail. |
| Medium | `download.c::save_locked` allocates/copies/formats/writes while holding the subsystem mutex and originally does not check allocations. | Slow disk operations block workers/UI readers. Snapshot state/version under the mutex, serialize/write outside, then carefully reconcile version/ordering; preserve atomic persistence and shutdown semantics. |
| Low | `spectrum.c` uses process-global mutable initialization/layout, while the API accepts instance state. | Reentrancy and multi-rate concurrency are implicit constraints; instance plans resolve this if DSP extraction proceeds. |

The cover decode budget is an admission mechanism for concurrent decodes, not an absolute app-owned memory cap: its documented behavior allows one larger decode to run alone. Snapshot rebuilds can also overlap old/new libraries, scan records, strings, sorting/interning scratch, and output cache bytes. Track observed owned peaks on representative large libraries; neither a 16 MB decode budget nor virtual reservation size alone proves the entire app stays under 30 MB.

## What should stay specialized

- Music tags, genre-from-folder policy, song-title/artist cleanup, duplicate song identity, playback queue behavior, likes, and persistence schema belong in game.
- OS executable/font/home/music-directory discovery, process groups, native file/thread/audio/window operations belong in platform.
- Fixed-size copied worker jobs are intentional ownership isolation. Avoid replacing them with borrowed transient arena views.
- Typed narrow entries and bounded queues are intentional memory/performance decisions. A generic hash map/job system must earn its additional indirection/state.
- `Cov_Stream`, `Lib_Reader`, and the MP3 scanner solve different access patterns; unify common byte/file operations only when contracts remain clear.
- Float math is intentionally compact; the spectrum initializer's double-precision trigonometry and double constant are not a missed use of float `CORE_TAU`.
- Standard C library calls at formatting/codec boundaries are not automatically unwanted reimplementations. Prefer core where it adds ownership, capacity, UTF-8, or portability guarantees.

## Verification and follow-up order

Preserve the fast build and current domain tests while consolidating. Focused tests should cover the changed contracts: malformed/overlong UTF-8 and codepoint capacity, NUL termination/zero-capacity formatting, arena arithmetic/alignment, deterministic RNG state, grayscale/RGB/RGBA image crop/upscale/downscale, and stable sorting if extracted. Verify visual changes with the documented headless screenshot workflow using isolated cache/config directories. Repeated create/destroy, thread cancellation, and allocation failure need targeted tests if those lifecycle fixes are undertaken.

The standalone test inclusion workflow is reasonable at this scale, but add occasional normal separate-translation-unit builds so missing declarations/includes do not disappear through include order. Third-party optimized objects are cached independently; sanitizer modes instrument first-party code but the cached vendor objects are not necessarily sanitizer-instrumented. Preserve that distinction when interpreting test results.

Recommended next sequence after the selected core consolidations: (1) fix remaining thumbnail decode limits/startup failures and complete teardown; (2) share text editing, scroll/list geometry, and window controls; (3) extract indexed sort and string interning when there are concrete callers; (4) separate reusable DSP/audio/UI mechanisms from policy; (5) address alternate renderer/OS interfaces when an actual second backend is being implemented. Benchmark each proposed performance change on real library and thumbnail workloads before claiming speed improvements.

## Delivered inventory and observed checks

`tools/list_functions.py` lists first-party `.c`/`.h` definitions, including inline helpers, with source locations, signatures, linkage flags and layer-prefix checks. Run `python3 tools/list_functions.py` or add `--format json`, `--layer core`, `--check-prefix`, or `--include-third-party`. Usage and parser limitations are in `tools/LIST_FUNCTIONS.md`. The final first-party source inventory contains 809 definitions: 226 core, 466 game, 114 platform and 3 main. It identifies 592 existing prefix violations; a repository-wide rename was kept separate from utility consolidation. This is a lexical source inventory, including independently balanced conditional branches, not a list of symbols enabled in one compiled configuration.

Validation completed for debug, release, ASan/UBSan application builds, the 12 C test programs under sanitizers, and seven Python inventory tests. Real-library tests scanned 1,817 tracks and exercised cold/warm cover caches, artwork, scrolling, LRU churn, prefetch, decoding and seeking. New focused tests cover image filtering/channel layouts, allocator alignment/overflow/accounting, RNG sequences, string/UTF-8 failure contracts, settings truncation, and local thumbnail size/pixel admission limits. Headless screenshots used isolated `/tmp` cache/config directories and were visually inspected. Sanitized application screenshots used `ASAN_OPTIONS=detect_leaks=0` because complete app/font/renderer teardown is an existing remaining issue; they establish runtime memory-access checks, not leak-free lifetime. Cached vendor objects remain outside full sanitizer coverage.

The final warm debug build took 0.208 seconds; release took 8.712 seconds and the sanitizer application build took 17.697 seconds in this environment. Only the warm debug mode meets a literal sub-second build target in these observations; release/sanitizer compilation and uncached vendor compilation do not. New application `.c` files are still discovered automatically. GCC still reports existing download path/message format-truncation warnings; this audit did not establish behavior for arbitrarily long joined filesystem paths.
