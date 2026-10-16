#include <assert.h>
#include <stdio.h>
#include "../source/game_settings.c"

/* The arguments are the game fixes repository's profile's (add_arguments),
 * as the runtime adds them; a game without one is left as it is. */
#include "repo_profiles.h"
static int tomb_raider_append_arguments( const char *target, uint16_t machine, char *command_line, size_t size )
{
    const struct game_settings_profile *profile;

    repo_profiles_load();
    profile = game_settings_find( target, machine );
    return !profile || game_settings_add_arguments( profile, command_line, size );
}

int main(void)
{
    char command[256] = "\"D:\\Wine\\Shadow of the Tomb Raider\\SOTTR.exe\" -benchmark";
    char previous[256];

    assert(tomb_raider_append_arguments("ums0:/Wine/SOTTR.exe", 0x8664, command, sizeof(command)));
    assert(strstr(command, "-benchmark -nolauncher"));
    strcpy(previous, command);
    assert(tomb_raider_append_arguments("D:\\Wine\\sottr.EXE", 0x8664, command, sizeof(command)));
    assert(!strcmp(command, previous));
    strcpy(command, "game \"-NoLauncher\"");
    strcpy(previous, command);
    assert(tomb_raider_append_arguments("SOTTR.exe", 0x8664, command, sizeof(command)));
    assert(!strcmp(command, previous));
    strcpy(command, "game -nolauncherSuffix");
    assert(tomb_raider_append_arguments("SOTTR.exe", 0x8664, command, sizeof(command)));
    assert(strstr(command, "Suffix -nolauncher"));
    strcpy(command, "game");
    assert(tomb_raider_append_arguments("Other.exe", 0x8664, command, sizeof(command)));
    assert(!strcmp(command, "game"));
    assert(tomb_raider_append_arguments("SOTTR.exe", 0x14c, command, sizeof(command)));
    assert(!strcmp(command, "game"));
    assert(!tomb_raider_append_arguments("SOTTR.exe", 0x8664, command, sizeof("game -nolauncher") - 1));
    assert(!strcmp(command, "game"));
    assert(tomb_raider_append_arguments("SOTTR.exe", 0x8664, command, sizeof("game -nolauncher")));
    assert(!strcmp(command, "game -nolauncher"));
    puts("Tomb Raider launch arguments passed");
    return 0;
}
