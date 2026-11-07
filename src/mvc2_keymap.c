/* The player's own keyboard keys. config.exe writes them to %LOCALAPPDATA%\MvC2Recomp\settings.ini, [keys]
 * (see mvc2_keymap.h); they are read once, when the game starts, and a key missing from the file keeps the
 * toolkit's default. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "input/keyboard.h"
#include "mvc2_keymap.h"

static int s_key[MVC2_KEYMAP_COUNT][2];
static int s_set[MVC2_KEYMAP_COUNT];

static int remap(size_t row, int slot, int vk)
{
    return (row < MVC2_KEYMAP_COUNT && s_set[row]) ? s_key[row][slot & 1] : vk;
}

void mvc2_keymap_init(void)
{
    static const char *const ids[MVC2_KEYMAP_COUNT] = {
#define X(id, label, k0, k1) #id,
        MVC2_KEYMAP(X)
#undef X
    };
    char base[MAX_PATH], path[MAX_PATH], key[48];
    int i, any = 0;

    if (xbox_key_map_count != MVC2_KEYMAP_COUNT)        /* the toolkit's table changed: keep its own keys */
        return;
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 40)
        return;
    snprintf(path, sizeof path, "%s\\MvC2Recomp\\settings.ini", base);
    for (i = 0; i < MVC2_KEYMAP_COUNT; i++) {
        int k0, k1;
        snprintf(key, sizeof key, "%s", ids[i]);
        k0 = (int)GetPrivateProfileIntA("keys", key, -1, path);
        snprintf(key, sizeof key, "%s_alt", ids[i]);
        k1 = (int)GetPrivateProfileIntA("keys", key, -1, path);
        if (k0 < 0 && k1 < 0)
            continue;                                   /* not in the file: the default */
        s_key[i][0] = (k0 >= 0 && k0 < 256) ? k0 : xbox_key_map[i].vk[0];
        s_key[i][1] = (k1 >= 0 && k1 < 256) ? k1 : xbox_key_map[i].vk[1];
        s_set[i] = 1;
        any = 1;
    }
    if (any)
        xbox_key_remap = remap;
}
