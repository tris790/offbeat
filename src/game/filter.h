#ifndef GAME_FILTER_H
#define GAME_FILTER_H

#include "../core/string.h"

/* Compiled substring filter. Every word must occur in at least one field.
   Latin accents/case are folded; other Unicode codepoints stay distinct. */
typedef struct {
    u32 text[256];
    u16 fallback[256];
    u16 start[128], length[128];
    u32 word_count;
} Game_Filter;

void game_filter_compile(Game_Filter *filter, Core_String query);
b32 game_filter_matches(const Game_Filter *filter, const Core_String *fields, u32 field_count);

#endif
