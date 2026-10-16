#include "game_fixes.h"
#include "json_reader.h"
#include "launcher_list.h"
#include "launcher_pe.h"
#include "launcher_settings.h"
#include "game_settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* The index is a few KB for each fix; this is room for many. */
#define INDEX_MAX  (4u * 1024u * 1024u)
/* A fix's files are setup programs and settings files, KB each. */
#define FILE_MAX   (16u * 1024u * 1024u)

/***********************************************************************
 * Reading an index
 */

static int object( struct parser *p, int (*field)( struct parser *p, const char *key, void *data ), void *data )
{
    char key[64];

    if (!consume( p, '{' )) return 0;
    whitespace( p );
    if (p->p < p->end && *p->p == '}') { p->p++; return 1; }
    for (;;)
    {
        if (!json_string( p, key, sizeof(key) ) || !consume( p, ':' )) return 0;
        if (!field( p, key, data )) return 0;
        whitespace( p );
        if (p->p < p->end && *p->p == '}') { p->p++; return 1; }
        if (p->p == p->end || *p->p++ != ',') return 0;
    }
}

static int array( struct parser *p, int (*item)( struct parser *p, void *data ), void *data )
{
    if (!consume( p, '[' )) return 0;
    whitespace( p );
    if (p->p < p->end && *p->p == ']') { p->p++; return 1; }
    for (;;)
    {
        if (!item( p, data )) return 0;
        whitespace( p );
        if (p->p < p->end && *p->p == ']') { p->p++; return 1; }
        if (p->p == p->end || *p->p++ != ',') return 0;
    }
}

/* A string too long for where it goes, or a value of the wrong kind, leaves
 * the fix out; only JSON that cannot be read at all stops the index. */
struct reading
{
    struct game_fix *fix;
    int bad;
    char run[64];
    /* A settings profile being read, and the row and choice in it. */
    struct game_fixes_profile *profile;
    struct game_setting *row;
    unsigned int preset;
    unsigned int choice_slot, choice_capacity;  /* where the row's choices are in owned */
};

/* A string too long for where it goes is passed over whole, so the rest of the
 * index still reads. */
static int string_or_bad( struct parser *p, struct reading *r, char *out, size_t size )
{
    const unsigned char *start;
    char scratch[4096];

    whitespace( p );
    start = p->p;
    if (p->p == p->end || *p->p != '"') { r->bad = 1; return skip_value( p ); }
    if (json_string( p, scratch, sizeof(scratch) ) && strlen( scratch ) < size)
    {
        memcpy( out, scratch, strlen( scratch ) + 1 );
        return 1;
    }
    r->bad = 1;
    p->p = start;
    return skip_string( p );
}

static unsigned int uint_or_bad( struct parser *p, struct reading *r )
{
    unsigned long long value;
    const unsigned char *start = p->p;

    if (json_uint( p, &value ) && value <= 0xffffffffu) return (unsigned int)value;
    p->p = start;
    r->bad = 1;
    skip_value( p );
    return 0;
}

/* A file name as the repository has them: no folders, nothing hidden. */
static int valid_name( const char *name )
{
    const char *c;

    if (!name[0] || name[0] == '.') return 0;
    for (c = name; *c; c++)
        if (!isalnum( (unsigned char)*c ) && !strchr( "._-+", *c )) return 0;
    return 1;
}

static int valid_hash( const char *text )
{
    size_t i;

    if (strlen( text ) != 64) return 0;
    for (i = 0; i < 64; i++) if (!isxdigit( (unsigned char)text[i] )) return 0;
    return 1;
}

/* A path relative to a folder, with / between its parts, none empty or "."
 * and ".." only where up allows it. */
static int valid_relative( const char *path, int up )
{
    const char *part = path;

    if (!path[0] || path[0] == '/' || strpbrk( path, "\\:" )) return 0;
    for (;;)
    {
        size_t length = strcspn( part, "/" );

        if (!length || (length == 1 && part[0] == '.')) return 0;
        if (length == 2 && part[0] == '.' && part[1] == '.' && !up) return 0;
        if (!part[length]) return 1;
        part += length + 1;
    }
}

static int read_exe( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fix *f = r->fix;
    char name[64] = "";
    size_t length;

    if (f->exe_count >= GAME_FIX_EXES) { r->bad = 1; return skip_value( p ); }
    if (!string_or_bad( p, r, name, sizeof(name) )) return 0;
    length = strlen( name );
    if (length < 5 || strcasecmp( name + length - 4, ".exe" ) || strpbrk( name, "/\\:" )) r->bad = 1;
    else memcpy( f->exes[f->exe_count++], name, length + 1 );
    return 1;
}

static int read_mark( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fix *f = r->fix;
    char path[128] = "";

    if (f->mark_count >= GAME_FIX_MARKS) { r->bad = 1; return skip_value( p ); }
    if (!string_or_bad( p, r, path, sizeof(path) )) return 0;
    if (!valid_relative( path, 1 )) r->bad = 1;
    else memcpy( f->marks[f->mark_count++], path, strlen( path ) + 1 );
    return 1;
}

static int read_version( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_fix *f = r->fix;
    struct game_fix_version *version;

    if (f->version_count >= GAME_FIX_VERSIONS || !key[0] || strlen( key ) >= sizeof(version->key))
    { r->bad = 1; return skip_value( p ); }
    version = &f->versions[f->version_count];
    if (!string_or_bad( p, r, version->value, sizeof(version->value) )) return 0;
    if (!version->value[0]) { r->bad = 1; return 1; }
    snprintf( version->key, sizeof(version->key), "%s", key );
    f->version_count++;
    return 1;
}

static int read_timestamp( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fix *f = r->fix;
    unsigned int value = uint_or_bad( p, r );

    if (f->timestamp_count >= GAME_FIX_BUILDS) r->bad = 1;
    else f->timestamps[f->timestamp_count++] = value;
    return 1;
}

static int read_size( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fix *f = r->fix;
    const unsigned char *start = p->p;
    unsigned long long value;

    if (!json_uint( p, &value ) || !value)
    {
        p->p = start;
        r->bad = 1;
        return skip_value( p );
    }
    if (f->size_count >= GAME_FIX_BUILDS) r->bad = 1;
    else f->sizes[f->size_count++] = value;
    return 1;
}

static int read_match_field( struct parser *p, const char *key, void *data )
{
    if (!strcmp( key, "exe" )) return array( p, read_exe, data );
    if (!strcmp( key, "files" )) return array( p, read_mark, data );
    if (!strcmp( key, "version" )) return object( p, read_version, data );
    if (!strcmp( key, "timestamp" )) return array( p, read_timestamp, data );
    if (!strcmp( key, "size" )) return array( p, read_size, data );
    ((struct reading *)data)->bad = 1;
    return skip_value( p );
}

