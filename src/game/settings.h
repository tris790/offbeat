#ifndef GAME_SETTINGS_H
#define GAME_SETTINGS_H

#include "../core/math.h"
#include "../core/string.h"

/*
 * User settings: what the Settings modal edits, persisted as a small
 * "key value" text file in the config dir (unknown keys are ignored, missing
 * keys keep their defaults, so adding a setting never breaks old files).
 *
 * Themes are recolorings of the violet design: a hue rotation plus a
 * saturation factor, turned into a 3x3 color matrix that the renderer applies
 * to every shape and glyph (artwork is left untouched). Grays and white are
 * fixed points, so text stays neutral.
 */

typedef enum { VIS_SILK = 0, VIS_BARS, VIS_OFF, VIS_COUNT } Vis_Mode;

typedef struct {
    const char *id;       /* persisted name */
    const char *label;
    f32 hue;              /* degrees, rotation from the violet design */
    f32 saturation;       /* 1 = as designed */
} Theme;

extern const Theme SETTINGS_THEMES[];
extern const u32   SETTINGS_THEME_COUNT;
extern const char *const SETTINGS_VIS_IDS[VIS_COUNT];
extern const char *const SETTINGS_VIS_LABELS[VIS_COUNT];

typedef struct {
    char music_dir[1024]; /* empty = the platform's default music folder */
    u32  theme;           /* index into SETTINGS_THEMES */
    u32  vis;             /* Vis_Mode */
    b32  debug;           /* on-screen debug overlay */
    char download_dir[1024]; /* where downloaded songs go; empty = "Downloads" inside the music folder */
    u32  download_parallel;  /* songs downloaded at once, 1..3 */
} Settings;

void settings_default(Settings *s);
/* Apply every recognized line of `text` on top of `s`. */
void settings_parse(Settings *s, Core_String text);
/* Serialize into `buf`; returns bytes written (excluding the terminator). */
u64  settings_format(const Settings *s, char *buf, u64 cap);

/* Row-major color matrix for a theme; returns false for the identity (the
   design's own palette), in which case `out` is still filled. */
b32  settings_theme_matrix(u32 theme, f32 out[9]);
vec3 settings_theme_apply(const f32 m[9], vec3 c);

#endif /* GAME_SETTINGS_H */
