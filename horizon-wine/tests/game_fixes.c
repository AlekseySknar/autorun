/* Game fixes from the game fixes repository (source/game_fixes.c), with a
 * folder standing in for GitHub.
 *
 *     game_fixes REPO SERVED VERSIONED BARE
 *
 * Run in an empty folder: the card is sdmc:/switch/wine under it, so the paths
 * are the card's own and the DOS paths a setup is told are what the runtime
 * would give it. REPO is the game fixes checkout's switch/wine/game-fixes;
 * SERVED is an empty folder whose files the test puts over REPO's; VERSIONED
 * is an executable with a version resource (ProductName "Wine ...",
 * OriginalFilename "regedit.exe") and BARE one without. */
#include "game_fixes.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK( cond ) do { if (!(cond)) { fprintf( stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond ); exit( 1 ); } } while (0)

#define ROOT "sdmc:/switch/wine"
#define DRIVE_C ROOT "/drive_c"

static const char *repo, *served;
static int fetches;

static enum horizon_dlls_result fetch( void *opaque, const char *url, horizon_dlls_sink sink, void *context,
                                       horizon_dlls_progress progress, void *progress_opaque )
{
    char path[1024], buffer[4096];
    struct stat st;
    size_t got;
    FILE *file;

    (void)opaque; (void)progress; (void)progress_opaque;
    fetches++;
    if (strncmp( url, GAME_FIXES_RAW, sizeof(GAME_FIXES_RAW) - 1 )) return HORIZON_DLLS_NOT_FOUND;
    snprintf( path, sizeof(path), "%s/%s", served, url + sizeof(GAME_FIXES_RAW) - 1 );
    if (stat( path, &st )) snprintf( path, sizeof(path), "%s/%s", repo, url + sizeof(GAME_FIXES_RAW) - 1 );
    if (!(file = fopen( path, "rb" ))) return HORIZON_DLLS_NOT_FOUND;
    while ((got = fread( buffer, 1, sizeof(buffer), file )) > 0)
        if (!sink( context, buffer, got )) { fclose( file ); return HORIZON_DLLS_IO; }
    fclose( file );
    return HORIZON_DLLS_OK;
}

static const struct horizon_dlls_transport transport = { fetch, NULL, NULL, NULL };

static char *slurp( const char *path, size_t *size )
{
    FILE *file = fopen( path, "rb" );
    char *data;
    long length;

    if (!file) return NULL;
    fseek( file, 0, SEEK_END );
    length = ftell( file );
    fseek( file, 0, SEEK_SET );
    data = malloc( length + 1 );
    CHECK( data && fread( data, 1, length, file ) == (size_t)length );
    data[length] = 0;
    fclose( file );
    if (size) *size = length;
    return data;
}

static void put( const char *path, const char *text )
{
    char partial[1024];
    size_t i;
    FILE *file;

    snprintf( partial, sizeof(partial), "%s", path );
    for (i = 1; partial[i]; i++)
        if (partial[i] == '/') { partial[i] = 0; mkdir( partial, 0777 ); partial[i] = '/'; }
    CHECK( (file = fopen( path, "wb" )) );
    fputs( text, file );
    fclose( file );
}

static void copy( const char *from, const char *to )
{
    size_t size;
    char *data = slurp( from, &size );
    FILE *file;

    CHECK( data );
    put( to, "" );
    CHECK( (file = fopen( to, "wb" )) && fwrite( data, 1, size, file ) == size );
    fclose( file );
    free( data );
}

/* The PE header's link time, read by hand rather than by what is tested. */
static unsigned int link_time( const char *path )
{
    unsigned char *data = (unsigned char *)slurp( path, NULL );
    unsigned int nt, time;

    CHECK( data );
    nt = data[0x3c] | data[0x3d] << 8 | data[0x3e] << 16 | (unsigned int)data[0x3f] << 24;
    time = data[nt + 8] | data[nt + 9] << 8 | data[nt + 10] << 16 | (unsigned int)data[nt + 11] << 24;
    free( data );
    return time;
}