static int read_file_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_fix_file *file = &r->fix->files[r->fix->file_count];

    if (!strcmp( key, "name" )) return string_or_bad( p, r, file->name, sizeof(file->name) );
    if (!strcmp( key, "sha256" )) return string_or_bad( p, r, file->sha256, sizeof(file->sha256) );
    if (!strcmp( key, "size" ))
    {
        unsigned long long size;
        const unsigned char *start = p->p;

        if (json_uint( p, &size ) && size <= FILE_MAX) { file->size = size; return 1; }
        p->p = start;
        r->bad = 1;
        return skip_value( p );
    }
    r->bad = 1;
    return skip_value( p );
}

static int read_file( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fix *f = r->fix;
    struct game_fix_file *file;
    unsigned int i;

    if (f->file_count >= GAME_FIX_FILES) { r->bad = 1; return skip_value( p ); }
    file = &f->files[f->file_count];
    memset( file, 0, sizeof(*file) );
    if (!object( p, read_file_field, data )) return 0;
    if (!valid_name( file->name ) || !valid_hash( file->sha256 )) { r->bad = 1; return 1; }
    for (i = 0; i < f->file_count; i++)
        if (!strcasecmp( f->files[i].name, file->name )) { r->bad = 1; return 1; }
    for (i = 0; i < 64; i++) file->sha256[i] = (char)tolower( (unsigned char)file->sha256[i] );
    f->file_count++;
    return 1;
}

static int read_install_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_fix_install *install = &r->fix->installs[r->fix->install_count];

    if (!strcmp( key, "file" )) return string_or_bad( p, r, install->file_name, sizeof(install->file_name) );
    if (!strcmp( key, "to" )) return string_or_bad( p, r, install->to, sizeof(install->to) );
    if (!strcmp( key, "keep" ))
    {
        if (json_bool( p, &install->keep )) return 1;
        r->bad = 1;
        return skip_value( p );
    }
    r->bad = 1;
    return skip_value( p );
}

/* {game}/ or {drive_c}/ and then a relative path, where only {stem} may stand. */
static int valid_destination( const char *to )
{
    const char *rest, *c;

    if (!strncmp( to, "{game}/", 7 )) rest = to + 7;
    else if (!strncmp( to, "{drive_c}/", 10 )) rest = to + 10;
    else return 0;
    for (c = rest; *c; c++)
    {
        if (*c == '}') return 0;
        if (*c != '{') continue;
        if (strncmp( c, "{stem}", 6 )) return 0;
        c += 5;
    }
    return valid_relative( rest, 0 );
}

static int read_install( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fix *f = r->fix;
    struct game_fix_install *install;

    if (f->install_count >= GAME_FIX_INSTALLS) { r->bad = 1; return skip_value( p ); }
    install = &f->installs[f->install_count];
    memset( install, 0, sizeof(*install) );
    if (!object( p, read_install_field, data )) return 0;
    /* The file it names is found among the files once they are all read. */
    if (!install->file_name[0] || !valid_destination( install->to )) { r->bad = 1; return 1; }
    f->install_count++;
    return 1;
}

static int read_setting( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_fix *f = r->fix;
    struct game_fix_setting *setting;
    const char *c;

    if (f->setting_count >= GAME_FIX_SETTINGS || strlen( key ) >= sizeof(setting->key))
    { r->bad = 1; return skip_value( p ); }
    for (c = key; *c; c++)
        if (!islower( (unsigned char)*c ) && !isdigit( (unsigned char)*c ) && *c != '-') r->bad = 1;
    setting = &f->settings[f->setting_count];
    if (!string_or_bad( p, r, setting->value, sizeof(setting->value) )) return 0;
    if (!key[0] || strpbrk( setting->value, "\r\n" )) { r->bad = 1; return 1; }
    snprintf( setting->key, sizeof(setting->key), "%s", key );
    f->setting_count++;
    return 1;
}

static int read_row_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_fix_row *row = &r->fix->game_settings.rows[r->fix->game_settings.row_count];

    if (!strcmp( key, "section" )) return string_or_bad( p, r, row->section, sizeof(row->section) );
    if (!strcmp( key, "key" )) return string_or_bad( p, r, row->key, sizeof(row->key) );
    if (!strcmp( key, "initial" )) return string_or_bad( p, r, row->initial, sizeof(row->initial) );
    r->bad = 1;
    return skip_value( p );
}

static int read_row( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fix_settings_file *file = &r->fix->game_settings;
    struct game_fix_row *row;

    if (file->row_count >= GAME_FIX_ROWS) { r->bad = 1; return skip_value( p ); }
    row = &file->rows[file->row_count];
    memset( row, 0, sizeof(*row) );
    if (!object( p, read_row_field, data )) return 0;
    if (!row->section[0] || !row->key[0] || !row->initial[0] ||
        strpbrk( row->section, "\r\n" ) || strpbrk( row->key, "\r\n" ) || strpbrk( row->initial, "\r\n" ))
    { r->bad = 1; return 1; }
    file->row_count++;
    return 1;
}

static int read_game_settings_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_fix_settings_file *file = &r->fix->game_settings;

    if (!strcmp( key, "path" )) return string_or_bad( p, r, file->path, sizeof(file->path) );
    if (!strcmp( key, "initial_text" )) return string_or_bad( p, r, file->initial_text, sizeof(file->initial_text) );
    if (!strcmp( key, "rows" )) return array( p, read_row, data );
    if (!strcmp( key, "format" ))
    {
        char format[8] = "";

        if (!string_or_bad( p, r, format, sizeof(format) )) return 0;
        if (!strcmp( format, "xml" )) file->xml = 1;
        else if (strcmp( format, "ini" )) r->bad = 1;
        return 1;
    }
    r->bad = 1;
    return skip_value( p );
}

static int read_fix_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_fix *f = r->fix;

    if (!strcmp( key, "id" )) return string_or_bad( p, r, f->id, sizeof(f->id) );
    if (!strcmp( key, "title" )) return string_or_bad( p, r, f->title, sizeof(f->title) );
    if (!strcmp( key, "summary" )) return string_or_bad( p, r, f->summary, sizeof(f->summary) );
    if (!strcmp( key, "version" )) { f->version = uint_or_bad( p, r ); return 1; }
    if (!strcmp( key, "required" ))
    {
        if (json_bool( p, &f->required )) return 1;
        r->bad = 1;
        return skip_value( p );
    }
    if (!strcmp( key, "match" )) return object( p, read_match_field, data );
    if (!strcmp( key, "run" )) return string_or_bad( p, r, r->run, sizeof(r->run) );
    if (!strcmp( key, "files" )) return array( p, read_file, data );
    if (!strcmp( key, "install" )) return array( p, read_install, data );
    if (!strcmp( key, "settings" )) return object( p, read_setting, data );
    if (!strcmp( key, "game_settings" ))
    {
        r->fix->has_game_settings = 1;
        return object( p, read_game_settings_field, data );
    }
    /* Something a later build knows how to do: applying the rest of the fix
     * without it would be applying half of it. */
    r->bad = 1;
    return skip_value( p );
}

