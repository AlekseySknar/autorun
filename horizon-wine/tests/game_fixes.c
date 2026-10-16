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
        char rest[65536];
        char *end = serial_at + strlen( "\"serial\": " );

        while (*end >= '0' && *end <= '9') end++;
        snprintf( rest, sizeof(rest), "%s", end );
        sprintf( serial_at, "\"serial\": %u%s", serial, rest );
    }
    snprintf( path, sizeof(path), "%s/index.json", served );
    put( path, out );
    free( text );
    free( out );
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
    char path[1024], setup[640], why[256], line[512], *text;
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
    CHECK( !strcmp( setup, ROOT "/game-fixes/war3/war3-setup.exe" ) );
    CHECK( (text = slurp( ROOT "/game-fixes/war3/war3-setup.args.txt", NULL )) );
    CHECK( !strcmp( text, "\"C:\\WarCraft III\\war3.exe\"\n" ) );
    free( text );
    /* Not applied until the setup has ended. */
    CHECK( need_of( &index, war3 ) == GAME_FIX_NEW );
    CHECK( !game_fixes_finish( ROOT, DRIVE_C "/windows/autorun-setup.exe", 0, line, sizeof(line) ) );
    CHECK( game_fixes_finish( ROOT, setup, 0, line, sizeof(line) ) );
    CHECK( strstr( line, "war3 1 for war3.exe applied" ) );
    CHECK( access( ROOT "/" GAME_FIXES_RUNNING, F_OK ) && access( ROOT "/game-fixes/war3/war3-setup.args.txt", F_OK ) );
    CHECK( need_of( &index, war3 ) == GAME_FIX_APPLIED );
    CHECK( !game_fixes_finish( ROOT, setup, 0, line, sizeof(line) ) );
    CHECK( (text = slurp( DRIVE_C "/WarCraft III/war3.wine-nx.txt", NULL )) && strstr( text, "game-fix=war3 1 0\n" ) );
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
    serve_index( "\"version\": 1,\n   \"required\": true,\n   \"summary\": \"Sets the game's video",
                 "\"version\": 2,\n   \"required\": true,\n   \"summary\": \"Sets the game's video", serial + 1 );
    CHECK( game_fixes_fetch( ROOT, &transport, serial, &fetched, &newer, NULL, NULL ) == HORIZON_DLLS_OK && newer );
    game_fixes_free( &fetched );
    game_fixes_free( &index );
    CHECK( game_fixes_load( ROOT, &index ) == HORIZON_DLLS_OK && index.serial == serial + 1 );
    CHECK( game_fixes_find( &index, "war3" )->version == 2 );
    CHECK( need_of( &index, war3 ) == GAME_FIX_UPDATE );
    /* Not asked again about this version, but about the next. */
    CHECK( game_fixes_skip( ROOT, war3, game_fixes_find( &index, "war3" ) ) );
    CHECK( need_of( &index, war3 ) == GAME_FIX_SKIPPED );
    game_fixes_free( &index );
    serve_index( "\"version\": 1,\n   \"required\": true,\n   \"summary\": \"Sets the game's video",
                 "\"version\": 3,\n   \"required\": true,\n   \"summary\": \"Sets the game's video", serial + 2 );
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

    printf( "game fixes: index read and kept, games matched by name, files, version strings and build, "
            "files checked and put in place, setups prepared and recorded, updates and skips\n" );
    return 0;
}