static void folder( const char *path )
{
    char marker[1024];

    snprintf( marker, sizeof(marker), "%s/.keep", path );
    put( marker, "" );
}

/* The index with what replace says in place of what find says, at serial. */
static void serve_index( const char *find, const char *replace, unsigned int serial )
{
    char path[1024], *text, *at, *out, *serial_at;
    size_t size;

    snprintf( path, sizeof(path), "%s/index.json", repo );
    CHECK( (text = slurp( path, &size )) );
    CHECK( (out = malloc( size + 4096 )) );
    out[0] = 0;
    if (find && (at = strstr( text, find )))
    {
        strncat( out, text, at - text );
        strcat( out, replace );
        strcat( out, at + strlen( find ) );
    }
    else strcpy( out, text );
    CHECK( (serial_at = strstr( out, "\"serial\": " )) );
    {
        char *end = serial_at + strlen( "\"serial\": " ), *rest;

        while (*end >= '0' && *end <= '9') end++;
        CHECK( (rest = strdup( end )) );
        sprintf( serial_at, "\"serial\": %u%s", serial, rest );
        free( rest );
    }
    snprintf( path, sizeof(path), "%s/index.json", served );
    put( path, out );
    free( text );
    free( out );
}

static const struct game_fixes_index *later_profiles;

static const struct game_settings_profile *later_profile( const char *target, uint16_t machine )
{
    const struct game_fixes_profile *found = game_fixes_match_profile( later_profiles, target, machine );

    return found ? &found->profile : NULL;
}

static enum game_fix_need need_of( const struct game_fixes_index *index, const char *exe )
{
    struct game_fix_state state;
    const struct game_fix *fix = game_fixes_match( index, exe );

    CHECK( fix );
    game_fixes_state( ROOT, exe, &state );
    return game_fixes_need( fix, &state );
}