static int file_index( const struct game_fix *f, const char *name )
{
    unsigned int i;

    for (i = 0; i < f->file_count; i++)
        if (!strcasecmp( f->files[i].name, name )) return (int)i;
    return -1;
}

static int read_fix( struct parser *p, void *data )
{
    struct game_fixes_index *index = data;
    struct reading r = {0};
    struct game_fix *f;
    unsigned int i;
    const char *c;

    if (index->count == index->capacity)
    {
        unsigned int capacity = index->capacity ? index->capacity * 2 : 16;
        struct game_fix *grown = realloc( index->fixes, capacity * sizeof(*grown) );

        if (!grown) return 0;
        index->fixes = grown;
        index->capacity = capacity;
    }
    f = &index->fixes[index->count];
    memset( f, 0, sizeof(*f) );
    f->run = -1;
    r.fix = f;
    if (!object( p, read_fix_field, &r )) return 0;

    /* A fix with nothing to tell its game by but files beside it would be
     * offered to every program in that folder. */
    if (!f->id[0] || !f->title[0] || !f->version || (!f->exe_count && !f->version_count)) r.bad = 1;
    for (c = f->id; *c; c++)
        if (!islower( (unsigned char)*c ) && !isdigit( (unsigned char)*c ) && *c != '-') r.bad = 1;
    if (r.run[0] && (f->run = file_index( f, r.run )) < 0) r.bad = 1;
    for (i = 0; i < f->install_count && !r.bad; i++)
    {
        struct game_fix_install *install = &f->installs[i];
        int at = file_index( f, install->file_name );

        if (at < 0) r.bad = 1;
        else install->file = (unsigned int)at;
    }
    if (f->has_game_settings && (!valid_destination( f->game_settings.path ) || !f->game_settings.row_count))
        r.bad = 1;
    if (!f->file_count && !f->setting_count && !f->has_game_settings) r.bad = 1;
    for (i = 0; i < index->count && !r.bad; i++)
        if (!strcmp( index->fixes[i].id, f->id )) r.bad = 1;
    if (r.bad) index->skipped++;
    else index->count++;
    return 1;
}

/***********************************************************************
 * Reading settings profiles
 */

/* As many as a resolution list has, and more. */
#define PROFILE_CHOICES 256

static void *own_block( struct game_fixes_profile *g, size_t size )
{
    void *block;

    if (g->owned_count == g->owned_capacity)
    {
        unsigned int capacity = g->owned_capacity ? g->owned_capacity * 2 : 32;
        void **grown = realloc( g->owned, capacity * sizeof(*grown) );

        if (!grown) return NULL;
        g->owned = grown;
        g->owned_capacity = capacity;
    }
    if (!(block = calloc( 1, size ))) return NULL;
    g->owned[g->owned_count++] = block;
    return block;
}

static void profile_free( struct game_fixes_profile *g )
{
    unsigned int i;

    if (!g) return;
    for (i = 0; i < g->owned_count; i++) free( g->owned[i] );
    free( g->owned );
    free( g );
}

/* A string the profile keeps, shorter than max; null too when nullable. Only
 * running out of memory stops the index. */
static int profile_string( struct parser *p, struct reading *r, const char **out, size_t max, int nullable )
{
    const unsigned char *start;
    char scratch[4096], *copy;
    size_t length;

    whitespace( p );
    start = p->p;
    if (nullable && json_null( p )) { *out = NULL; return 1; }
    if (p->p == p->end || *p->p != '"') { r->bad = 1; return skip_value( p ); }
    if (!json_string( p, scratch, sizeof(scratch) ) || (length = strlen( scratch )) >= max)
    {
        r->bad = 1;
        p->p = start;
        return skip_string( p );
    }
    if (!(copy = own_block( r->profile, length + 1 ))) return 0;
    memcpy( copy, scratch, length + 1 );
    *out = copy;
    return 1;
}

static int read_row_preset( struct parser *p, void *data )
{
    struct reading *r = data;

    if (r->preset >= 4) { r->bad = 1; return skip_value( p ); }
    return profile_string( p, r, &r->row->presets[r->preset++], GAME_SETTINGS_VALUE_SIZE, 1 );
}

static int read_choice_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_setting_choice *choice = (struct game_setting_choice *)&r->row->choices[r->row->choice_count];

    if (!strcmp( key, "label" )) return profile_string( p, r, &choice->label, 64, 0 );
    if (!strcmp( key, "value" )) return profile_string( p, r, &choice->value, GAME_SETTINGS_VALUE_SIZE, 1 );
    r->bad = 1;
    return skip_value( p );
}

static int read_choice( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fixes_profile *g = r->profile;

    if (r->row->choice_count >= PROFILE_CHOICES) { r->bad = 1; return skip_value( p ); }
    /* The row's choices grow where the profile keeps them. */
    if (r->row->choice_count == r->choice_capacity)
    {
        unsigned int capacity = r->choice_capacity * 2;
        struct game_setting_choice *grown = realloc( g->owned[r->choice_slot], capacity * sizeof(*grown) );

        if (!grown) return 0;
        memset( grown + r->choice_capacity, 0, (capacity - r->choice_capacity) * sizeof(*grown) );
        g->owned[r->choice_slot] = grown;
        r->row->choices = grown;
        r->choice_capacity = capacity;
    }
    if (!object( p, read_choice_field, data )) return 0;
    if (!r->row->choices[r->row->choice_count].label) { r->bad = 1; return 1; }
    r->row->choice_count++;
    return 1;
}

static int read_profile_row_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_setting *row = r->row;
    unsigned int index = row - r->profile->rows;

    /* An XML file's settings have no section. */
    if (!strcmp( key, "section" )) return profile_string( p, r, &row->section, 64, 1 );
    if (!strcmp( key, "key" )) return profile_string( p, r, &row->key, 64, 0 );
    if (!strcmp( key, "key2" )) return profile_string( p, r, &row->key2, 64, 0 );
    if (!strcmp( key, "label" )) return profile_string( p, r, &row->label, 96, 0 );
    if (!strcmp( key, "initial" )) return profile_string( p, r, &row->initial, GAME_SETTINGS_VALUE_SIZE, 1 );
    if (!strcmp( key, "help" )) return profile_string( p, r, &r->profile->help[index], 256, 0 );
    if (!strcmp( key, "presets" )) { r->preset = 0; return array( p, read_row_preset, data ); }
    if (!strcmp( key, "choices" ))
    {
        if (row->choices) { r->bad = 1; return skip_value( p ); }
        if (!(row->choices = own_block( r->profile, 8 * sizeof(*row->choices) ))) return 0;
        r->choice_slot = r->profile->owned_count - 1;
        r->choice_capacity = 8;
        return array( p, read_choice, data );
    }
    if (!strcmp( key, "type" ))
    {
        static const char *const types[] = { "number", "integer", "text", "resolution", "d3d_api" };
        char type[16] = "";
        unsigned int i;

        if (!string_or_bad( p, r, type, sizeof(type) )) return 0;
        for (i = 0; i < sizeof(types) / sizeof(types[0]); i++)
            if (!strcmp( type, types[i] )) { row->type = (enum game_setting_type)i; return 1; }
        r->bad = 1;
        return 1;
    }
    r->bad = 1;
    return skip_value( p );
}

