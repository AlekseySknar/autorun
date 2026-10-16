/*
 * Game fixes, from the game fixes repository (autorun-game-fixes).
 *
 * Some games need more than Autorun gives every program: registry keys their
 * installer would have written, a settings file their own launcher would have
 * made, controls that suit them. The repository publishes one fix per game,
 * laid out as on the card under switch/wine/game-fixes, with an index that
 * says which game each is for -- an executable's name and files that sit
 * beside it -- and what it does: a setup program to run before the game, files
 * to put in place, the game's own launcher settings.
 *
 * Every Autorun build ships the index it was made with as bundled.json, and
 * the launcher downloads the published one over it as index.json; whichever
 * has the higher serial is read. A card that is offline when a game starts
 * still knows that the game has a fix, and says it needs a network for it.
 *
 * What a game has had of its fix is kept in its own settings
 * (<game>.wine-nx.txt): game-fix=<id> <version> <exit code> once applied, and
 * game-fix-skip=<id> <version> when the player chose not to be asked again.
 *
 * A fix with a setup program runs it as the program the runtime starts, with
 * the game queued in run-next.txt behind it; running.txt says which game the
 * setup is for, so the runtime can write down how it went when it ends.
 *
 * Nothing here draws or knows about the network: the DLL manager's transport
 * fetches a URL, which is curl on the Switch and a folder in the host test.
 */
#ifndef WINE_NX_GAME_FIXES_H
#define WINE_NX_GAME_FIXES_H

#include <stddef.h>
#include <stdint.h>
#include "horizon_dlls.h"
#include "game_settings.h"

#define GAME_FIXES_RAW       "https://raw.githubusercontent.com/autorunhq/autorun-game-fixes/main/switch/wine/game-fixes/"
#define GAME_FIXES_INDEX_URL GAME_FIXES_RAW "index.json"
/* Relative to the runtime directory, as in the repository under switch/wine. */
#define GAME_FIXES_DIR       "game-fixes"
#define GAME_FIXES_INDEX     GAME_FIXES_DIR "/index.json"
#define GAME_FIXES_BUNDLED   GAME_FIXES_DIR "/bundled.json"
#define GAME_FIXES_RUNNING   GAME_FIXES_DIR "/running.txt"
#define GAME_FIXES_SCHEMA    1

/* What a fix may hold, as tools/build-fixes.py in the repository checks it. */
#define GAME_FIX_EXES     8
#define GAME_FIX_MARKS    8
#define GAME_FIX_FILES    16
#define GAME_FIX_INSTALLS 8
#define GAME_FIX_SETTINGS 16
#define GAME_FIX_VERSIONS 4
#define GAME_FIX_BUILDS   8
#define GAME_FIX_ROWS     32

struct game_fix_file
{
    char name[64];
    unsigned long long size;
    char sha256[65];
};

struct game_fix_install
{
    char file_name[64];
    unsigned int file;          /* index into files */
    int keep;                   /* a file already there is the player's */
    char to[256];               /* {game}/... or {drive_c}/..., with {stem} */
};

/* A string of the executable's version resource: ProductName, CompanyName,
 * OriginalFilename... compared without case, a * at the end matching any rest. */
struct game_fix_version
{
    char key[32];
    char value[96];
};

/* A value in the game's own settings file, written by Autorun's game settings
 * engine (game_settings.h) as a text row. */
struct game_fix_row
{
    char section[64];
    char key[64];
    char initial[64];
};

/* The game's own settings file: where it is, as an install's destination,
 * and what goes in it. */
struct game_fix_settings_file
{
    char path[256];
    int xml;
    char initial_text[4096];
    struct game_fix_row rows[GAME_FIX_ROWS];
    unsigned int row_count;
};

struct game_fix_setting
{
    char key[32];
    char value[64];
};

struct game_fix
{
    char id[32];
    char title[96];
    char summary[640];
    unsigned int version;
    int required;               /* the game may not start without it */
    /* What a game is recognised by: every kind given must match. exe lists
     * names, any of which will do, and may be left out when the version
     * strings say which game it is; timestamps (the PE header's link time)
     * and sizes each pin builds, any of which will do; marks are files that
     * must all be beside it. */
    char exes[GAME_FIX_EXES][64];
    unsigned int exe_count;
    struct game_fix_version versions[GAME_FIX_VERSIONS];
    unsigned int version_count;
    unsigned int timestamps[GAME_FIX_BUILDS];
    unsigned int timestamp_count;
    unsigned long long sizes[GAME_FIX_BUILDS];
    unsigned int size_count;
    char marks[GAME_FIX_MARKS][128];
    unsigned int mark_count;
    int run;                    /* the setup program, an index into files; -1 for none */
    struct game_fix_file files[GAME_FIX_FILES];
    unsigned int file_count;
    struct game_fix_install installs[GAME_FIX_INSTALLS];
    unsigned int install_count;
    struct game_fix_setting settings[GAME_FIX_SETTINGS];
    unsigned int setting_count;
    int has_game_settings;
    struct game_fix_settings_file game_settings;
};

