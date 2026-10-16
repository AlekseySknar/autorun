/* The game fixes repository's settings profiles, for host tests: its index
 * read as a card reads it (game_fixes.c) and offered to game_settings_find,
 * as the runtime does. GAME_FIXES_INDEX_PATH is the checkout's index, given by
 * the script that builds the test. */
#ifndef AUTORUN_TESTS_REPO_PROFILES_H
#define AUTORUN_TESTS_REPO_PROFILES_H

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_fixes.h"

static struct game_fixes_index repo_profiles_index;

static const struct game_settings_profile *repo_profiles_find( const char *target, uint16_t machine )
{
    const struct game_fixes_profile *found = game_fixes_match_profile( &repo_profiles_index, target, machine );

    return found ? &found->profile : NULL;
}

static void repo_profiles_load( void )
{
    static int loaded;
    FILE *file;
    char *text;
    long size;

    if (loaded) return;
    loaded = 1;
    assert( (file = fopen( GAME_FIXES_INDEX_PATH, "rb" )) );
    fseek( file, 0, SEEK_END );
    size = ftell( file );
    fseek( file, 0, SEEK_SET );
    assert( (text = malloc( size )) && fread( text, 1, size, file ) == (size_t)size );
    fclose( file );
    assert( game_fixes_parse( text, size, &repo_profiles_index ) == HORIZON_DLLS_OK );
    assert( !repo_profiles_index.profiles_skipped );
    free( text );
    game_settings_set_provider( repo_profiles_find );
}

/* The repository's profile by its id; it has to be there. */
static inline const struct game_settings_profile *repo_profile( const char *id )
{
    repo_profiles_load();
    for (unsigned int i = 0; i < repo_profiles_index.profile_count; i++)
        if (!strcmp( repo_profiles_index.profiles[i]->id, id )) return &repo_profiles_index.profiles[i]->profile;
    fprintf( stderr, "no profile %s in %s\n", id, GAME_FIXES_INDEX_PATH );
    abort();
}

#endif