static int read_profile_row( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fixes_profile *g = r->profile;

    if (g->profile.count >= GAME_PROFILE_ROWS) { r->bad = 1; return skip_value( p ); }
    r->row = &g->rows[g->profile.count];
    r->row->type = GAME_SETTING_TEXT;
    if (!object( p, read_profile_row_field, data )) return 0;
    if (!r->row->key || !r->row->label ||
        (r->row->type == GAME_SETTING_RESOLUTION && !r->row->key2) ||
        (!r->row->choices && r->row->choice_count)) { r->bad = 1; return 1; }
    if (!r->row->choice_count) r->row->choices = NULL;
    g->profile.count++;
    return 1;
}

static int read_profile_preset( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fixes_profile *g = r->profile;

    if (g->profile.preset_count >= 4) { r->bad = 1; return skip_value( p ); }
    if (!profile_string( p, r, &g->presets[g->profile.preset_count], 32, 0 )) return 0;
    if (g->presets[g->profile.preset_count]) g->profile.preset_count++;
    return 1;
}

static int read_pending( struct parser *p, void *data )
{
    struct reading *r = data;

    if (r->preset >= 2) { r->bad = 1; return skip_value( p ); }
    return profile_string( p, r, &r->profile->profile.pending_resolution[r->preset++], 64, 0 );
}

static int read_add_argument_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_argument *argument = &r->profile->add_arguments[r->profile->profile.add_argument_count];

    if (!strcmp( key, "unless" )) return profile_string( p, r, &argument->unless, 128, 0 );
    if (!strcmp( key, "add" )) return profile_string( p, r, &argument->add, 256, 0 );
    r->bad = 1;
    return skip_value( p );
}

static int read_add_argument( struct parser *p, void *data )
{
    struct reading *r = data;
    struct game_fixes_profile *g = r->profile;
    struct game_argument *argument;

    if (g->profile.add_argument_count >= sizeof(g->add_arguments) / sizeof(g->add_arguments[0]))
    { r->bad = 1; return skip_value( p ); }
    argument = &g->add_arguments[g->profile.add_argument_count];
    if (!object( p, read_add_argument_field, data )) return 0;
    if (!argument->unless || !argument->unless[0] || !argument->add || !argument->add[0]) { r->bad = 1; return 1; }
    g->profile.add_argument_count++;
    return 1;
}

static int read_profile_field( struct parser *p, const char *key, void *data )
{
    struct reading *r = data;
    struct game_fixes_profile *g = r->profile;
    const char *text = NULL;

    if (!strcmp( key, "id" )) return string_or_bad( p, r, g->id, sizeof(g->id) );
    if (!strcmp( key, "version" )) { g->version = uint_or_bad( p, r ); return 1; }
    if (!strcmp( key, "match" )) return object( p, read_match_field, data );
    if (!strcmp( key, "rows" )) return array( p, read_profile_row, data );
    if (!strcmp( key, "presets" )) return array( p, read_profile_preset, data );
    if (!strcmp( key, "pending_resolution" )) { r->preset = 0; return array( p, read_pending, data ); }
    if (!strcmp( key, "initial_text" )) return profile_string( p, r, &g->profile.initial_text, 4096, 0 );
    if (!strcmp( key, "arguments" )) return profile_string( p, r, &g->profile.arguments, 1024, 0 );
    if (!strcmp( key, "add_arguments" )) return array( p, read_add_argument, data );
    if (!strcmp( key, "path" ))
    {
        /* Under drive_c, which is where the engine opens it from the runtime's folder. */
        if (!profile_string( p, r, &text, 256, 0 )) return 0;
        if (text && (strncmp( text, "{drive_c}/", 10 ) || !valid_relative( text + 10, 0 ))) r->bad = 1;
        else if (text)
        {
            char *path = own_block( g, strlen( text ) );

            if (!path) return 0;
            snprintf( path, strlen( text ), "drive_c/%s", text + 10 );
            g->profile.path = path;
        }
        return 1;
    }
    if (!strcmp( key, "machine" ) || !strcmp( key, "format" ) || !strcmp( key, "update" ))
    {
        char value[32] = "";

        if (!string_or_bad( p, r, value, sizeof(value) )) return 0;
        if (key[0] == 'm')
        {
            if (!strcmp( value, "x86" )) g->profile.machine = 0x014c;
            else if (!strcmp( value, "x64" )) g->profile.machine = 0x8664;
            else r->bad = 1;
        }
        else if (key[0] == 'f')
        {
            if (!strcmp( value, "xml" )) g->profile.format = GAME_SETTINGS_XML;
            else if (strcmp( value, "ini" )) r->bad = 1;
        }
        /* A hook this build does not have: the profile would be half of itself. */
        else if (!(g->profile.update = game_settings_update_hook( value ))) r->bad = 1;
        return 1;
    }
    r->bad = 1;
    return skip_value( p );
}

static int read_profile( struct parser *p, void *data )
{
    struct game_fixes_index *index = data;
    struct game_fixes_profile *g;
    struct reading r = {0};
    unsigned int i;
    const char *c;

    if (index->profile_count == index->profile_capacity)
    {
        unsigned int capacity = index->profile_capacity ? index->profile_capacity * 2 : 16;
        struct game_fixes_profile **grown = realloc( index->profiles, capacity * sizeof(*grown) );

        if (!grown) return 0;
        index->profiles = grown;
        index->profile_capacity = capacity;
    }
    if (!(g = calloc( 1, sizeof(*g) ))) return 0;
    g->match.run = -1;
    r.fix = &g->match;
    r.profile = g;
    if (!object( p, read_profile_field, &r )) { profile_free( g ); return 0; }

    for (c = g->id; *c; c++)
        if (!islower( (unsigned char)*c ) && !isdigit( (unsigned char)*c ) && *c != '-') r.bad = 1;
    /* A settings file with its settings, or arguments alone: never half of a file. */
    if (!g->id[0] || !g->version || (!g->match.exe_count && !g->match.version_count) || !g->profile.machine ||
        !g->profile.path != !g->profile.count ||
        (!g->profile.path && !g->profile.arguments && !g->profile.add_argument_count)) r.bad = 1;
    for (i = 0; i < g->profile.count && !r.bad; i++)
        if (g->profile.format == GAME_SETTINGS_INI && !g->rows[i].section) r.bad = 1;
    for (i = 0; i < index->profile_count && !r.bad; i++)
        if (!strcmp( index->profiles[i]->id, g->id )) r.bad = 1;
    if (r.bad)
    {
        profile_free( g );
        index->profiles_skipped++;
        return 1;
    }
    /* The engine's view of it: what it shows, what it writes and where. */
    g->profile.executable = g->match.exe_count ? g->match.exes[0] : g->id;
    g->profile.settings = g->rows;
    if (g->profile.preset_count) g->profile.presets = g->presets;
    if (g->profile.add_argument_count) g->profile.add_arguments = g->add_arguments;
    for (i = 0; i < g->profile.count; i++)
        if (g->help[i]) g->profile.help = g->help;
    index->profiles[index->profile_count++] = g;
    return 1;
}

