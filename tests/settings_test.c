/*
 * Settings test: text round trip, tolerance of unknown/partial files, and
 * theme matrices keeping neutral colors neutral.
 */

#include "core/memory.c"
#include "core/string.c"
#include "game/settings.c"

#include <stdio.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static b32 near(f32 a, f32 b) { return fabsf(a - b) < 0.002f; }

int main(void) {
    Settings s, t;
    settings_default(&s);
    CHECK(s.vis == VIS_SILK && s.theme == 0 && !s.debug && s.music_dir[0] == 0);
    CHECK(s.download_dir[0] == 0 && s.download_parallel == 2);

    /* round trip */
    snprintf(s.music_dir, sizeof(s.music_dir), "/home/me/Music Folder/ünï");
    s.theme = 4; s.vis = VIS_BARS; s.debug = true;
    char buf[2048];
    u64 n = settings_format(&s, buf, sizeof(buf));
    settings_default(&t);
    settings_parse(&t, (Core_String){ .str = (u8 *)buf, .len = n });
    CHECK(strcmp(t.music_dir, s.music_dir) == 0);
    CHECK(t.theme == 4 && t.vis == VIS_BARS && t.debug);
    CHECK(t.download_dir[0] == 0 && t.download_parallel == 2);
    snprintf(s.download_dir, sizeof(s.download_dir), "/srv/dl folder/ünï");
    s.download_parallel = 3;
    n = settings_format(&s, buf, sizeof(buf));
    settings_default(&t);
    settings_parse(&t, (Core_String){ .str = (u8 *)buf, .len = n });
    CHECK(strcmp(t.download_dir, s.download_dir) == 0 && t.download_parallel == 3);
    settings_parse(&t, core_str("download_parallel 99\n"));
    CHECK(t.download_parallel == 3);
    settings_parse(&t, core_str("download_parallel 0\n"));
    CHECK(t.download_parallel == 1);

    /* unknown keys and values are ignored; missing keys keep defaults */
    settings_default(&t);
    settings_parse(&t, core_str("offbeat-settings 9\nfuture_key 12\ntheme nope\nvisualizer off\r\n"));
    CHECK(t.theme == 0 && t.vis == VIS_OFF && t.music_dir[0] == 0);
    settings_parse(&t, core_str("theme rose"));   /* no trailing newline */
    CHECK(strcmp(SETTINGS_THEMES[t.theme].id, "rose") == 0);

    /* Path fields use complete UTF-8 codepoints at the fixed buffer limit. */
    char long_line[1300];
    memcpy(long_line, "music_dir ", 10);
    memset(long_line + 10, 'a', 1022);
    memcpy(long_line + 1032, "é/end", 7);
    settings_parse(&t, core_str(long_line));
    CHECK(strlen(t.music_dir) == 1022);
    CHECK(core_utf8_valid(core_str(t.music_dir)));

    /* themes: identity for the design, grays are fixed points for all */
    f32 m[9];
    CHECK(!settings_theme_matrix(0, m));
    CHECK(near(m[0], 1) && near(m[4], 1) && near(m[8], 1) && near(m[1], 0));
    for (u32 i = 0; i < SETTINGS_THEME_COUNT; i++) {
        settings_theme_matrix(i, m);
        vec3 w = settings_theme_apply(m, vec3_make(1, 1, 1));
        vec3 g = settings_theme_apply(m, vec3_make(0.4f, 0.4f, 0.4f));
        CHECK(near(w.x, 1) && near(w.y, 1) && near(w.z, 1));
        CHECK(near(g.x, 0.4f) && near(g.y, 0.4f) && near(g.z, 0.4f));
    }
    /* ocean pulls the violet accent toward blue: less red */
    settings_theme_matrix(1, m);
    vec3 a = settings_theme_apply(m, vec3_make(139 / 255.0f, 92 / 255.0f, 246 / 255.0f));
    CHECK(a.x < 139 / 255.0f && a.z > a.x);

    if (g_fail) { printf("settings_test: %d failure(s)\n", g_fail); return 1; }
    printf("settings_test: ok\n");
    return 0;
}
