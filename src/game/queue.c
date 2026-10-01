#include "queue.h"

b32 game_queue_remove_track(u32 *queue, u32 *original, u32 *count, s32 *current, u32 track) {
    u32 n = *count, w = 0, before = 0;
    b32 removed_current = *current >= 0 && (u32)*current < n && queue[*current] == track;
    for (u32 i = 0; i < n; i++) {
        if (queue[i] == track) continue;
        if ((s32)i < *current) before++;
        queue[w++] = queue[i];
    }
    u32 wo = 0;
    for (u32 i = 0; i < n; i++)
        if (original[i] != track) original[wo++] = original[i];
    *count = w;
    *current = *current < 0 || before >= w ? -1 : (s32)before;
    return removed_current;
}