static int read_top( struct parser *p, const char *key, void *data )
{
    struct game_fixes_index *index = data;
    unsigned long long value;

    if (!strcmp( key, "schema" ) || !strcmp( key, "serial" ))
    {
        if (!json_uint( p, &value ) || value > 0xffffffffu) return 0;
        if (key[1] == 'c') index->schema = (int)value;
        else index->serial = (unsigned int)value;
        return 1;
    }
    if (!strcmp( key, "fixes" )) return array( p, read_fix, data );
    if (!strcmp( key, "profiles" )) return array( p, read_profile, data );
    return skip_value( p );
}

enum horizon_dlls_result game_fixes_parse( const char *text, size_t size, struct game_fixes_index *out )
{
    struct parser p = { (const unsigned char *)text, (const unsigned char *)text + size, 0 };

    memset( out, 0, sizeof(*out) );
    if (!object( &p, read_top, out ))
    {
        game_fixes_free( out );
        return HORIZON_DLLS_INVALID;
    }
    whitespace( &p );
    if (p.p != p.end || out->schema != GAME_FIXES_SCHEMA)
    {
        game_fixes_free( out );
        return HORIZON_DLLS_INVALID;
    }
    return HORIZON_DLLS_OK;
}

void game_fixes_free( struct game_fixes_index *index )
{
    unsigned int i;

    for (i = 0; i < index->profile_count; i++) profile_free( index->profiles[i] );
    free( index->profiles );
    free( index->fixes );
    memset( index, 0, sizeof(*index) );
}

static int join( char *out, size_t size, const char *a, const char *b )
{
    int n = snprintf( out, size, "%s/%s", a, b );

    return n > 0 && (size_t)n < size;
}

static enum horizon_dlls_result load_file( const char *path, struct game_fixes_index *out )
{
    enum horizon_dlls_result result;
    FILE *file = fopen( path, "rb" );
    char *text;
    long size;

    memset( out, 0, sizeof(*out) );
    if (!file) return errno == ENOENT ? HORIZON_DLLS_NOT_FOUND : HORIZON_DLLS_IO;
    if (fseek( file, 0, SEEK_END ) || (size = ftell( file )) < 0 || size > INDEX_MAX || fseek( file, 0, SEEK_SET ))
    {
        fclose( file );
        return HORIZON_DLLS_INVALID;
    }
    if (!(text = malloc( size + 1 ))) { fclose( file ); return HORIZON_DLLS_MEMORY; }
    if (fread( text, 1, size, file ) != (size_t)size) result = HORIZON_DLLS_IO;
    else result = game_fixes_parse( text, size, out );
    free( text );
    fclose( file );
    return result;
}

enum horizon_dlls_result game_fixes_load( const char *root, struct game_fixes_index *out )
{
    struct game_fixes_index downloaded, bundled;
    enum horizon_dlls_result a, b;
    char path[600];

    memset( out, 0, sizeof(*out) );
    if (!join( path, sizeof(path), root, GAME_FIXES_INDEX )) return HORIZON_DLLS_IO;
    a = load_file( path, &downloaded );
    if (!join( path, sizeof(path), root, GAME_FIXES_BUNDLED )) { game_fixes_free( &downloaded ); return HORIZON_DLLS_IO; }
    b = load_file( path, &bundled );
    if (a == HORIZON_DLLS_OK && (b != HORIZON_DLLS_OK || downloaded.serial >= bundled.serial))
    {
        game_fixes_free( &bundled );
        *out = downloaded;
        return HORIZON_DLLS_OK;
    }
    game_fixes_free( &downloaded );
    if (b == HORIZON_DLLS_OK) { *out = bundled; return HORIZON_DLLS_OK; }
    memset( out, 0, sizeof(*out) );
    return a == HORIZON_DLLS_NOT_FOUND ? b : a;
}

/***********************************************************************
 * Downloading
 */

struct memory
{
    char *data;
    size_t size, capacity, limit;
};

static int to_memory( void *context, const void *data, size_t size )
{
    struct memory *m = context;
    size_t wanted;
    char *grown;

    if (size > m->limit - m->size) return 0;
    if (m->size + size + 1 > m->capacity)
    {
        wanted = m->capacity ? m->capacity : 16384;
        while (wanted < m->size + size + 1) wanted *= 2;
        if (!(grown = realloc( m->data, wanted ))) return 0;
        m->data = grown;
        m->capacity = wanted;
    }
    memcpy( m->data + m->size, data, size );
    m->size += size;
    m->data[m->size] = 0;
    return 1;
}

/* Every folder on the way to path's last part. */
static int make_parents( const char *path )
{
    char partial[600];
    size_t i, length = strlen( path );

    if (length >= sizeof(partial)) return 0;
    memcpy( partial, path, length + 1 );
    /* Past "sdmc:/" or "ums0:/", which are there. */
    for (i = strchr( partial, '/' ) ? (size_t)(strchr( partial, '/' ) - partial) + 1 : 0; i < length; i++)
    {
        if (partial[i] != '/') continue;
        partial[i] = 0;
        if (mkdir( partial, 0777 ) && errno != EEXIST) return 0;
        partial[i] = '/';
    }
    return 1;
}

/* Through a .part file beside it; FAT cannot rename over a file. */
static int write_atomically( const char *path, const void *data, size_t size )
{
    char part[640];
    FILE *file;
    int ok;

    if ((size_t)snprintf( part, sizeof(part), "%s.part", path ) >= sizeof(part) || !make_parents( path )) return 0;
    if (!(file = fopen( part, "wb" ))) return 0;
    ok = fwrite( data, 1, size, file ) == size;
    ok = !fclose( file ) && ok;
    if (ok) remove( path );
    if (!ok || rename( part, path ))
    {
        remove( part );
        return 0;
    }
    return 1;
}

