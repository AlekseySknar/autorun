/* The game settings profiles built into the runtime (source/games/), as the
 * game fixes repository carries them (source/game_fixes.c).
 *
 *     game_profiles export     every built-in profile as JSON, one array
 *     game_profiles check      each one through JSON and game_fixes_parse,
 *                              and back the same, field for field
 *
 * The check is what lets a profile move out of source/games into the repository
 * without a game noticing: what the repository's copy builds is what the
 * built-in one is. It covers every profile game_settings_builtin lists, so a
 * profile added there is covered as soon as it is. */
#include "../source/game_settings.c"
#include "game_fixes.h"

#include <stdarg.h>

#include <assert.h>

#define CHECK( cond ) do { if (!(cond)) { fprintf( stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond ); exit( 1 ); } } while (0)

struct text
{
    char *data;
    size_t size, capacity;
};

static void add( struct text *t, const char *format, ... ) __attribute__((format(printf, 2, 3)));
static void add( struct text *t, const char *format, ... )
{
    va_list args;
    int n;

    for (;;)
    {
        va_start( args, format );
        n = vsnprintf( t->data ? t->data + t->size : NULL, t->data ? t->capacity - t->size : 0, format, args );
        va_end( args );
        CHECK( n >= 0 );
        if (t->data && t->size + n < t->capacity) { t->size += n; return; }
        t->capacity = (t->capacity + n + 1) * 2;
        CHECK( (t->data = realloc( t->data, t->capacity )) );
    }
}

static void add_string( struct text *t, const char *s )
{
    if (!s) { add( t, "null" ); return; }
    add( t, "\"" );
    for (; *s; s++)
    {
        unsigned char c = *s;

        if (c == '"' || c == '\\') add( t, "\\%c", c );
        else if (c == '\n') add( t, "\\n" );
        else if (c == '\r') add( t, "\\r" );
        else if (c == '\t') add( t, "\\t" );
        else if (c < 0x20) add( t, "\\u%04x", c );
        else add( t, "%c", c );
    }
    add( t, "\"" );
}

/* The repository's name for a profile: its executable, lowercase, without
 * .exe and with dashes for anything else. */
static void profile_id( const struct game_settings_profile *profile, char *out, size_t size )
{
    const char *name = profile->executable;
    size_t n = 0, length = strlen( name );

    if (length > 4 && !strcasecmp( name + length - 4, ".exe" )) length -= 4;
    for (size_t i = 0; i < length && n + 1 < size; i++)
    {
        char c = (char)tolower( (unsigned char)name[i] );

        if (isalnum( (unsigned char)c )) out[n++] = c;
        else if (n && out[n - 1] != '-') out[n++] = '-';
    }
    while (n && out[n - 1] == '-') n--;
    out[n] = 0;
}

static const char *update_name( game_settings_update_fn update )
{
    if (!update) return NULL;
    CHECK( update == game_settings_update_hook( "ff7remake" ) );  /* every hook has a name */
    return "ff7remake";
}

static void export_profile( struct text *t, const struct game_settings_profile *profile, unsigned int version )
{
    static const char *const types[] = { "number", "integer", "text", "resolution", "d3d_api" };
    char id[32];

    profile_id( profile, id, sizeof(id) );
    add( t, "{\"id\": \"%s\", \"version\": %u, \"match\": {\"exe\": [", id, version );
    add_string( t, profile->executable );
    add( t, "]}, \"machine\": \"%s\", \"path\": ", profile->machine == 0x8664 ? "x64" : "x86" );
    {
        char path[300];

        snprintf( path, sizeof(path), "{drive_c}/%s", profile->path + strlen( "drive_c/" ) );
        CHECK( !strncmp( profile->path, "drive_c/", 8 ) );
        add_string( t, path );
    }
    add( t, ", \"format\": \"%s\"", profile->format == GAME_SETTINGS_XML ? "xml" : "ini" );
    if (profile->initial_text) { add( t, ", \"initial_text\": " ); add_string( t, profile->initial_text ); }
    if (profile->preset_count)
    {
        add( t, ", \"presets\": [" );
        for (unsigned int i = 0; i < profile->preset_count; i++)
        {
            if (i) add( t, ", " );
            add_string( t, profile->presets[i] );
        }
        add( t, "]" );
    }
    if (profile->pending_resolution[0] || profile->pending_resolution[1])
    {
        CHECK( profile->pending_resolution[0] && profile->pending_resolution[1] );
        add( t, ", \"pending_resolution\": [" );
        add_string( t, profile->pending_resolution[0] );
        add( t, ", " );
        add_string( t, profile->pending_resolution[1] );
        add( t, "]" );
    }
    if (profile->arguments) { add( t, ", \"arguments\": " ); add_string( t, profile->arguments ); }
    if (profile->update) { add( t, ", \"update\": " ); add_string( t, update_name( profile->update ) ); }
    add( t, ", \"rows\": [" );
    for (unsigned int i = 0; i < profile->count; i++)
    {
        const struct game_setting *row = profile->settings + i;

        add( t, "%s\n  {\"section\": ", i ? "," : "" );
        add_string( t, row->section );
        add( t, ", \"key\": " );
        add_string( t, row->key );
        if (row->key2) { add( t, ", \"key2\": " ); add_string( t, row->key2 ); }
        add( t, ", \"label\": " );
        add_string( t, row->label );
        add( t, ", \"type\": \"%s\"", types[row->type] );
        if (row->presets[0] || row->presets[1] || row->presets[2] || row->presets[3])
        {
            add( t, ", \"presets\": [" );
            for (unsigned int j = 0; j < 4; j++)
            {
                if (j) add( t, ", " );
                add_string( t, row->presets[j] );
            }
            add( t, "]" );
        }
        if (row->initial) { add( t, ", \"initial\": " ); add_string( t, row->initial ); }
        if (row->choice_count)
        {
            add( t, ", \"choices\": [" );
            for (unsigned int j = 0; j < row->choice_count; j++)
            {
                add( t, "%s{\"label\": ", j ? ", " : "" );
                add_string( t, row->choices[j].label );
                add( t, ", \"value\": " );
                add_string( t, row->choices[j].value );
                add( t, "}" );
            }
            add( t, "]" );
        }
        if (profile->help) { add( t, ", \"help\": " ); add_string( t, profile->help[i] ); }
        add( t, "}" );
    }
    add( t, "]}" );
}

