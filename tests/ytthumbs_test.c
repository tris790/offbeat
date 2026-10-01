#include "core/memory.c"
#include "core/string.c"
#include "core/image.c"
#include "platform/platform_posix.c"
#include "game/ytthumbs.c"
#include "third_party/stb_image_write.h"

/* The cache's worker/lifetime code can run without a GL context. */
Core_Texture core_texture_create(Core_Renderer *r, u32 format, u32 w, u32 h,
                                 const void *pixels, b32 mipmaps) { return (Core_Texture){.id = 1}; }
void core_texture_destroy(Core_Renderer *r, Core_Texture tex) {}
void core_texture_update(Core_Renderer *r, Core_Texture tex, u32 x, u32 y, u32 w, u32 h,
                         const void *pixels) {}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "ytthumbs test: %s at %d\n", #x, __LINE__); return 1; } } while (0)
int main(void) {
    u64 before = core_mem_stats().heap_live;
    char dir[] = "/tmp/offbeat-ytthumbs-test-XXXXXX";
    CHECK(mkdtemp(dir));
    Ythumbs cache = {0};
    core_cstr_copy(cache.dir, sizeof(cache.dir), dir);
    core_image_color_tables_init(&cache.color_tables);
    char path[1200];
    snprintf(path, sizeof(path), "%s/normal.jpg", dir);
    u8 pixels[320 * 180 * 3];
    for (u32 p = 0; p < 320 * 180; p++) {
        pixels[p * 3] = 40; pixels[p * 3 + 1] = 50; pixels[p * 3 + 2] = 60;
    }
    CHECK(stbi_write_png(path, 320, 180, 3, pixels, 320 * 3));
    u8 *decoded = fetch_and_decode(&cache, "normal");
    CHECK(decoded);
    for (u32 p = 0; p < YT_SLOT * YT_SLOT; p++) {
        CHECK(abs((int)decoded[p * 4] - 40) <= 1);
        CHECK(abs((int)decoded[p * 4 + 1] - 50) <= 1);
        CHECK(abs((int)decoded[p * 4 + 2] - 60) <= 1);
        CHECK(decoded[p * 4 + 3] == 255);
    }
    core_heap_free(decoded);
    /* A small compressed file can still advertise an excessive pixel count. */
    u8 *large = core_heap_calloc(1025u * 1024u);
    CHECK(large);
    snprintf(path, sizeof(path), "%s/large.jpg", dir);
    CHECK(stbi_write_png(path, 1025, 1024, 1, large, 1025));
    core_heap_free(large);
    CHECK(!fetch_and_decode(&cache, "large"));
    /* File cap applies before allocating an arena or calling the decoder. */
    snprintf(path, sizeof(path), "%s/oversize.jpg", dir);
    FILE *file = fopen(path, "wb");
    CHECK(file);
    CHECK(ftruncate(fileno(file), YT_MAX_FILE_BYTES + 1) == 0);
    fclose(file);
    CHECK(!fetch_and_decode(&cache, "oversize"));
    Ythumbs *worker = ythumbs_create(0, dir);
    CHECK(worker);
    ythumbs_destroy(worker);
    ythumbs_update(0);
    CHECK(ythumbs_get(0, "normal", 0).missing);
    CHECK(core_mem_stats().heap_live == before);
    platform_remove_tree(dir);
    puts("ytthumbs_test: ok");
    return 0;
}
