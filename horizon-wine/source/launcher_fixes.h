#ifndef LAUNCHER_FIXES_H
#define LAUNCHER_FIXES_H

#include <stddef.h>

/* Game fixes in the launcher (game_fixes.h): the published index read in the
 * background when the launcher starts, and the question before a game with a
 * fix starts -- apply it, start without it, or do not ask again -- saying when
 * the game may not start without it. */
struct ui;
struct launcher_fixes;

enum launcher_fixes_start
{
    LAUNCHER_FIXES_CANCEL,      /* the player went back */
    LAUNCHER_FIXES_GAME,        /* start the game */
    LAUNCHER_FIXES_APPLIED,     /* start the game, whose settings the fix may have changed */
    LAUNCHER_FIXES_SETUP,       /* start the fix's setup, then the game */
};

/* Reads the card's index and starts checking the repository in the background. */
struct launcher_fixes *launcher_fixes_create( struct ui *ui, const char *runtime_dir );
/* 1 once, when the check brought an index newer than the card had. */
int  launcher_fixes_tick( struct launcher_fixes *fixes );
/* What the game has had of its fix, with what the fix does in summary; NULL
 * with no fix. tone is enum ui_value_tone. */
const char *launcher_fixes_status( struct launcher_fixes *fixes, const char *exe_path, char *summary,
                                   size_t summary_size, char *buffer, size_t size, int *tone );
/* Whether the game has a fix it has not had, and has not been told to skip. */
int  launcher_fixes_pending( struct launcher_fixes *fixes, const char *exe_path );
/* Forget what the game has had, so its next start asks again. */
int  launcher_fixes_forget( struct launcher_fixes *fixes, const char *exe_path );
/* Before a game starts: asks about its fix when it has one to apply, and
 * applies it. With LAUNCHER_FIXES_SETUP, setup is the program to start first. */
enum launcher_fixes_start launcher_fixes_before_start( struct launcher_fixes *fixes, const char *title,
                                                       const char *exe_path, char *setup, size_t size );
void launcher_fixes_destroy( struct launcher_fixes *fixes );

#endif
