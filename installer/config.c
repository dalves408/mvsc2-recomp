/* config.exe: the player's settings program, installed next to the game. The same options as the game's F1 menu, so they
 * can be set before the game runs, plus the window size. Saved in %LOCALAPPDATA%\MvC2Recomp\settings.ini, [display], and
 * read when the game starts. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

typedef struct {
    const char *label, *key;
    int count;
    const char *const *text;
    const int *value;
    int def;                    /* the choice shown when the key is missing, and by Defaults */
    HWND combo;
} Opt;

static const char *const T_RES[] = { "1x  640x480 (Native)", "2x  1280x960", "3x  1920x1440", "4x  2560x1920", "5x  3200x2400" };
static const int V_RES[] = { 1, 2, 3, 4, 5 };
static const char *const T_OFFON[] = { "Off", "On" };
static const int V_OFFON[] = { 0, 1 };
static const char *const T_STRETCH[] = { "Off (4:3)", "On (fills window)" };
static const char *const T_AA[] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA" };
static const int V_AA[] = { 1, 2, 4, 8 };
static const char *const T_ANISO[] = { "Off", "2x", "4x", "8x", "16x" };
static const int V_ANISO[] = { 1, 2, 4, 8, 16 };
static const char *const T_WIN[] = { "1x  640x480", "2x  1280x960", "3x  1920x1440", "4x  2560x1920" };
static const int V_WIN[] = { 1, 2, 3, 4 };

static Opt s_opts[] = {
    { "Render Scale", "resolution_scale", 5, T_RES, V_RES, 0 },
    { "Upscaling", "upscaling", 2, T_OFFON, V_OFFON, 0 },
    { "Fullscreen", "fullscreen", 2, T_OFFON, V_OFFON, 1 },
    { "Window size", "window_scale", 4, T_WIN, V_WIN, 1 },
    { "Stretch image", "stretch", 2, T_STRETCH, V_OFFON, 0 },
    { "VSync", "vsync", 2, T_OFFON, V_OFFON, 1 },
    { "Anti-aliasing", "msaa", 4, T_AA, V_AA, 0 },
    { "Anisotropic filter", "anisotropy", 5, T_ANISO, V_ANISO, 0 },
};
#define OPT_COUNT ((int)(sizeof s_opts / sizeof *s_opts))
#define ROW 32

static char s_ini[MAX_PATH];
static HFONT s_font;

static int ini_path(void)
{
    char base[MAX_PATH], dir[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 60)
        return 0;
    snprintf(dir, sizeof dir, "%s\\MvC2Recomp", base);
    CreateDirectoryA(dir, NULL);
    snprintf(s_ini, sizeof s_ini, "%s\\settings.ini", dir);
    return 1;
}

static HWND child(HWND parent, const char *cls, const char *text, DWORD style, int x, int y, int w, int h, INT_PTR id)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)id,
                             GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

static void show_saved(int defaults)
{
    int i, k;
    for (i = 0; i < OPT_COUNT; i++) {
        Opt *o = &s_opts[i];
        int sel = o->def;
        if (!defaults) {
            int v = (int)GetPrivateProfileIntA("display", o->key, o->value[o->def], s_ini);
            for (k = 0; k < o->count; k++)
                if (o->value[k] == v) sel = k;
        }
        SendMessageA(o->combo, CB_SETCURSEL, sel, 0);
    }
}

static int save_values(void)
{
    int i, ok = 1;
    for (i = 0; i < OPT_COUNT; i++) {
        Opt *o = &s_opts[i];
        char v[16];
        int sel = (int)SendMessageA(o->combo, CB_GETCURSEL, 0, 0);
        snprintf(v, sizeof v, "%d", o->value[sel < 0 ? o->def : sel]);
        ok = WritePrivateProfileStringA("display", o->key, v, s_ini) && ok;
    }
    return ok;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        int i, k, y = 16;
        NONCLIENTMETRICSA nm;
        nm.cbSize = sizeof nm;
        SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
        s_font = CreateFontIndirectA(&nm.lfMessageFont);
        for (i = 0; i < OPT_COUNT; i++, y += ROW) {
            Opt *o = &s_opts[i];
            child(h, "STATIC", o->label, 0, 16, y + 4, 130, 20, 0);
            o->combo = child(h, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 150, y, 190, 180, 100 + i);
            for (k = 0; k < o->count; k++)
                SendMessageA(o->combo, CB_ADDSTRING, 0, (LPARAM)o->text[k]);
        }
        child(h, "STATIC", "In the game: F1 opens these settings, Esc asks to quit, F11 or Alt+Enter switches fullscreen. On a "
                           "controller: Back + Start opens settings. A window too large for your screen is reduced to fit.",
              0, 16, y + 4, 324, 54, 0);
        y += 62;
        child(h, "BUTTON", "Defaults", WS_TABSTOP, 16, y, 84, 28, IDRETRY);
        child(h, "BUTTON", "Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 166, y, 84, 28, IDOK);
        child(h, "BUTTON", "Cancel", WS_TABSTOP, 256, y, 84, 28, IDCANCEL);
        show_saved(0);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDRETRY:
            show_saved(1);
            return 0;
        case IDOK:
            if (!save_values()) {
                MessageBoxA(h, "The settings could not be saved.", "Marvel vs. Capcom 2 Settings", MB_ICONERROR);
                return 0;
            }
            DestroyWindow(h);
            return 0;
        case IDCANCEL:
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc = { 0 };
    RECT r = { 0, 0, 356, 16 + OPT_COUNT * ROW + 62 + 28 + 16 };
    MSG msg;
    HWND h;
    (void)prev; (void)cmd;

    if (!ini_path()) {
        MessageBoxA(NULL, "The settings folder could not be found.", "Marvel vs. Capcom 2 Settings", MB_ICONERROR);
        return 1;
    }
    InitCommonControls();
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MvC2Config";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    h = CreateWindowExA(0, wc.lpszClassName, "Marvel vs. Capcom 2 Settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(h, show);
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageA(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return 0;
}
