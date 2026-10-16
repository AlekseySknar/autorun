#include "launcher_fixes.h"
#include "launcher.h"
#include "launcher_ui.h"
#include "game_fixes.h"
#include "horizon_dlls_curl.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum fixes_job { JOB_NONE, JOB_CHECK, JOB_DOWNLOAD };

struct launcher_fixes
{
    struct ui *ui;
    char root[512];
    SDL_Thread *thread;
    SDL_atomic_t cancel, done;
    int job;
    /* The launcher's: the card's index, or the published one once read. */
    struct game_fixes_index index;
    int have_index, online, newer;
    /* The worker's while it runs. */
    struct game_fixes_index fetched;
    int fetched_ok, fetched_newer;
    char download_id[32];
    struct game_fix download;
    enum horizon_dlls_result result;
};

static void trace( const char *format, ... ) __attribute__((format(printf, 1, 2)));
static void trace( const char *format, ... )
{
    char line[400];
    va_list args;

    va_start( args, format );
    vsnprintf( line, sizeof(line), format, args );
    va_end( args );
    wine_nx_runtime_trace( line );
}

static int cancelled( void *opaque, const char *what, unsigned long long current, unsigned long long total )
{
    struct launcher_fixes *f = opaque;

    (void)what; (void)current; (void)total;
    return SDL_AtomicGet( &f->cancel );
}

/* The published index, and for a download the fix's files as it lists them:
 * the copy the card has may be older than what the repository now serves. */
static int worker( void *opaque )
{
    struct launcher_fixes *f = opaque;
    struct horizon_dlls_transport transport = {0};
    SDL_Event event = {0};
    Uint32 started = SDL_GetTicks();

    f->fetched_ok = f->fetched_newer = 0;
    if (!horizon_dlls_curl_open( &transport, 1 )) f->result = HORIZON_DLLS_NETWORK;
    else
    {
        f->result = game_fixes_fetch( f->root, &transport, f->have_index ? f->index.serial : 0, &f->fetched,
                                      &f->fetched_newer, cancelled, f );
        f->fetched_ok = f->result == HORIZON_DLLS_OK;
        if (f->job == JOB_DOWNLOAD)
        {
            const struct game_fix *fix = f->fetched_ok ? game_fixes_find( &f->fetched, f->download_id ) : NULL;

            /* Offline, or the repository no longer has it: the card's own
             * index says what the files should be. */
            if (fix) f->download = *fix;
            f->result = game_fixes_download( f->root, &transport, &f->download, cancelled, f );
        }
        horizon_dlls_curl_close( &transport, 1 );
    }
    trace( "[FIXES] %s in %u ms: %s; the published index %s", f->job == JOB_DOWNLOAD ? "download" : "check",
           SDL_GetTicks() - started, horizon_dlls_error( f->result ),
           !f->fetched_ok ? "was not read" : f->fetched_newer ? "is newer, and kept" : "is the card's" );
    SDL_AtomicSet( &f->done, 1 );
    event.type = SDL_USEREVENT;
    SDL_PushEvent( &event );
    return 0;
}

static int start( struct launcher_fixes *f, int job )
{
    if (f->thread) return 0;
    f->job = job;
    SDL_AtomicSet( &f->cancel, 0 );
    SDL_AtomicSet( &f->done, 0 );
    f->thread = SDL_CreateThreadWithStackSize( worker, "game-fixes", 128 * 1024, f );
    if (!f->thread) { f->job = JOB_NONE; f->result = HORIZON_DLLS_MEMORY; return 0; }
    return 1;
}

/* Done with the worker, if it is done: the published index replaces the
 * card's when it was read. */
static int finish( struct launcher_fixes *f )
{
    if (!f->thread || !SDL_AtomicGet( &f->done )) return 0;
    SDL_WaitThread( f->thread, NULL );
    f->thread = NULL;
    f->job = JOB_NONE;
    if (f->fetched_ok)
    {
        game_fixes_free( &f->index );
        f->index = f->fetched;
        memset( &f->fetched, 0, sizeof(f->fetched) );
        f->have_index = f->online = 1;
        f->newer |= f->fetched_newer;
    }
    return 1;
}