int main( int argc, char **argv )
{
    static const char war3[] = DRIVE_C "/WarCraft III/war3.exe";
    static const char sims2[] = DRIVE_C "/Sims 2 UC/EP9/TSBin/Sims2EP9.exe";
    static const char halo[] = DRIVE_C "/Halo/HALO.EXE";
    struct game_fixes_index index, fetched;
    const struct game_fix *fix;
    char path[1024], setup[640], why[256], line[512], war3_args[1024], *text;
    unsigned int war3_version;
    unsigned int serial;
    int newer;

    CHECK( argc == 5 );
    repo = argv[1];
    served = argv[2];

    /* A card with no index knows of no fixes. */
    CHECK( game_fixes_load( ROOT, &index ) == HORIZON_DLLS_NOT_FOUND );

    /* The build's own index, as the archive ships it. */
    snprintf( path, sizeof(path), "%s/index.json", repo );
    CHECK( (text = slurp( path, NULL )) );
    put( ROOT "/" GAME_FIXES_BUNDLED, text );
    free( text );
    CHECK( game_fixes_load( ROOT, &index ) == HORIZON_DLLS_OK );
    /* Its settings profiles are every game's: offered to game_settings_find
     * from the start, as the runtime does. */
    game_fixes_install_profiles( ROOT );
    CHECK( game_settings_find( DRIVE_C "/FNV/FalloutNV.exe", 0x014c ) );
    CHECK( !strcmp( game_settings_find( DRIVE_C "/FNV/FalloutNV.exe", 0x014c )->path,
                    "drive_c/users/steamuser/Documents/My Games/FalloutNV/FalloutPrefs.ini" ) );
    CHECK( index.count >= 4 && !index.skipped );
    serial = index.serial;

    /* The games, and one that only shares an executable's name. */
    put( war3, "MZ" );
    put( DRIVE_C "/WarCraft III/Game.dll", "MZ" );
    put( sims2, "MZ" );
    folder( DRIVE_C "/Sims 2 UC/EP9/TSData" );
    put( halo, "MZ" );
    folder( DRIVE_C "/Halo/Controls" );
    put( DRIVE_C "/Other/war3.exe", "MZ" );
    CHECK( (fix = game_fixes_match( &index, war3 )) && !strcmp( fix->id, "war3" ) && fix->required );
    CHECK( (fix = game_fixes_match( &index, sims2 )) && !strcmp( fix->id, "sims2" ) );
    CHECK( (fix = game_fixes_match( &index, halo )) && !strcmp( fix->id, "halo" ) );
    CHECK( !game_fixes_match( &index, DRIVE_C "/Other/war3.exe" ) );
    CHECK( !game_fixes_match( &index, DRIVE_C "/WarCraft III/Game.dll" ) );
    CHECK( need_of( &index, war3 ) == GAME_FIX_NEW );

    /* The published index is the same: nothing is written over the card's. */
    CHECK( game_fixes_fetch( ROOT, &transport, serial, &fetched, &newer, NULL, NULL ) == HORIZON_DLLS_OK );
    CHECK( !newer && fetched.count == index.count );
    game_fixes_free( &fetched );
    CHECK( access( ROOT "/" GAME_FIXES_INDEX, F_OK ) );

    /* WarCraft III: downloaded, then its setup made ready to run. */
    fix = game_fixes_find( &index, "war3" );
    CHECK( fix && fix->run >= 0 && !game_fixes_ready( ROOT, fix ) );
    CHECK( game_fixes_download( ROOT, &transport, fix, NULL, NULL ) == HORIZON_DLLS_OK );
    CHECK( game_fixes_ready( ROOT, fix ) );
    fetches = 0;
    CHECK( game_fixes_download( ROOT, &transport, fix, NULL, NULL ) == HORIZON_DLLS_OK && !fetches );
    CHECK( game_fixes_apply( ROOT, war3, fix, setup, sizeof(setup), why, sizeof(why) ) == 1 );
    /* Its setup and version are the repository's: read, not assumed. */
    war3_version = fix->version;
    snprintf( path, sizeof(path), ROOT "/game-fixes/war3/%s", fix->files[fix->run].name );
    CHECK( !strcmp( setup, path ) );
    snprintf( war3_args, sizeof(war3_args), "%.*s.args.txt", (int)(strlen( path ) - 4), path );
    CHECK( (text = slurp( war3_args, NULL )) );
    CHECK( !strcmp( text, "\"C:\\WarCraft III\\war3.exe\"\n" ) );
    free( text );
    /* Not applied until the setup has ended. */
    CHECK( need_of( &index, war3 ) == GAME_FIX_NEW );
    CHECK( !game_fixes_finish( ROOT, DRIVE_C "/windows/autorun-setup.exe", 0, line, sizeof(line) ) );
    CHECK( game_fixes_finish( ROOT, setup, 0, line, sizeof(line) ) );
    snprintf( path, sizeof(path), "war3 %u for war3.exe applied", war3_version );
    CHECK( strstr( line, path ) );
    CHECK( access( ROOT "/" GAME_FIXES_RUNNING, F_OK ) && access( war3_args, F_OK ) );
    CHECK( need_of( &index, war3 ) == GAME_FIX_APPLIED );
    CHECK( !game_fixes_finish( ROOT, setup, 0, line, sizeof(line) ) );
    snprintf( path, sizeof(path), "game-fix=war3 %u 0\n", war3_version );
    CHECK( (text = slurp( DRIVE_C "/WarCraft III/war3.wine-nx.txt", NULL )) && strstr( text, path ) );
    free( text );

    /* A setup that reports a failed step is offered again. */
    CHECK( game_fixes_apply( ROOT, war3, fix, setup, sizeof(setup), why, sizeof(why) ) == 1 );
    CHECK( game_fixes_finish( ROOT, setup, 1, line, sizeof(line) ) && strstr( line, "had a step fail" ) );
    CHECK( need_of( &index, war3 ) == GAME_FIX_FAILED );

    /* Halo: a file put beside the game, named after it, and no setup. The
     * player's own controls are kept. */
    fix = game_fixes_find( &index, "halo" );
    CHECK( fix && fix->run < 0 );
    CHECK( game_fixes_download( ROOT, &transport, fix, NULL, NULL ) == HORIZON_DLLS_OK );
    CHECK( game_fixes_apply( ROOT, halo, fix, setup, sizeof(setup), why, sizeof(why) ) == 0 );
    CHECK( (text = slurp( DRIVE_C "/Halo/HALO.keys.txt", NULL )) && strstr( text, "LUP=0x57" ) );
    free( text );
    CHECK( need_of( &index, halo ) == GAME_FIX_APPLIED );
    put( DRIVE_C "/Halo/HALO.keys.txt", "X=0x20\n" );
    CHECK( game_fixes_apply( ROOT, halo, fix, setup, sizeof(setup), why, sizeof(why) ) == 0 );
    CHECK( (text = slurp( DRIVE_C "/Halo/HALO.keys.txt", NULL )) && !strcmp( text, "X=0x20\n" ) );
    free( text );

    /* A file into drive_c, folders and all, from a fix only this test serves. */
    {
        static const char prefs[] = "[Display]\r\niSize W=1280\r\n";
        struct game_fixes_index own;
        char json[1024], hash[65];

        snprintf( path, sizeof(path), "%s/prefs/Prefs.ini", served );
        put( path, prefs );
        horizon_dlls_sha256( prefs, strlen( prefs ), hash );
        snprintf( json, sizeof(json),
            "{\"schema\": 1, \"serial\": 1, \"fixes\": [{\"id\": \"prefs\", \"title\": \"P\", \"version\": 1,"
            " \"summary\": \"s\", \"match\": {\"exe\": [\"Prefs.exe\"]},"
            " \"files\": [{\"name\": \"Prefs.ini\", \"size\": %zu, \"sha256\": \"%s\"}],"
            " \"install\": [{\"file\": \"Prefs.ini\", \"to\": \"{drive_c}/users/steamuser/Documents/My Games/P/Prefs.ini\","
            " \"keep\": true}]}]}", strlen( prefs ), hash );
        CHECK( game_fixes_parse( json, strlen( json ), &own ) == HORIZON_DLLS_OK && own.count == 1 );
        put( DRIVE_C "/P/Prefs.exe", "MZ" );
        CHECK( (fix = game_fixes_match( &own, DRIVE_C "/P/Prefs.exe" )) );
        CHECK( game_fixes_download( ROOT, &transport, fix, NULL, NULL ) == HORIZON_DLLS_OK );
        CHECK( game_fixes_apply( ROOT, DRIVE_C "/P/Prefs.exe", fix, setup, sizeof(setup), why, sizeof(why) ) == 0 );
        CHECK( (text = slurp( DRIVE_C "/users/steamuser/Documents/My Games/P/Prefs.ini", NULL )) );
        CHECK( !strcmp( text, prefs ) );
        free( text );
        remove( path );
        game_fixes_free( &own );
    }

    /* The repository raises WarCraft III's version and adds settings: the
     * card keeps the newer index, and the game is offered the update. */
    snprintf( path, sizeof(path), "\"version\": %u,\n   \"required\": true,\n   \"summary\": \"Sets the game's video",
              war3_version );
    snprintf( line, sizeof(line), "\"version\": %u,\n   \"required\": true,\n   \"summary\": \"Sets the game's video",
              war3_version + 1 );
    serve_index( path, line, serial + 1 );
    CHECK( game_fixes_fetch( ROOT, &transport, serial, &fetched, &newer, NULL, NULL ) == HORIZON_DLLS_OK && newer );
    game_fixes_free( &fetched );
    game_fixes_free( &index );
    CHECK( game_fixes_load( ROOT, &index ) == HORIZON_DLLS_OK && index.serial == serial + 1 );
    CHECK( game_fixes_find( &index, "war3" )->version == war3_version + 1 );
    CHECK( need_of( &index, war3 ) == GAME_FIX_UPDATE );
    /* Not asked again about this version, but about the next. */
    CHECK( game_fixes_skip( ROOT, war3, game_fixes_find( &index, "war3" ) ) );
    CHECK( need_of( &index, war3 ) == GAME_FIX_SKIPPED );
    game_fixes_free( &index );
    snprintf( line, sizeof(line), "\"version\": %u,\n   \"required\": true,\n   \"summary\": \"Sets the game's video",
              war3_version + 2 );
    serve_index( path, line, serial + 2 );
    CHECK( game_fixes_fetch( ROOT, &transport, serial + 1, &index, &newer, NULL, NULL ) == HORIZON_DLLS_OK && newer );
    CHECK( need_of( &index, war3 ) == GAME_FIX_UPDATE );

    /* A build that ships an older index than the card downloaded keeps the card's. */
    snprintf( path, sizeof(path), "%s/index.json", repo );
    CHECK( (text = slurp( path, NULL )) );
    put( ROOT "/" GAME_FIXES_BUNDLED, text );
    free( text );
    game_fixes_free( &index );
    CHECK( game_fixes_load( ROOT, &index ) == HORIZON_DLLS_OK && index.serial == serial + 2 );

    /* A file that is not what the index says is refused, and the card keeps
     * the one it had. */
    fix = game_fixes_find( &index, "sims2" );
    snprintf( path, sizeof(path), "%s/sims2/dxvk.conf", served );
    put( path, "d3d9.maxAvailableMemory = 99999\n" );
    CHECK( game_fixes_download( ROOT, &transport, fix, NULL, NULL ) == HORIZON_DLLS_HASH );
    CHECK( access( ROOT "/game-fixes/sims2/dxvk.conf", F_OK ) && !game_fixes_ready( ROOT, fix ) );
    remove( path );
    CHECK( game_fixes_download( ROOT, &transport, fix, NULL, NULL ) == HORIZON_DLLS_OK && game_fixes_ready( ROOT, fix ) );
    CHECK( game_fixes_apply( ROOT, sims2, fix, setup, sizeof(setup), why, sizeof(why) ) == 1 );
    CHECK( (text = slurp( ROOT "/game-fixes/sims2/sims2-setup.args.txt", NULL )) );
    CHECK( !strcmp( text, "\"C:\\Sims 2 UC\\EP9\\TSBin\\Sims2EP9.exe\"\n" ) );
    free( text );
    game_fixes_free( &index );

    /* What this build cannot apply whole is left out, not applied in part;
     * JSON that cannot be read is no index at all. */
    {
        static const char later[] =
            "{\"schema\": 1, \"serial\": 9, \"fixes\": ["
            "{\"id\": \"a\", \"title\": \"A\", \"version\": 1, \"summary\": \"s\", \"match\": {\"exe\": [\"a.exe\"]},"
            " \"settings\": {\"d3d\": \"wine\"}, \"registry\": []},"
            "{\"id\": \"b\", \"title\": \"B\", \"version\": 1, \"summary\": \"s\", \"match\": {\"exe\": [\"b.exe\"]},"
            " \"files\": [{\"name\": \"x.txt\", \"size\": 1, \"sha256\": \""
            "0000000000000000000000000000000000000000000000000000000000000000\"}],"
            " \"install\": [{\"file\": \"x.txt\", \"to\": \"{game}/../x.txt\"}]},"
            "{\"id\": \"c\", \"title\": \"C\", \"version\": 1, \"summary\": \"s\", \"match\": {\"exe\": [\"c.exe\"]},"
            " \"settings\": {\"cpu\": \"box64\"}}]}";

        CHECK( game_fixes_parse( later, sizeof(later) - 1, &index ) == HORIZON_DLLS_OK );
        CHECK( index.count == 1 && index.skipped == 2 && !strcmp( index.fixes[0].id, "c" ) );
        game_fixes_free( &index );
        CHECK( game_fixes_parse( later, sizeof(later) - 3, &index ) == HORIZON_DLLS_INVALID );
        CHECK( game_fixes_parse( "{\"schema\": 2, \"serial\": 1, \"fixes\": []}", 39, &index ) == HORIZON_DLLS_INVALID );
    }

    /* Told by what the executable says of itself: a renamed one is still the
     * game, a build is told by its link time or size, and the fix that asks
     * more of the game wins over one that asks less. */
    {
        static const char renamed[] = DRIVE_C "/Tools/renamed.exe", bare[] = DRIVE_C "/Tools/bare.exe";
        unsigned int time = link_time( argv[3] );
        char json[4096];
        size_t size;

        copy( argv[3], renamed );
        copy( argv[4], bare );
        free( slurp( renamed, &size ) );
        snprintf( json, sizeof(json),
            "{\"schema\": 1, \"serial\": 1, \"fixes\": ["
            "{\"id\": \"by-name\", \"title\": \"N\", \"version\": 1, \"summary\": \"s\","
            " \"match\": {\"exe\": [\"renamed.exe\", \"bare.exe\"]}, \"settings\": {\"d3d\": \"wine\"}},"
            "{\"id\": \"by-product\", \"title\": \"P\", \"version\": 1, \"summary\": \"s\","
            " \"match\": {\"version\": {\"productname\": \"WINE*\", \"OriginalFilename\": \"regedit.exe\"}},"
            " \"settings\": {\"d3d\": \"wine\"}},"
            "{\"id\": \"other-build\", \"title\": \"O\", \"version\": 1, \"summary\": \"s\","
            " \"match\": {\"version\": {\"OriginalFilename\": \"regedit.exe\"}, \"timestamp\": [%u]},"
            " \"settings\": {\"d3d\": \"wine\"}},"
            "{\"id\": \"this-build\", \"title\": \"T\", \"version\": 1, \"summary\": \"s\","
            " \"match\": {\"exe\": [\"renamed.exe\"], \"timestamp\": [1, %u], \"size\": [%zu]},"
            " \"settings\": {\"d3d\": \"wine\"}},"
            "{\"id\": \"files-only\", \"title\": \"F\", \"version\": 1, \"summary\": \"s\","
            " \"match\": {\"files\": [\"bare.exe\"]}, \"settings\": {\"d3d\": \"wine\"}}]}",
            time + 1, time, size );
        CHECK( game_fixes_parse( json, strlen( json ), &index ) == HORIZON_DLLS_OK );
        /* A fix with nothing to tell its game by but files beside it is left out. */
        CHECK( index.count == 4 && index.skipped == 1 );
        CHECK( (fix = game_fixes_match( &index, renamed )) && !strcmp( fix->id, "this-build" ) );
        index.fixes[3].sizes[0]++;      /* another build of it */
        CHECK( (fix = game_fixes_match( &index, renamed )) && !strcmp( fix->id, "by-product" ) );
        index.fixes[1].versions[0].value[0] = 'X';
        CHECK( (fix = game_fixes_match( &index, renamed )) && !strcmp( fix->id, "by-name" ) );
        /* No version resource at all: only what asks nothing of one. */
        CHECK( (fix = game_fixes_match( &index, bare )) && !strcmp( fix->id, "by-name" ) );
        index.fixes[0].exes[1][0] = 'x';
        CHECK( !game_fixes_match( &index, bare ) );
        game_fixes_free( &index );
    }

    /* The game's own settings file, through Autorun's game settings engine:
     * made from initial_text when it is not there, every row written on every
     * apply, and left alone for a game the engine has settings of its own for. */
    {
        static const char json[] =
            "{\"schema\": 1, \"serial\": 1, \"fixes\": ["
            "{\"id\": \"ini\", \"title\": \"I\", \"version\": 1, \"summary\": \"s\","
            " \"match\": {\"exe\": [\"Game.exe\", \"FalloutNV.exe\"]},"
            " \"game_settings\": {\"path\": \"{game}/Config/{stem}.ini\", \"format\": \"ini\","
            " \"initial_text\": \"[Video]\\r\\nWidth=640\\r\\nGamma=2\\r\\n\","
            " \"rows\": [{\"section\": \"Video\", \"key\": \"Width\", \"initial\": \"1280\"},"
            "          {\"section\": \"Video\", \"key\": \"Height\", \"initial\": \"720\"}]}},"
            "{\"id\": \"yaml\", \"title\": \"Y\", \"version\": 1, \"summary\": \"s\", \"match\": {\"exe\": [\"y.exe\"]},"
            " \"game_settings\": {\"path\": \"{game}/y.ini\", \"format\": \"yaml\","
            " \"rows\": [{\"section\": \"A\", \"key\": \"B\", \"initial\": \"C\"}]}},"
            "{\"id\": \"norow\", \"title\": \"N\", \"version\": 1, \"summary\": \"s\", \"match\": {\"exe\": [\"n.exe\"]},"
            " \"game_settings\": {\"path\": \"{game}/n.ini\", \"rows\": [{\"section\": \"A\", \"key\": \"B\"}]}}]}";
        struct game_fixes_index own;
        char big[9000], *at;

        CHECK( game_fixes_parse( json, strlen( json ), &own ) == HORIZON_DLLS_OK );
        CHECK( own.count == 1 && own.skipped == 2 && own.fixes[0].game_settings.row_count == 2 );
        put( DRIVE_C "/Ini/Game.exe", "MZ" );
        CHECK( (fix = game_fixes_match( &own, DRIVE_C "/Ini/Game.exe" )) );
        CHECK( game_fixes_apply( ROOT, DRIVE_C "/Ini/Game.exe", fix, setup, sizeof(setup), why, sizeof(why) ) == 0 );
        CHECK( (text = slurp( DRIVE_C "/Ini/Config/Game.ini", NULL )) );
        CHECK( strstr( text, "Width=1280" ) && strstr( text, "Height=720" ) && strstr( text, "Gamma=2" ) );
        CHECK( !strstr( text, "Width=640" ) );
        free( text );
        /* The player's change goes, and comes back with the next apply. */
        put( DRIVE_C "/Ini/Config/Game.ini", "[Video]\r\nWidth=800\r\nHeight=600\r\nGamma=3\r\n" );
        CHECK( game_fixes_apply( ROOT, DRIVE_C "/Ini/Game.exe", fix, setup, sizeof(setup), why, sizeof(why) ) == 0 );
        CHECK( (text = slurp( DRIVE_C "/Ini/Config/Game.ini", NULL )) );
        CHECK( strstr( text, "Width=1280" ) && strstr( text, "Height=720" ) && strstr( text, "Gamma=3" ) );
        free( text );
        /* Fallout: New Vegas has a settings profile: the fix's file is not written. */
        put( DRIVE_C "/FNV2/FalloutNV.exe", "MZ" );
        CHECK( game_fixes_apply( ROOT, DRIVE_C "/FNV2/FalloutNV.exe", fix, setup, sizeof(setup), why, sizeof(why) ) == 0 );
        CHECK( access( DRIVE_C "/FNV2/Config/FalloutNV.ini", F_OK ) );
        game_fixes_free( &own );

        /* A string longer than where it goes leaves its fix out, not the index. */
        at = big + sprintf( big, "{\"schema\": 1, \"serial\": 1, \"fixes\": [{\"id\": \"long\", \"title\": \"" );
        memset( at, 'x', 5000 );
        at += 5000;
        sprintf( at, "\", \"version\": 1, \"summary\": \"s\", \"match\": {\"exe\": [\"l.exe\"]}, "
                     "\"settings\": {\"d3d\": \"wine\"}}, {\"id\": \"after\", \"title\": \"A\", \"version\": 1, "
                     "\"summary\": \"s\", \"match\": {\"exe\": [\"a.exe\"]}, \"settings\": {\"d3d\": \"wine\"}}]}" );
        CHECK( game_fixes_parse( big, strlen( big ), &own ) == HORIZON_DLLS_OK );
        CHECK( own.count == 1 && own.skipped == 1 && !strcmp( own.fixes[0].id, "after" ) );
        game_fixes_free( &own );
    }

    /* Settings profiles: offered to game_settings_find for games with none
     * built in, written by the engine when such a game starts, and never over
     * a built-in one, even when the index has a copy of it. */
    {
        static const char extra[] =
            "\"profiles\": [{\"id\": \"test-game\", \"version\": 1, \"match\": {\"exe\": [\"Test.exe\"]},"
            " \"machine\": \"x86\", \"path\": \"{drive_c}/users/steamuser/Documents/Test/test.ini\","
            " \"format\": \"ini\", \"initial_text\": \"[Video]\\r\\nGamma=1\\r\\n\","
            " \"rows\": [{\"section\": \"Video\", \"key\": \"Width\", \"label\": \"Width\", \"type\": \"integer\","
            " \"initial\": \"1280\"}]},";
        const struct game_settings_profile *profile;
        static struct game_fixes_index later;
        char *index_text, *at, *joined;
        size_t size;

        snprintf( path, sizeof(path), "%s/index.json", repo );
        CHECK( (index_text = slurp( path, &size )) && (at = strstr( index_text, "\"profiles\": [" )) );
        CHECK( (joined = malloc( size + sizeof(extra) + 64 )) );
        sprintf( joined, "%.*s%s%s", (int)(at - index_text), index_text, extra, at + strlen( "\"profiles\": [" ) );
        /* Newer than anything else on the card. */
        CHECK( (at = strstr( joined, "\"serial\": " )) );
        {
            char *digits = at + strlen( "\"serial\": " ), *end = digits, *spliced;

            while (*end >= '0' && *end <= '9') end++;
            CHECK( (spliced = malloc( strlen( joined ) + 16 )) );
            sprintf( spliced, "%.*s9999%s", (int)(digits - joined), joined, end );
            put( ROOT "/" GAME_FIXES_INDEX, spliced );
            free( spliced );
        }
        free( joined );
        free( index_text );

        /* The card's index with it, newest, as the provider the runtime installs. */
        CHECK( game_fixes_load( ROOT, &later ) == HORIZON_DLLS_OK && later.serial == 9999 );
        later_profiles = &later;
        game_settings_set_provider( later_profile );
        put( DRIVE_C "/Test/Test.exe", "MZ" );
        CHECK( (profile = game_settings_find( DRIVE_C "/Test/Test.exe", 0x014c )) );
        CHECK( !strcmp( profile->path, "drive_c/users/steamuser/Documents/Test/test.ini" ) && profile->count == 1 );
        CHECK( !game_settings_find( DRIVE_C "/Test/Test.exe", 0x8664 ) );
        CHECK( game_settings_find( DRIVE_C "/FNV/FalloutNV.exe", 0x014c ) &&
               game_settings_find( DRIVE_C "/FNV/FalloutNV.exe", 0x014c )->count == 44 );
        CHECK( game_settings_prepare( ROOT, DRIVE_C "/Test/Test.exe", 0x014c ) );
        CHECK( (text = slurp( DRIVE_C "/users/steamuser/Documents/Test/test.ini", NULL )) );
        CHECK( strstr( text, "Width=1280" ) && strstr( text, "Gamma=1" ) );
        free( text );
    }

    printf( "game fixes: index read and kept, games matched by name, files, version strings and build, "
            "files checked and put in place, the game's own settings file, settings profiles, setups prepared and recorded, updates and skips\n" );
    return 0;
}
