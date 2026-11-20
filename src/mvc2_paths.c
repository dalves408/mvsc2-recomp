/* mvc2_paths.c - where the game keeps its settings, saves and logs.
 *
 * The player build is portable: everything lives in the "data" folder beside mvc2_recomp.exe (settings.ini,
 * SaveData, Partitions, logs, and the texture pack downloads Setup keeps when asked), so the whole game is one folder
 * that can be moved or deleted, with nothing in the registry or under the user profile. config.exe, installed beside
 * the game, uses the same folder.
 *
 * Developer builds keep %LOCALAPPDATA%\MvC2Recomp, so test scripts can give each run its own folder through
 * LOCALAPPDATA. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

const char *mvc2_data_dir(void)
{
    static char dir[MAX_PATH];
    if (!dir[0]) {
        char *slash;
#ifdef MVC2_RELEASE
        if (!GetModuleFileNameA(NULL, dir, MAX_PATH - 8) || (slash = strrchr(dir, '\\')) == NULL)
            strcpy(dir, ".\\data");
        else
            strcpy(slash, "\\data");
#else
        if (GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH - 20) && dir[0])
            strcat(dir, "\\MvC2Recomp");
        else
            strcpy(dir, ".\\data");
        (void)slash;
#endif
        CreateDirectoryA(dir, NULL);
    }
    return dir;
}