/* A job with its progress over the screen. */
static enum horizon_dlls_result run( struct launcher_fixes *f, int job, const char *title, const char *status )
{
    if (job != JOB_NONE && !f->thread && !start( f, job )) return f->result;
    ui_progress_begin( f->ui );
    while (f->thread && f->ui->running && !finish( f ))
    {
        ui_progress_update( f->ui, title, status, 0, 0 );
        ui_wait( f->ui );
    }
    if (f->thread)  /* the launcher is closing */
    {
        SDL_AtomicSet( &f->cancel, 1 );
        SDL_WaitThread( f->thread, NULL );
        f->thread = NULL;
        f->job = JOB_NONE;
        game_fixes_free( &f->fetched );
    }
    ui_progress_end( f->ui );
    return f->result;
}

struct launcher_fixes *launcher_fixes_create( struct ui *ui, const char *root )
{
    struct launcher_fixes *f = calloc( 1, sizeof(*f) );

    if (!f) return NULL;
    if (strlen( root ) >= sizeof(f->root)) { free( f ); return NULL; }
    f->ui = ui;
    snprintf( f->root, sizeof(f->root), "%s", root );
    while (strlen( f->root ) > 1 && f->root[strlen( f->root ) - 1] == '/') f->root[strlen( f->root ) - 1] = 0;
    f->have_index = game_fixes_load( f->root, &f->index ) == HORIZON_DLLS_OK;
    trace( "[FIXES] the card's index: %s, %u fixes, serial %u%s", f->have_index ? "read" : "none",
           f->index.count, f->index.serial, f->index.skipped ? " (some need a newer Autorun)" : "" );
    start( f, JOB_CHECK );
    return f;
}

void launcher_fixes_destroy( struct launcher_fixes *f )
{
    if (!f) return;
    SDL_AtomicSet( &f->cancel, 1 );
    if (f->thread) SDL_WaitThread( f->thread, NULL );
    game_fixes_free( &f->fetched );
    game_fixes_free( &f->index );
    free( f );
}

int launcher_fixes_tick( struct launcher_fixes *f )
{
    int newer;

    if (!f) return 0;
    finish( f );
    newer = f->newer;
    f->newer = 0;
    return newer;
}

static const struct game_fix *match( struct launcher_fixes *f, const char *exe_path, enum game_fix_need *need )
{
    const struct game_fix *fix;
    struct game_fix_state state;

    if (!f || !f->have_index || !(fix = game_fixes_match( &f->index, exe_path ))) return NULL;
    game_fixes_state( f->root, exe_path, &state );
    *need = game_fixes_need( fix, &state );
    return fix;
}

const char *launcher_fixes_status( struct launcher_fixes *f, const char *exe_path, char *summary,
                                   size_t summary_size, char *buffer, size_t size, int *tone )
{
    enum game_fix_need need;
    const struct game_fix *fix = match( f, exe_path, &need );
    static const char *const words[] =
        { "Applied", "Not applied", "Update available", "A step failed", "Not asked" };

    if (!fix) return NULL;
    snprintf( summary, summary_size, "%s", fix->summary );
    *tone = need == GAME_FIX_APPLIED ? UI_VALUE_SUCCESS :
            need == GAME_FIX_SKIPPED || !fix->required ? UI_VALUE_NORMAL : UI_VALUE_DANGER;
    snprintf( buffer, size, "%s (version %u)", words[need], fix->version );
    return buffer;
}

int launcher_fixes_pending( struct launcher_fixes *f, const char *exe_path )
{
    enum game_fix_need need;

    return match( f, exe_path, &need ) && need != GAME_FIX_APPLIED && need != GAME_FIX_SKIPPED;
}

int launcher_fixes_forget( struct launcher_fixes *f, const char *exe_path )
{
    return f && game_fixes_forget( f->root, exe_path );
}

/* The files of the fix in f->download on the card, downloading what is
 * missing; the fix the download went by, which may be newer than the one
 * asked about, is then in f->download. */