enum horizon_dlls_result game_fixes_fetch( const char *root, const struct horizon_dlls_transport *transport,
                                           unsigned int card_serial, struct game_fixes_index *out, int *newer,
                                           horizon_dlls_progress progress, void *opaque )
{
    struct memory body = { .limit = INDEX_MAX };
    enum horizon_dlls_result result;
    char path[600];

    *newer = 0;
    memset( out, 0, sizeof(*out) );
    result = transport->fetch( transport->opaque, GAME_FIXES_INDEX_URL, to_memory, &body, progress, opaque );
    if (result == HORIZON_DLLS_OK) result = game_fixes_parse( body.data, body.size, out );
    if (result == HORIZON_DLLS_OK && out->serial > card_serial)
    {
        if (!join( path, sizeof(path), root, GAME_FIXES_INDEX ) || !write_atomically( path, body.data, body.size ))
            result = HORIZON_DLLS_IO;
        else *newer = 1;
        if (result != HORIZON_DLLS_OK) game_fixes_free( out );
    }
    free( body.data );
    return result;
}

static int fix_file_path( const char *root, const struct game_fix *fix, unsigned int file, char *out, size_t size )
{
    int n = snprintf( out, size, "%s/" GAME_FIXES_DIR "/%s/%s", root, fix->id, fix->files[file].name );

    return n > 0 && (size_t)n < size;
}

/* Whether the card's copy is these bytes. */
static int file_matches( const char *path, const struct game_fix_file *f )
{
    struct memory m = { .limit = FILE_MAX };
    char buffer[16384], hash[65];
    FILE *file = fopen( path, "rb" );
    size_t got;
    int ok = 1;

    if (!file) return 0;
    while (ok && (got = fread( buffer, 1, sizeof(buffer), file )) > 0) ok = to_memory( &m, buffer, got );
    fclose( file );
    if (ok && m.size == f->size)
    {
        horizon_dlls_sha256( m.data ? m.data : "", m.size, hash );
        ok = !strcmp( hash, f->sha256 );
    }
    else ok = 0;
    free( m.data );
    return ok;
}

int game_fixes_ready( const char *root, const struct game_fix *fix )
{
    char path[640];
    unsigned int i;

    for (i = 0; i < fix->file_count; i++)
        if (!fix_file_path( root, fix, i, path, sizeof(path) ) || !file_matches( path, &fix->files[i] )) return 0;
    return 1;
}

enum horizon_dlls_result game_fixes_download( const char *root, const struct horizon_dlls_transport *transport,
                                              const struct game_fix *fix, horizon_dlls_progress progress,
                                              void *opaque )
{
    char path[640], url[384], hash[65];
    unsigned int i;

    for (i = 0; i < fix->file_count; i++)
    {
        const struct game_fix_file *f = &fix->files[i];
        struct memory body = { .limit = FILE_MAX };
        enum horizon_dlls_result result;

        if (!fix_file_path( root, fix, i, path, sizeof(path) )) return HORIZON_DLLS_IO;
        if (file_matches( path, f )) continue;
        if ((size_t)snprintf( url, sizeof(url), GAME_FIXES_RAW "%s/%s", fix->id, f->name ) >= sizeof(url))
            return HORIZON_DLLS_INVALID;
        if (progress && progress( opaque, f->name, 0, 0 )) return HORIZON_DLLS_CANCELLED;
        result = transport->fetch( transport->opaque, url, to_memory, &body, NULL, NULL );
        if (result == HORIZON_DLLS_OK)
        {
            horizon_dlls_sha256( body.data ? body.data : "", body.size, hash );
            if (body.size != f->size || strcmp( hash, f->sha256 )) result = HORIZON_DLLS_HASH;
            else if (!write_atomically( path, body.data ? body.data : "", body.size )) result = HORIZON_DLLS_IO;
        }
        free( body.data );
        if (result != HORIZON_DLLS_OK) return result;
    }
    return HORIZON_DLLS_OK;
}

/***********************************************************************
 * Recognising a game
 */

