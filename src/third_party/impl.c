/*
 * Implementation unit for the vendored single-header libraries. Compiled once
 * with gcc -O2 by build.sh and cached (see build.sh), never by the fast debug
 * compiler. Their allocations are routed through the tracked heap so the app's
 * memory report stays exact.
 */

#include "core/memory.h"

#define STBI_MALLOC(sz)        core_heap_alloc(sz)
#define STBI_REALLOC(p, newsz) core_heap_realloc(p, newsz)
#define STBI_FREE(p)           core_heap_free(p)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#include "stb_image.h"

#define STBIW_MALLOC(sz)        core_heap_alloc(sz)
#define STBIW_REALLOC(p, newsz) core_heap_realloc(p, newsz)
#define STBIW_FREE(p)           core_heap_free(p)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STBTT_malloc(x, u) ((void)(u), core_heap_alloc(x))
#define STBTT_free(x, u)   ((void)(u), core_heap_free(x))
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define DRMP3_MALLOC(sz)        core_heap_alloc(sz)
#define DRMP3_REALLOC(p, sz)    core_heap_realloc(p, sz)
#define DRMP3_FREE(p)           core_heap_free(p)
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DRFLAC_MALLOC(sz)       core_heap_alloc(sz)
#define DRFLAC_REALLOC(p, sz)   core_heap_realloc(p, sz)
#define DRFLAC_FREE(p)          core_heap_free(p)
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

/* stb_vorbis has no allocator hooks; include the libc headers first so the
   macros below only rename its own calls. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#define malloc(sz)      core_heap_alloc(sz)
#define free(p)         core_heap_free(p)
#include "stb_vorbis.h"
#undef malloc
#undef free
