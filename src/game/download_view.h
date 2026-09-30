#ifndef GAME_DOWNLOAD_VIEW_H
#define GAME_DOWNLOAD_VIEW_H

#include "download.h"
#include "library.h"
#include "ui.h"

/*
 * The "Get music" page: its own full-window view on top of the player.
 *
 *   - a search bar with a Song / Artist switch. Songs list YouTube results to
 *     pick one from; artists list their top songs (up to 100) with checkboxes
 *     and "Select all", then download what is ticked;
 *   - a dock at the bottom that shows overall progress and expands on hover
 *     into the per-song list (phase, progress, retry, remove);
 *   - a missing yt-dlp / ffmpeg is explained right there instead of failing.
 *
 * Closing the page never stops anything: downloads run on the Downloads
 * threads and a small status pill (dlv_draw_status) keeps them visible on the
 * player screen. The page keeps its query, results and selection while hidden.
 */

typedef struct Dl_View Dl_View;

/* `cache_dir`: where downloaded thumbnails are kept. */
Dl_View *dlv_create(Downloads *d, Core_Renderer *r, const char *cache_dir);
void     dlv_destroy(Dl_View *v);

void dlv_open(Dl_View *v, b32 open);
b32  dlv_is_open(const Dl_View *v);
/* Open or still animating out: the player screen underneath stays inert. */
b32  dlv_visible(const Dl_View *v);
/* Open the page and run a search (kind: Dl_QueryKind). `select_all` ticks
   every artist result as it arrives ("download the top 100"). */
void dlv_search(Dl_View *v, u32 kind, const char *query, b32 select_all);
/* Open the page with an empty search bar in the given mode. */
void dlv_start_empty(Dl_View *v, u32 kind);

typedef struct {
    const Library *lib;        /* to mark songs you already have            */
    const char    *dest_dir;   /* the library: where songs are saved        */
    Platform_Window *win;      /* window buttons in the header              */
    b32            blocked;    /* something floats above: ignore input      */
} Dlv_Frame;

typedef struct {
    b32 open_settings;         /* the user clicked the folder chip          */
} Dlv_Out;

void dlv_draw(Dl_View *v, Ui *ui, const Dlv_Frame *f, Dlv_Out *out);

/* Status pill for the player screen (progress while downloading, a short
   "saved" message afterwards). `pos` is its top-left. Returns true when
   clicked (the caller opens the page). Does nothing while the page is open. */
b32  dlv_draw_status(Dl_View *v, Ui *ui, vec2 pos);

/* Demo / test hooks. */
void dlv_demo_download(Dl_View *v);   /* download what is ticked (or the highlighted song) */
void dlv_demo_dock(Dl_View *v, b32 open);

#endif /* GAME_DOWNLOAD_VIEW_H */
