#include "settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const Theme SETTINGS_THEMES[] = {
    { "violet",  "Violet",    0.0f, 1.00f },
    { "ocean",   "Ocean",   -50.0f, 1.00f },
    { "aurora",  "Aurora", -105.0f, 0.95f },
    { "rose",    "Rose",     60.0f, 1.00f },
    { "ember",   "Ember",   125.0f, 1.05f },
    { "mono",    "Mono",      0.0f, 0.08f },
};
const u32 SETTINGS_THEME_COUNT = CORE_ARRAY_COUNT(SETTINGS_THEMES);

const char *const SETTINGS_VIS_IDS[VIS_COUNT]    = { "silk", "bars", "off" };
const char *const SETTINGS_VIS_LABELS[VIS_COUNT] = { "Silk", "Bars", "Off" };

void settings_default(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->theme = 0;
    s->vis = VIS_SILK;
    s->download_parallel = 2;
}

static u32 find_id(const char *v, u32 n, const char *(*get)(u32)) {
    for (u32 i = 0; i < n; i++)
        if (strcmp(v, get(i)) == 0) return i;
    return UINT32_MAX;
}
static const char *theme_id(u32 i) { return SETTINGS_THEMES[i].id; }
static const char *vis_id(u32 i) { return SETTINGS_VIS_IDS[i]; }

void settings_parse(Settings *s, Core_String text) {
    u64 at = 0;
    while (at < text.len) {
        u64 end = at;
        while (end < text.len && text.str[end] != '\n') end++;
        char line[1200];
        u64 n = CORE_MIN(end - at, (u64)sizeof(line) - 1);
        memcpy(line, text.str + at, n);
        line[n] = 0;
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
        at = end + 1;

        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        const char *key = line, *val = sp + 1;
        if (strcmp(key, "music_dir") == 0) {
            snprintf(s->music_dir, sizeof(s->music_dir), "%s", val);
        } else if (strcmp(key, "theme") == 0) {
            u32 i = find_id(val, SETTINGS_THEME_COUNT, theme_id);
            if (i != UINT32_MAX) s->theme = i;
        } else if (strcmp(key, "visualizer") == 0) {
            u32 i = find_id(val, VIS_COUNT, vis_id);
            if (i != UINT32_MAX) s->vis = i;
        } else if (strcmp(key, "debug") == 0) {
            s->debug = atoi(val) != 0;
        } else if (strcmp(key, "download_dir") == 0) {
            snprintf(s->download_dir, sizeof(s->download_dir), "%s", val);
        } else if (strcmp(key, "download_parallel") == 0) {
            s->download_parallel = CORE_CLAMP((u32)atoi(val), 1u, 3u);
        }
    }
}

u64 settings_format(const Settings *s, char *buf, u64 cap) {
    int n = snprintf(buf, cap, "offbeat-settings 1\nmusic_dir %s\ntheme %s\nvisualizer %s\ndebug %d\n"
                     "download_dir %s\ndownload_parallel %u\n",
                     s->music_dir, SETTINGS_THEMES[s->theme % SETTINGS_THEME_COUNT].id,
                     SETTINGS_VIS_IDS[s->vis % VIS_COUNT], s->debug ? 1 : 0,
                     s->download_dir, CORE_CLAMP(s->download_parallel, 1u, 3u));
    if (n < 0) return 0;
    return CORE_MIN((u64)n, cap ? cap - 1 : 0);
}

/* Luminance-preserving hue rotation followed by saturation (the SVG
   feColorMatrix formulas). Every row sums to 1, so grays stay gray. */
b32 settings_theme_matrix(u32 theme, f32 out[9]) {
    const Theme *t = &SETTINGS_THEMES[theme % SETTINGS_THEME_COUNT];
    f32 a = t->hue * (CORE_PI / 180.0f), c = cosf(a), sn = sinf(a);
    f32 h[9] = {
        0.213f + c * 0.787f - sn * 0.213f, 0.715f - c * 0.715f - sn * 0.715f, 0.072f - c * 0.072f + sn * 0.928f,
        0.213f - c * 0.213f + sn * 0.143f, 0.715f + c * 0.285f + sn * 0.140f, 0.072f - c * 0.072f - sn * 0.283f,
        0.213f - c * 0.213f - sn * 0.787f, 0.715f - c * 0.715f + sn * 0.715f, 0.072f + c * 0.928f + sn * 0.072f,
    };
    f32 k = t->saturation;
    f32 sat[9] = {
        0.213f + 0.787f * k, 0.715f - 0.715f * k, 0.072f - 0.072f * k,
        0.213f - 0.213f * k, 0.715f + 0.285f * k, 0.072f - 0.072f * k,
        0.213f - 0.213f * k, 0.715f - 0.715f * k, 0.072f + 0.928f * k,
    };
    for (u32 i = 0; i < 3; i++)
        for (u32 j = 0; j < 3; j++)
            out[i * 3 + j] = sat[i * 3 + 0] * h[0 * 3 + j] + sat[i * 3 + 1] * h[1 * 3 + j] + sat[i * 3 + 2] * h[2 * 3 + j];
    return t->hue != 0.0f || t->saturation != 1.0f;
}

vec3 settings_theme_apply(const f32 m[9], vec3 c) {
    vec3 o = {
        .x = m[0] * c.x + m[1] * c.y + m[2] * c.z,
        .y = m[3] * c.x + m[4] * c.y + m[5] * c.z,
        .z = m[6] * c.x + m[7] * c.y + m[8] * c.z,
    };
    o.x = CORE_CLAMP(o.x, 0.0f, 1.0f); o.y = CORE_CLAMP(o.y, 0.0f, 1.0f); o.z = CORE_CLAMP(o.z, 0.0f, 1.0f);
    return o;
}