static const char *base_name( const char *path )
{
    const char *name = path;

    for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

/* The executable's folder joined with a relative path, its ".." parts taken
 * off the folder rather than handed to the file system. */
static int resolve( const char *exe_path, const char *relative, char *out, size_t size )
{
    const char *part = relative;
    size_t length, root;
    char *slash;

    if ((length = base_name( exe_path ) - exe_path) == 0 || length >= size) return 0;
    memcpy( out, exe_path, length - 1 );
    out[length - 1] = 0;
    root = strchr( out, '/' ) ? (size_t)(strchr( out, '/' ) - out) : 0;
    while (*part)
    {
        size_t n = strcspn( part, "/" );

        if (n == 2 && part[0] == '.' && part[1] == '.')
        {
            if (!(slash = strrchr( out, '/' )) || (size_t)(slash - out) <= root) return 0;
            *slash = 0;
        }
        else
        {
            length = strlen( out );
            if (length + 1 + n + 1 > size) return 0;
            out[length] = '/';
            memcpy( out + length + 1, part, n );
            out[length + 1 + n] = 0;
        }
        part += n;
        if (*part == '/') part++;
    }
    return 1;
}

/* What the executable says of itself, read once for every fix asked about it. */
struct exe_info
{
    int read, pe;
    unsigned int timestamp;
    unsigned long long size;
    unsigned char *version;
    uint32_t version_size;
};

static void read_exe_info( const char *path, struct exe_info *info )
{
    struct launcher_pe pe;
    unsigned char header[12];

    info->read = 1;
    if (!launcher_pe_open( &pe, path )) return;
    info->pe = 1;
    info->size = pe.file_size;
    /* The link time is the COFF header's second field, after "PE\0\0" and
     * the machine and section count. */
    if (launcher_pe_read_at( &pe, 0x3c, header, 4 ) &&
        launcher_pe_read_at( &pe, launcher_u32( header ), header, sizeof(header) ))
        info->timestamp = launcher_u32( header + 8 );
    if (!launcher_pe_load_resource( &pe, LAUNCHER_RT_VERSION, -1, &info->version, &info->version_size ))
        info->version = NULL;
    launcher_pe_close( &pe );
}

/* value is what the fix wants: a * at its end matches any rest. */
static int version_matches( const char *have, const char *value )
{
    size_t length = strlen( value );

    if (length && value[length - 1] == '*') return !strncasecmp( have, value, length - 1 );
    return !strcasecmp( have, value );
}

/* How much of the game the fix matched, or 0 when it does not. */
static int fix_score( const struct game_fix *f, const char *exe_path, struct exe_info *info )
{
    const char *name = base_name( exe_path );
    char path[640], have[256];
    struct stat st;
    unsigned int j;
    int found, score = 0;

    if (f->exe_count)
    {
        for (j = 0, found = 0; j < f->exe_count && !found; j++) found = !strcasecmp( name, f->exes[j] );
        if (!found) return 0;
        score++;
    }
    for (j = 0; j < f->mark_count; j++)
        if (!resolve( exe_path, f->marks[j], path, sizeof(path) ) || stat( path, &st )) return 0;
    score += f->mark_count;
    if (!f->version_count && !f->timestamp_count && !f->size_count) return score;

    if (!info->read) read_exe_info( exe_path, info );
    if (!info->pe) return 0;
    for (j = 0; j < f->version_count; j++)
    {
        const char *keys[2] = { f->versions[j].key, NULL };

        if (!info->version || !launcher_version_string( info->version, info->version_size, keys, have, sizeof(have) ) ||
            !version_matches( have, f->versions[j].value )) return 0;
    }
    score += 2 * f->version_count;
    if (f->timestamp_count)
    {
        for (j = 0, found = 0; j < f->timestamp_count && !found; j++) found = f->timestamps[j] == info->timestamp;
        if (!found) return 0;
        score += 4;
    }
    if (f->size_count)
    {
        for (j = 0, found = 0; j < f->size_count && !found; j++) found = f->sizes[j] == info->size;
        if (!found) return 0;
        score += 4;
    }
    return score;
}

const struct game_fix *game_fixes_match( const struct game_fixes_index *index, const char *exe_path )
{
    const struct game_fix *best = NULL;
    struct exe_info info = {0};
    int best_score = 0, score;
    unsigned int i;

    for (i = 0; i < index->count; i++)
        if ((score = fix_score( &index->fixes[i], exe_path, &info )) > best_score)
        {
            best = &index->fixes[i];
            best_score = score;
        }
    free( info.version );
    return best;
}

const struct game_fixes_profile *game_fixes_match_profile( const struct game_fixes_index *index, const char *exe_path,
                                                           uint16_t machine )
{
    const struct game_fixes_profile *best = NULL;
    struct exe_info info = {0};
    int best_score = 0, score;
    unsigned int i;

    for (i = 0; i < index->profile_count; i++)
        if (index->profiles[i]->profile.machine == machine &&
            (score = fix_score( &index->profiles[i]->match, exe_path, &info )) > best_score)
        {
            best = index->profiles[i];
            best_score = score;
        }
    free( info.version );
    return best;
}

/* The card's index for game_settings_find, read once and never freed: the
 * launcher's settings screen and the runtime hold the profiles it hands out. */
static struct game_fixes_index installed_profiles;

static const struct game_settings_profile *installed_profile( const char *target, uint16_t machine )
{
    const struct game_fixes_profile *found = game_fixes_match_profile( &installed_profiles, target, machine );

    return found ? &found->profile : NULL;
}

void game_fixes_install_profiles( const char *root )
{
    static int installed;

    if (installed) return;
    installed = 1;
    if (game_fixes_load( root, &installed_profiles ) != HORIZON_DLLS_OK) return;
    if (installed_profiles.profile_count) game_settings_set_provider( installed_profile );
}

const struct game_fix *game_fixes_find( const struct game_fixes_index *index, const char *id )
{
    unsigned int i;

    for (i = 0; i < index->count; i++)
        if (!strcmp( index->fixes[i].id, id )) return &index->fixes[i];
    return NULL;
}

/***********************************************************************
 * What a game has had
 */

static int settings_path( const char *root, const char *exe_path, char *out, size_t size )
{
    if (launcher_settings_on_usb( exe_path ))
    {
        char folder[600];

        if (!join( folder, sizeof(folder), root, "program-settings" ) ||
            (mkdir( folder, 0777 ) && errno != EEXIST)) return 0;
    }
    return launcher_program_settings_path( root, exe_path, out, size );
}

void game_fixes_state( const char *root, const char *exe_path, struct game_fix_state *state )
{
    struct launcher_kv kv;
    char path[768], value[96];

    memset( state, 0, sizeof(*state) );
    if (!launcher_program_settings_path( root, exe_path, path, sizeof(path) ) || !launcher_kv_load( &kv, path ))
        return;
    if (launcher_kv_get( &kv, "game-fix", value, sizeof(value) ) &&
        sscanf( value, "%31s %u %x", state->id, &state->applied, &state->exit_code ) < 2)
        memset( state, 0, sizeof(*state) );
    if (launcher_kv_get( &kv, "game-fix-skip", value, sizeof(value) ) &&
        sscanf( value, "%31s %u", state->skipped_id, &state->skipped ) != 2)
        state->skipped_id[0] = 0, state->skipped = 0;
}

enum game_fix_need game_fixes_need( const struct game_fix *fix, const struct game_fix_state *state )
{
    int applied = !strcmp( state->id, fix->id ) && state->applied;

    if (applied && state->applied >= fix->version) return state->exit_code ? GAME_FIX_FAILED : GAME_FIX_APPLIED;
    if (!strcmp( state->skipped_id, fix->id ) && state->skipped >= fix->version) return GAME_FIX_SKIPPED;
    return applied ? GAME_FIX_UPDATE : GAME_FIX_NEW;
}

static int record( const char *root, const char *exe_path, const char *key, const char *value,
                   const struct game_fix *settings_of )
{
    struct launcher_kv kv;
    char path[768];
    unsigned int i;

    if (!settings_path( root, exe_path, path, sizeof(path) ) || !launcher_kv_load( &kv, path )) return 0;
    for (i = 0; settings_of && i < settings_of->setting_count; i++)
        if (!launcher_kv_set( &kv, settings_of->settings[i].key, settings_of->settings[i].value )) return 0;
    if (!launcher_kv_set( &kv, key, value )) return 0;
    if (!strcmp( key, "game-fix" ) && !launcher_kv_set( &kv, "game-fix-skip", NULL )) return 0;
    return launcher_kv_save( &kv, path );
}

int game_fixes_skip( const char *root, const char *exe_path, const struct game_fix *fix )
{
    char value[64];

    snprintf( value, sizeof(value), "%s %u", fix->id, fix->version );
    return record( root, exe_path, "game-fix-skip", value, NULL );
}

int game_fixes_forget( const char *root, const char *exe_path )
{
    return record( root, exe_path, "game-fix", NULL, NULL );
}

/* Where an install entry goes for this game. */
static int destination( const char *root, const char *exe_path, const char *to, char *out, size_t size )
{
    const char *name = base_name( exe_path ), *rest;
    size_t used, stem = strlen( name );

    if (stem > 4 && !strcasecmp( name + stem - 4, ".exe" )) stem -= 4;
    if (!strncmp( to, "{game}/", 7 ))
    {
        used = name - exe_path;
        if (!used || used >= size) return 0;
        memcpy( out, exe_path, used );     /* with its slash */
        rest = to + 7;
    }
    else
    {
        int n = snprintf( out, size, "%s/drive_c/", root );

        if (n < 0 || (size_t)n >= size) return 0;
        used = n;
        rest = to + 10;
    }
    while (*rest)
    {
        if (!strncmp( rest, "{stem}", 6 ))
        {
            if (used + stem >= size) return 0;
            memcpy( out + used, name, stem );
            used += stem;
            rest += 6;
        }
        else
        {
            if (used + 1 >= size) return 0;
            out[used++] = *rest++;
        }
    }
    out[used] = 0;
    return 1;
}

static int copy_file( const char *from, const char *to )
{
    struct memory m = { .limit = FILE_MAX };
    char buffer[16384];
    FILE *file = fopen( from, "rb" );
    size_t got;
    int ok = 1;

    if (!file) return 0;
    while (ok && (got = fread( buffer, 1, sizeof(buffer), file )) > 0) ok = to_memory( &m, buffer, got );
    ok = !ferror( file ) && ok;
    fclose( file );
    ok = ok && write_atomically( to, m.data ? m.data : "", m.size );
    free( m.data );
    return ok;
}

static int write_text( const char *path, const char *text )
{
    return write_atomically( path, text, strlen( text ) );
}

/* The fix's values into the game's own settings file, through Autorun's game
 * settings engine, as rows of text it writes whatever the file held. A game
 * Autorun has settings of its own for (game_settings_find) keeps those: one source of
 * values for a file, never two. Returns 0 with why when the file could not be
 * read or written. */
static int apply_game_settings( const char *root, const char *exe_path, const struct game_fix *fix,
                                char *why, size_t why_size )
{
    const struct game_fix_settings_file *file = &fix->game_settings;
    struct game_setting rows[GAME_FIX_ROWS];
    struct game_settings_profile profile;
    struct game_settings *settings;
    char path[640], device[16];
    const char *relative;
    unsigned int i;
    int ok;

    if (game_settings_find( exe_path, 0x014c ) || game_settings_find( exe_path, 0x8664 )) return 1;
    /* The engine opens root/path: the device is the root, sdmc: or ums0:. */
    if (!destination( root, exe_path, file->path, path, sizeof(path) ) || !(relative = strstr( path, ":/" )) ||
        (size_t)(relative - path) + 2 > sizeof(device))
    {
        snprintf( why, why_size, "The path of the game's settings file is too long." );
        return 0;
    }
    memcpy( device, path, relative - path + 1 );
    device[relative - path + 1] = 0;
    relative += 2;
    memset( rows, 0, sizeof(rows) );
    for (i = 0; i < file->row_count; i++)
    {
        rows[i].section = file->rows[i].section;
        rows[i].key = file->rows[i].key;
        rows[i].label = file->rows[i].key;
        rows[i].initial = file->rows[i].initial;
        rows[i].type = GAME_SETTING_TEXT;
    }
    memset( &profile, 0, sizeof(profile) );
    profile.executable = base_name( exe_path );
    profile.path = relative;
    profile.settings = rows;
    profile.count = file->row_count;
    profile.format = file->xml ? GAME_SETTINGS_XML : GAME_SETTINGS_INI;
    profile.initial_text = file->initial_text[0] ? file->initial_text : NULL;
    if (!make_parents( path ) || !(settings = game_settings_open( device, &profile )))
    {
        snprintf( why, why_size, "Could not read %s.", path );
        return 0;
    }
    /* Every row, every time: the engine sets initial values only once a file,
     * and a newer version of the fix has to reach a file set by an older one. */
    for (i = 0, ok = 1; i < file->row_count && ok; i++) ok = game_settings_set( settings, i, rows[i].initial );
    ok = ok && game_settings_save( settings );
    game_settings_close( settings );
    if (!ok) snprintf( why, why_size, "Could not write %s.", path );
    return ok;
}

int game_fixes_apply( const char *root, const char *exe_path, const struct game_fix *fix,
                      char *setup, size_t setup_size, char *why, size_t why_size )
{
    char from[640], to[640], value[96], text[1400], args[640], dos[600];
    struct stat st;
    unsigned int i;

    for (i = 0; i < fix->install_count; i++)
    {
        const struct game_fix_install *install = &fix->installs[i];

        if (!fix_file_path( root, fix, install->file, from, sizeof(from) ) ||
            !destination( root, exe_path, install->to, to, sizeof(to) ))
        {
            snprintf( why, why_size, "The path for %s is too long.", fix->files[install->file].name );
            return -1;
        }
        if (install->keep && !stat( to, &st )) continue;
        if (!copy_file( from, to ))
        {
            snprintf( why, why_size, "Could not write %s.", to );
            return -1;
        }
    }
    if (fix->has_game_settings && !apply_game_settings( root, exe_path, fix, why, why_size )) return -1;
    if (fix->run < 0)
    {
        snprintf( value, sizeof(value), "%s %u 0", fix->id, fix->version );
        if (record( root, exe_path, "game-fix", value, fix )) return 0;
        snprintf( why, why_size, "Could not save the game's settings." );
        return -1;
    }

    /* The setup runs next, told which game it is for; the game's settings are
     * changed now and the fix recorded when the setup has ended. */
    if (!fix_file_path( root, fix, (unsigned int)fix->run, setup, setup_size ) ||
        !launcher_dos_path( exe_path, dos, sizeof(dos) ) || !launcher_args_path( setup, args, sizeof(args) ) ||
        (size_t)snprintf( text, sizeof(text), "\"%s\"\n", dos ) >= sizeof(text) ||
        !write_text( args, text ) ||
        (size_t)snprintf( text, sizeof(text), "%s\n%s\n%s %u\n", setup, exe_path, fix->id, fix->version ) >= sizeof(text) ||
        !join( to, sizeof(to), root, GAME_FIXES_RUNNING ) || !write_text( to, text ))
    {
        snprintf( why, why_size, "Could not prepare the fix's setup program." );
        return -1;
    }
    if (fix->setting_count && !record( root, exe_path, "game-fix-skip", NULL, fix ))
    {
        snprintf( why, why_size, "Could not save the game's settings." );
        return -1;
    }
    return 1;
}

static int read_line( FILE *file, char *out, size_t size )
{
    if (!fgets( out, (int)size, file )) return 0;
    out[strcspn( out, "\r\n" )] = 0;
    return 1;
}

int game_fixes_finish( const char *root, const char *program, unsigned int exit_code, char *line, size_t size )
{
    char path[600], setup[600], game[600], id[32], value[96], args[640];
    unsigned int version;
    FILE *file;
    int ok;

    if (!join( path, sizeof(path), root, GAME_FIXES_RUNNING ) || !(file = fopen( path, "r" ))) return 0;
    ok = read_line( file, setup, sizeof(setup) ) && read_line( file, game, sizeof(game) ) &&
         read_line( file, value, sizeof(value) ) && sscanf( value, "%31s %u", id, &version ) == 2;
    fclose( file );
    if (ok && strcasecmp( setup, program )) return 0;    /* another program ran first */
    remove( path );
    if (launcher_args_path( setup, args, sizeof(args) )) remove( args );
    if (!ok)
    {
        snprintf( line, size, "[FIXES] running.txt could not be read; removed" );
        return 1;
    }
    snprintf( value, sizeof(value), "%s %u %x", id, version, exit_code );
    ok = record( root, game, "game-fix", value, NULL );
    snprintf( line, size, "[FIXES] %s %u for %s %s (exit code 0x%x)%s", id, version, base_name( game ),
              exit_code ? "had a step fail" : "applied", exit_code, ok ? "" : "; its settings could not be saved" );
    return 1;
}
