#include "core/memory.c"
#include "game/queue.c"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void) {
    /* Duplicates before, at, and after current; original order differs. */
    u32 queue[] = {2, 1, 2, 3, 2, 4};
    u32 original[] = {1, 2, 2, 2, 4, 3};
    u32 n = 6;
    s32 current = 2;
    CHECK(game_queue_remove_track(queue, original, &n, &current, 2));
    CHECK(n == 3 && current == 1 && queue[current] == 3);
    u32 expected[] = {1, 3, 4}, expected_original[] = {1, 4, 3};
    CHECK(!memcmp(queue, expected, sizeof(expected)));
    CHECK(!memcmp(original, expected_original, sizeof(expected_original)));

    /* A surviving current row shifts left without changing songs. */
    CHECK(!game_queue_remove_track(queue, original, &n, &current, 1));
    CHECK(n == 2 && current == 0 && queue[current] == 3);
    CHECK(original[0] == 4 && original[1] == 3);
    CHECK(!game_queue_remove_track(queue, original, &n, &current, 99));
    CHECK(n == 2 && current == 0);

    /* Deleting the last current row stops instead of selecting a past row. */
    current = 1;
    CHECK(game_queue_remove_track(queue, original, &n, &current, 4));
    CHECK(n == 1 && current == -1 && queue[0] == 3 && original[0] == 3);
    CHECK(!game_queue_remove_track(queue, original, &n, &current, 3));
    CHECK(n == 0 && current == -1);
    CHECK(!game_queue_remove_track(0, 0, &n, &current, 3));

    u32 all[] = {7, 7}, all_original[] = {7, 7};
    n = 2;
    current = 0;
    CHECK(game_queue_remove_track(all, all_original, &n, &current, 7));
    CHECK(n == 0 && current == -1);

    printf(failures ? "queue_test: %d FAILED\n" : "queue_test: ok\n", failures);
    return failures != 0;
}
