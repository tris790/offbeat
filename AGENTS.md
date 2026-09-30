# Offbeat

C23 music player (Linux/Wayland/OpenGL first). Guidance for agents working in this repo.

## Goals
- simple performant OpenGL 4.6 rendering, behind an abstracted renderer interface so the backend can change later; self contained
- simple performant utf8 strings in C
- simple and performant SDF font drawing
- simple and performant shapes (rectangle, triangle, circle) and images
- simple and performant memory allocator with virtual memory support
- simple and performant input handling (keyboard, clicking, mouse movement, scroll wheel)

## Project structure
Each function in `src/<layer>/` is prefixed with its layer name: `core_`, `platform_`, `game_`.
- `src/core` -> code reusable on other projects (string, allocators, renderer, etc)
- `src/game` -> code specific to this application, works across all platforms
- `src/platform` -> ALL OS specific code goes here (linux wayland/alsa; windows unimplemented)
- `src/third_party` -> stb-style single header libraries
- `src/main.c` -> very simple file that only calls the different layers
- `tests/*.c` -> unit tests
- `tools/` -> demo recording scripts

## Build
- `./build.sh` debug (tcc), `./build.sh release` (gcc -O2), `./build.sh asan`, `./build.sh test`
- Must stay sub 1 sec even at 100k lines of code, and must not need editing when code is added (new `.c` files are picked up automatically).

## Constraints
- Memory budget "<30MB" is app-owned memory only (arenas, index, thumbnails, audio buffers), excluding the fixed GL driver cost.
- No heavy dependencies (ffmpeg, SDL, ...) without asking.

## Verifying visuals
Use headless screenshots rather than a live window (this implies mute; the /tmp dirs keep the user's real cache/state untouched):

    OFFBEAT_CACHE_DIR=/tmp/offbeat-cache OFFBEAT_CONFIG_DIR=/tmp/offbeat-config \
    OFFBEAT_SHOT=/tmp/x.png OFFBEAT_DEMO="find=bored;seek=60;tab=1;debug" ./build/offbeat
