#ifndef GAME_APP_H
#define GAME_APP_H

#include "../core/memory.h"
#include "../core/renderer.h"
#include "../platform/platform.h"

/*
 * Offbeat application: owns the library, cover art, audio engine and all UI
 * state. main.c only drives the loop: poll input -> app_frame -> present.
 */

typedef struct App App;

App *app_create(Core_Arena *arena, Core_Renderer *r, Platform_Window *win);
/* Update + draw one frame. Returns true while anything is moving (the main
   loop keeps presenting); false means the loop may sleep until input. */
b32  app_frame(App *app, const Platform_Input *in, f32 dt, u32 width, u32 height);
void app_shutdown(App *app);

#endif /* GAME_APP_H */