/* A settings profile from the repository: what the launcher's game settings
 * screen shows, what the runtime writes for a game before it starts, and the
 * arguments it starts it with (game_settings.h). Unlike a fix it is not asked
 * about. Its id, version and the game it is for are
 * the repository's; profile is ready for game_settings_open, its strings and
 * arrays owned here. Each is allocated on its own, so profile stays where it is. */
#define GAME_PROFILE_ROWS (GAME_SETTINGS_MAX_ROWS - 1)
struct game_fixes_profile
{
    char id[32];
    unsigned int version;
    struct game_fix match;      /* only its exes, versions, builds and marks */
    struct game_settings_profile profile;
    struct game_setting rows[GAME_PROFILE_ROWS];
    const char *presets[4];
    const char *help[GAME_PROFILE_ROWS];
    struct game_argument add_arguments[16];
    void **owned;
    unsigned int owned_count, owned_capacity;
};

struct game_fixes_index
{
    int schema;
    unsigned int serial;
    struct game_fix *fixes;
    unsigned int count, capacity;
    struct game_fixes_profile **profiles;
    unsigned int profile_count, profile_capacity, profiles_skipped;
    /* Fixes this Autorun cannot read -- past its limits, or with something a
     * later build added -- which it leaves out rather than apply in part. */
    unsigned int skipped;
};

/* What a game has had of its fix. */
enum game_fix_need
{
    GAME_FIX_APPLIED,           /* this version, and it worked */
    GAME_FIX_NEW,               /* never applied */
    GAME_FIX_UPDATE,            /* an earlier version was */
    GAME_FIX_FAILED,            /* this version, but a step failed */
    GAME_FIX_SKIPPED,           /* the player said not to ask again */
};

struct game_fix_state
{
    char id[32];
    unsigned int applied;       /* the version applied, 0 for none */
    unsigned int exit_code;
    char skipped_id[32];
    unsigned int skipped;       /* the version not to ask about */
};

enum horizon_dlls_result game_fixes_parse( const char *text, size_t size, struct game_fixes_index *out );
/* The card's: the newer of the downloaded index and the build's own.
 * HORIZON_DLLS_NOT_FOUND when it has neither. */
enum horizon_dlls_result game_fixes_load( const char *root, struct game_fixes_index *out );
/* The published index. When its serial is above the one the card has, it is
 * kept on the card as index.json; *newer says whether it was. */
enum horizon_dlls_result game_fixes_fetch( const char *root, const struct horizon_dlls_transport *transport,
                                           unsigned int card_serial, struct game_fixes_index *out, int *newer,
                                           horizon_dlls_progress progress, void *opaque );
void game_fixes_free( struct game_fixes_index *index );

/* The fix for the game at exe_path (sdmc:/... or ums0:/...), or NULL. When
 * more than one matches, the one that asked more of the game wins: a fix for
 * one build over a fix for every build. */
const struct game_fix *game_fixes_match( const struct game_fixes_index *index, const char *exe_path );
const struct game_fix *game_fixes_find( const struct game_fixes_index *index, const char *id );
/* The repository's settings profile for the game, for the machine it is built for, or NULL. */
const struct game_fixes_profile *game_fixes_match_profile( const struct game_fixes_index *index, const char *exe_path,
                                                           uint16_t machine );
/* Offer the card's settings profiles to game_settings_find. The index is read once and kept for the life of the process,
 * so a profile handed out stays valid. */
void game_fixes_install_profiles( const char *root );

void game_fixes_state( const char *root, const char *exe_path, struct game_fix_state *state );
enum game_fix_need game_fixes_need( const struct game_fix *fix, const struct game_fix_state *state );

/* Whether the card holds every file of the fix, at its size and SHA-256. */
int game_fixes_ready( const char *root, const struct game_fix *fix );
/* Download the files the card does not have as they should be, each checked
 * before it is put in place. */
enum horizon_dlls_result game_fixes_download( const char *root, const struct horizon_dlls_transport *transport,
                                              const struct game_fix *fix, horizon_dlls_progress progress,
                                              void *opaque );

/* Remember that the player does not want to be asked about this version. */
int game_fixes_skip( const char *root, const char *exe_path, const struct game_fix *fix );
/* Forget what the game has had of any fix, so the next start asks again. */
int game_fixes_forget( const char *root, const char *exe_path );
/* Apply the fix to the game: its files put in place, its values into the
 * game's own settings file -- unless Autorun has settings of its own for that
 * game (game_settings_find), which are kept -- and its settings into the
 * game's. Returns 1 when its setup program is to run next, at setup (with its
 * arguments and running.txt written); 0 when the fix is applied and recorded;
 * -1 when it could not be, with why. The files must be ready. */
int game_fixes_apply( const char *root, const char *exe_path, const struct game_fix *fix,
                      char *setup, size_t setup_size, char *why, size_t why_size );
/* After a program ends: when it was a fix's setup, record how it went in the
 * game's settings and say so in line. Returns 1 when it was one. */
int game_fixes_finish( const char *root, const char *program, unsigned int exit_code, char *line, size_t size );

#endif
