#ifndef GAME_QUEUE_H
#define GAME_QUEUE_H

#include "../core/types.h"

/* Remove every occurrence from both queue orders. Preserve the current row
   when it survives, otherwise select the next surviving row (or -1).
   Returns true when the current row was removed. */
b32 game_queue_remove_track(u32 *queue, u32 *original, u32 *count, s32 *current, u32 track);

#endif