static int same( const char *a, const char *b )
{
    return (!a && !b) || (a && b && !strcmp( a, b ));
}

/* What the repository's copy builds is the built-in profile, field for field. */
static void compare( const struct game_settings_profile *want, const struct game_settings_profile *got, const char *id )
{
    fprintf( stderr, "  %s: %u rows\n", id, want->count );
    CHECK( !strcasecmp( want->executable, got->executable ) );
    CHECK( want->machine == got->machine && same( want->path, got->path ) && want->format == got->format );
    CHECK( same( want->initial_text, got->initial_text ) && same( want->arguments, got->arguments ) );
    CHECK( want->update == got->update );
    CHECK( want->preset_count == got->preset_count );
    for (unsigned int i = 0; i < want->preset_count; i++) CHECK( same( want->presets[i], got->presets[i] ) );
    CHECK( same( want->pending_resolution[0], got->pending_resolution[0] ) &&
           same( want->pending_resolution[1], got->pending_resolution[1] ) );
    CHECK( !want->help == !got->help );
    CHECK( want->count == got->count );
    for (unsigned int i = 0; i < want->count; i++)
    {
        const struct game_setting *a = want->settings + i, *b = got->settings + i;

        CHECK( same( a->section, b->section ) && same( a->key, b->key ) && same( a->key2, b->key2 ) );
        CHECK( same( a->label, b->label ) && same( a->initial, b->initial ) && a->type == b->type );
        for (unsigned int j = 0; j < 4; j++) CHECK( same( a->presets[j], b->presets[j] ) );
        CHECK( a->choice_count == b->choice_count && !a->choices == !b->choices );
        for (unsigned int j = 0; j < a->choice_count; j++)
            CHECK( same( a->choices[j].label, b->choices[j].label ) && same( a->choices[j].value, b->choices[j].value ) );
        if (want->help) CHECK( same( want->help[i], got->help[i] ) );
    }
}

int main( int argc, char **argv )
{
    const unsigned int count = sizeof(game_settings_builtin) / sizeof(game_settings_builtin[0]);
    struct text t = {0};

    CHECK( argc == 2 && (!strcmp( argv[1], "export" ) || !strcmp( argv[1], "check" )) );
    add( &t, "{\"schema\": 1, \"serial\": 1, \"fixes\": [], \"profiles\": [" );
    for (unsigned int i = 0; i < count; i++)
    {
        add( &t, "%s\n", i ? "," : "" );
        export_profile( &t, game_settings_builtin[i], 1 );
    }
    add( &t, "]}\n" );
    if (!strcmp( argv[1], "export" ))
    {
        fputs( t.data, stdout );
        return 0;
    }

    {
        struct game_fixes_index index;
        char id[32];

        CHECK( game_fixes_parse( t.data, t.size, &index ) == HORIZON_DLLS_OK );
        CHECK( index.profile_count == count && !index.profiles_skipped );
        for (unsigned int i = 0; i < count; i++)
        {
            const struct game_settings_profile *want = game_settings_builtin[i];
            const struct game_fixes_profile *got;
            char exe[300];

            profile_id( want, id, sizeof(id) );
            CHECK( !strcmp( index.profiles[i]->id, id ) );
            compare( want, &index.profiles[i]->profile, id );
            /* And found for its game, for its machine only. */
            snprintf( exe, sizeof(exe), "sdmc:/nowhere/%s", want->executable );
            CHECK( (got = game_fixes_match_profile( &index, exe, want->machine )) == index.profiles[i] );
            CHECK( !game_fixes_match_profile( &index, exe, want->machine == 0x8664 ? 0x014c : 0x8664 ) );
        }
        game_fixes_free( &index );
    }
    free( t.data );
    printf( "game profiles: %u built-in profiles through JSON and back, field for field\n", count );
    return 0;
}