static int fetch_files( struct launcher_fixes *f, const char *title )
{
    char text[640], heading[160];

    if (game_fixes_ready( f->root, &f->download )) return 1;
    /* A check still running is waited for: the download reads the index too. */
    if (f->thread) run( f, JOB_NONE, "Game fixes", "Checking the game fixes repository..." );
    snprintf( f->download_id, sizeof(f->download_id), "%s", f->download.id );
    snprintf( heading, sizeof(heading), "Fix for %s", title );
    if (run( f, JOB_DOWNLOAD, heading, "Downloading from the game fixes repository..." ) == HORIZON_DLLS_OK)
        return 1;
    if (!f->ui->running) return 0;
    snprintf( text, sizeof(text), "The fix could not be downloaded: %s\n\nConnect the console to the internet and "
              "start the game again to apply it.%s", horizon_dlls_error( f->result ),
              f->download.required ? " Without it the game may not start." : "" );
    return ui_confirm( f->ui, "Game fix unavailable", text, "Start anyway" ) ? -1 : 0;
}

enum launcher_fixes_start launcher_fixes_before_start( struct launcher_fixes *f, const char *title,
                                                       const char *exe_path, char *setup, size_t size )
{
    static const struct ui_hint hints[] =
        { { UI_A, "Apply fix" }, { UI_X, "Start without" }, { UI_Y, "Don't ask again" }, { UI_B, "Cancel" } };
    char text[1200], heading[160], why[256];
    const struct game_fix *fix;
    enum game_fix_need need;
    int answer, files, applied;

    if (!f) return LAUNCHER_FIXES_GAME;
    finish( f );
    if (!(fix = match( f, exe_path, &need )) || need == GAME_FIX_APPLIED || need == GAME_FIX_SKIPPED)
        return LAUNCHER_FIXES_GAME;
    /* The index is replaced when a check finishes, which it can between any
     * two frames of the question: from here on the fix is a copy. */
    f->download = *fix;
    fix = &f->download;

    snprintf( heading, sizeof(heading), "%s for %s", need == GAME_FIX_UPDATE ? "Game fix update" : "Game fix",
              title );
    snprintf( text, sizeof(text), "%s%s\n\n%s",
              need == GAME_FIX_UPDATE ? "A newer version of this game's fix is available. " :
              need == GAME_FIX_FAILED ? "The last time this fix ran, a step failed; autorun_runtime.log says which. " : "",
              fix->summary,
              fix->required ? "Without this fix the game may not start or work correctly." :
                              "Recommended for playing on the Switch." );
    if (!f->online && !game_fixes_ready( f->root, fix ))
        strncat( text, "\n\nApplying it downloads it from the game fixes repository, which needs an internet "
                 "connection.", sizeof(text) - strlen( text ) - 1 );
    answer = ui_ask( f->ui, heading, text, hints, 4 );
    if (answer == UI_X) return LAUNCHER_FIXES_GAME;
    if (answer == UI_Y)
    {
        if (!game_fixes_skip( f->root, exe_path, fix )) ui_toast( f->ui, "Could not save the game's settings", 2500 );
        return LAUNCHER_FIXES_GAME;
    }
    if (answer != UI_A) return LAUNCHER_FIXES_CANCEL;

    /* After this, f->download is what was downloaded: the published version,
     * if it is newer than the one asked about. */
    if (!(files = fetch_files( f, title ))) return LAUNCHER_FIXES_CANCEL;
    if (files < 0) return LAUNCHER_FIXES_GAME;
    applied = game_fixes_apply( f->root, exe_path, fix, setup, size, why, sizeof(why) );
    trace( "[FIXES] %s %u for %s: %s", fix->id, fix->version, exe_path,
           applied > 0 ? "its setup runs first" : applied == 0 ? "applied" : why );
    if (applied == 0)
    {
        ui_toast( f->ui, "Game fix applied", 1500 );
        return LAUNCHER_FIXES_APPLIED;
    }
    if (applied > 0) return LAUNCHER_FIXES_SETUP;
    snprintf( text, sizeof(text), "The fix could not be applied: %s%s", why,
              fix->required ? "\n\nWithout it the game may not start." : "" );
    return ui_confirm( f->ui, "Game fix", text, "Start anyway" ) ? LAUNCHER_FIXES_GAME : LAUNCHER_FIXES_CANCEL;
}
