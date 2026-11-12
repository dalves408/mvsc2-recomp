/* config.exe -- the player's settings program, installed next to the game. Everything is kept in
 * %LOCALAPPDATA%\MvC2Recomp\settings.ini and read when the game starts.
 *
 * Video tab: the same options as the game's F1 menu, plus the window size; saved in [display].
 * Controls tab: a keyboard remapper. Each Xbox control has a key and an alternate key, saved in [keys]
 * (see src/mvc2_keymap.h).
 * Textures tab: the hi-res texture pack's on/off switch ([display] hires_textures, the F1 menu's Hi-res textures),
 * greyed out with a download button when the pack is not installed.
 * Report a problem: opens the logs folder (the game's own logs, %LOCALAPPDATA%\MvC2Recomp\logs) with the newest
 * selected, and a new GitHub issue filled in with the game version and the PC. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mvc2_keymap.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define TITLE "Marvel vs. Capcom 2 - Settings"
#define TEX_URL "https://github.com/dalves408/mvsc2-recomp/releases/tag/textures-1"

/* ---- keyboard actions (shared with the game through mvc2_keymap.h) ------------------------------ */
static const char *const IDS[MVC2_KEYMAP_COUNT] = {
#define X(id, label, k0, k1) #id,
    MVC2_KEYMAP(X)
#undef X
};
static const char *const LABELS[MVC2_KEYMAP_COUNT] = {
#define X(id, label, k0, k1) label,
    MVC2_KEYMAP(X)
#undef X
};
static const int DEFAULTS[MVC2_KEYMAP_COUNT][2] = {
#define X(id, label, k0, k1) { k0, k1 },
    MVC2_KEYMAP(X)
#undef X
};

/* ---- video options (the same as the game's F1 menu, and the window size) ------------------------ */
typedef struct {
    const char *label;       /* the line's name */
    const char *key;         /* settings.ini [display] key */
    int count;               /* number of choices */
    const char *const *text; /* their names */
    const int *value;        /* the number stored for each choice */
    int def;                 /* the choice shown when the key is missing, and by Reset */
} VideoOpt;

static const char *const T_RES[] = { "Original", "Medium", "High" };
static const int V_RES[] = { 1, 2, 3 };
static const char *const T_OFFON[] = { "Off", "On" };
static const int V_OFFON[] = { 0, 1 };
static const char *const T_STRETCH[] = { "Off (4:3)", "On (fills window)" };
static const char *const T_SPRITE[] = { "Off", "xBR", "OmniScale", "ScaleFX" };
static const int V_SPRITE[] = { 0, 1, 2, 3 };
static const char *const T_LEVEL[] = { "Off", "Low", "High" };
static const int V_LEVEL[] = { 0, 1, 2 };
static const char *const T_WIN[] = { "1x  640x480", "2x  1280x960", "3x  1920x1440", "4x  2560x1920" };
static const int V_WIN[] = { 1, 2, 3, 4 };

#define NVIDEO 9
enum { V_RENDER, V_UPSCALE, V_SPRITE_FILTER, V_SMOOTH, V_SHARPEN, V_FULLSCREEN, V_WINDOW, V_STRETCH, V_VSYNC };
static const VideoOpt VIDEO[NVIDEO] = {
    { "Render Scale",   "resolution_scale", 3, T_RES,     V_RES,    0 },
    { "Upscaling",      "upscaling",        2, T_OFFON,   V_OFFON,  0 },
    { "Sprite filter",  "sprite_filter",    4, T_SPRITE,  V_SPRITE, 0 },
    { "Smoothing",      "sprite_smoothing", 3, T_LEVEL,   V_LEVEL,  0 },
    { "Sharpen",        "sprite_sharpen",   3, T_LEVEL,   V_LEVEL,  0 },
    { "Fullscreen",     "fullscreen",       2, T_OFFON,   V_OFFON,  1 },
    { "Window size",    "window_scale",     4, T_WIN,     V_WIN,    1 },
    { "Stretch image",  "stretch",          2, T_STRETCH, V_OFFON,  0 },
    { "VSync",          "vsync",            2, T_OFFON,   V_OFFON,  1 },
};

enum { ID_TAB = 100, ID_LIST, ID_CHANGE, ID_ALT, ID_CLEAR, ID_RESET, ID_SAVE, ID_CLOSE, ID_STATUS, ID_VIDEO0 = 200,
       ID_TEX_MASTER = 300, ID_TEX_DL = 320, ID_REPORT = 330 };
enum { PAGE_VIDEO, PAGE_CONTROLS, PAGE_TEXTURES };

static int s_tex_on = 1, s_tex_installed;
static HWND s_tpage[16], s_tmaster, s_tdl, s_tinfo;
static int s_nt;

static int s_key[MVC2_KEYMAP_COUNT][2];
static int s_vsel[NVIDEO];                    /* chosen index of each video option */
static int s_dirty;
static int s_wait_row = -1, s_wait_slot;      /* waiting for a key press for this cell */
static int s_page = PAGE_VIDEO;
static HWND s_main, s_tab, s_list, s_status;
static HWND s_vcombo[NVIDEO];
static HWND s_vpage[24], s_cpage[16];         /* the controls shown on each tab */
static int s_nv, s_nc;

static void key_name(int vk, char *out, size_t n)
{
    LONG sc;
    if (!vk) { snprintf(out, n, "(none)"); return; }
    switch (vk) {
    case VK_SHIFT:   snprintf(out, n, "Shift"); return;
    case VK_CONTROL: snprintf(out, n, "Ctrl"); return;
    case VK_BACK:    snprintf(out, n, "Backspace"); return;
    case VK_RETURN:  snprintf(out, n, "Enter"); return;
    case VK_SPACE:   snprintf(out, n, "Space"); return;
    }
    sc = (LONG)MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16;
    switch (vk) {                                    /* the extended keys */
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
    case VK_END: case VK_HOME: case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
        sc |= 1L << 24;
    }
    if (!GetKeyNameTextA(sc, out, (int)n) || !out[0])
        snprintf(out, n, "Key %d", vk);
}

/* Is there a texture in this folder or below it (three levels)? */
static int has_textures(const char *dir, int depth)
{
    WIN32_FIND_DATAA fd;
    char pat[MAX_PATH + 8];
    HANDLE h;
    int found = 0;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        size_t n = strlen(fd.cFileName);
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char sub[MAX_PATH];
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            if (depth < 3 && has_textures(sub, depth + 1)) found = 1;
        } else if (n > 20 && (!_stricmp(fd.cFileName + n - 4, ".dds") || !_stricmp(fd.cFileName + n - 4, ".png")))
            found = 1;
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* The pack the installer put next to this program, or one in the player's own folder (where the game also looks) */
static int pack_installed(void)
{
    char d[MAX_PATH], *sl;
    if (GetModuleFileNameA(NULL, d, sizeof d - 16) && (sl = strrchr(d, '\\'))) {
        strcpy(sl, "\\texture_pack");
        if (has_textures(d, 0)) return 1;
    }
    if (GetEnvironmentVariableA("LOCALAPPDATA", d, sizeof d - 40)) {
        strcat(d, "\\MvC2Recomp\\texture_pack");
        if (has_textures(d, 0)) return 1;
    }
    return 0;
}

static void ini_path(char *out, size_t n, int create_dir)
{
    char base[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base))
        strcpy(base, ".");
    snprintf(out, n, "%s\\MvC2Recomp", base);
    if (create_dir) CreateDirectoryA(out, NULL);
    snprintf(out, n, "%s\\MvC2Recomp\\settings.ini", base);
}

/* ---- load and save --------------------------------------------------------------------------- */
static void load_keys(void)
{
    char path[MAX_PATH], key[48];
    int i;
    ini_path(path, sizeof path, 0);
    for (i = 0; i < MVC2_KEYMAP_COUNT; i++) {
        int k0, k1;
        snprintf(key, sizeof key, "%s", IDS[i]);
        k0 = (int)GetPrivateProfileIntA("keys", key, -1, path);
        snprintf(key, sizeof key, "%s_alt", IDS[i]);
        k1 = (int)GetPrivateProfileIntA("keys", key, -1, path);
        s_key[i][0] = (k0 >= 0 && k0 < 256) ? k0 : DEFAULTS[i][0];
        s_key[i][1] = (k1 >= 0 && k1 < 256) ? k1 : DEFAULTS[i][1];
    }
}

static void load_video(void)
{
    char path[MAX_PATH];
    int i, j;
    ini_path(path, sizeof path, 0);
    for (i = 0; i < NVIDEO; i++) {
        int v = (int)GetPrivateProfileIntA("display", VIDEO[i].key, -1000, path);
        int pick = VIDEO[i].def;
        if (v != -1000) {                              /* the choice with the nearest stored number */
            int best = 1 << 30;
            for (j = 0; j < VIDEO[i].count; j++) {
                int d = abs(VIDEO[i].value[j] - v);
                if (d < best) { best = d; pick = j; }
            }
        }
        s_vsel[i] = pick;
    }
}

static void load_textures(void)
{
    char path[MAX_PATH];
    ini_path(path, sizeof path, 0);
    s_tex_on = GetPrivateProfileIntA("display", "hires_textures", 1, path) != 0;
    s_tex_installed = pack_installed();
}

static int save_textures(void)
{
    char path[MAX_PATH];
    ini_path(path, sizeof path, 1);
    return WritePrivateProfileStringA("display", "hires_textures", s_tex_on ? "1" : "0", path);
}

static int save_keys(void)
{
    char path[MAX_PATH], key[48], val[16];
    int i, ok = 1;
    ini_path(path, sizeof path, 1);
    for (i = 0; i < MVC2_KEYMAP_COUNT; i++) {
        snprintf(key, sizeof key, "%s", IDS[i]);
        snprintf(val, sizeof val, "%d", s_key[i][0]);
        ok &= WritePrivateProfileStringA("keys", key, val, path);
        snprintf(key, sizeof key, "%s_alt", IDS[i]);
        snprintf(val, sizeof val, "%d", s_key[i][1]);
        ok &= WritePrivateProfileStringA("keys", key, val, path);
    }
    return ok;
}

static int save_video(void)
{
    char path[MAX_PATH], val[16];
    int i, ok = 1;
    ini_path(path, sizeof path, 1);
    for (i = 0; i < NVIDEO; i++) {
        snprintf(val, sizeof val, "%d", VIDEO[i].value[s_vsel[i]]);
        ok &= WritePrivateProfileStringA("display", VIDEO[i].key, val, path);
    }
    return ok;
}

/* ---- the Controls tab -------------------------------------------------------------------------- */
static void refresh_row(int i)
{
    char t[64];
    key_name(s_key[i][0], t, sizeof t);
    ListView_SetItemText(s_list, i, 1, t);
    key_name(s_key[i][1], t, sizeof t);
    ListView_SetItemText(s_list, i, 2, t);
}

static void refresh_all(void)
{
    int i;
    for (i = 0; i < MVC2_KEYMAP_COUNT; i++) refresh_row(i);
}

static void status(const char *s) { SetWindowTextA(s_status, s); }

static void begin_wait(int slot)
{
    char m[160];
    int row = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
    if (row < 0) { status("Select an action in the list first."); return; }
    s_wait_row = row; s_wait_slot = slot;
    snprintf(m, sizeof m, "Press the new %s for \"%s\"  (Esc cancels).", slot ? "alternate key" : "key", LABELS[row]);
    status(m);
    SetFocus(s_main);                  /* the key goes to this window, not a button */
}

static void assign(int vk)
{
    char m[220], n[64];
    int i, s, moved = 0;
    if (vk == VK_ESCAPE) { s_wait_row = -1; status("Cancelled."); return; }
    if (vk == VK_F11 || vk == VK_F1 || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) {
        key_name(vk, n, sizeof n);
        snprintf(m, sizeof m, "%s is used by the game itself (F1 settings, F11 or Alt+Enter fullscreen, Esc quit) and can't be assigned. "
                              "Press another key.", vk == VK_F11 ? "F11" : vk == VK_F1 ? "F1" : n);
        status(m);
        return;
    }
    for (i = 0; i < MVC2_KEYMAP_COUNT; i++)          /* a key drives one action: take it from the old one */
        for (s = 0; s < 2; s++)
            if (s_key[i][s] == vk && !(i == s_wait_row && s == s_wait_slot)) {
                s_key[i][s] = 0;
                refresh_row(i);
                key_name(vk, n, sizeof n);
                snprintf(m, sizeof m, "%s was moved here from \"%s\".", n, LABELS[i]);
                status(m);
                moved = 1;
            }
    s_key[s_wait_row][s_wait_slot] = vk;
    refresh_row(s_wait_row);
    if (!moved) {
        key_name(vk, n, sizeof n);
        snprintf(m, sizeof m, "\"%s\" is now %s.", LABELS[s_wait_row], n);
        status(m);
    }
    s_wait_row = -1;
    s_dirty = 1;
}

/* ---- tabs, reset, save ------------------------------------------------------------------------- */
static void tex_sync(void);

static void show_page(int page)
{
    int i;
    s_page = page;
    s_wait_row = -1;
    for (i = 0; i < s_nv; i++) ShowWindow(s_vpage[i], page == PAGE_VIDEO ? SW_SHOW : SW_HIDE);
    for (i = 0; i < s_nc; i++) ShowWindow(s_cpage[i], page == PAGE_CONTROLS ? SW_SHOW : SW_HIDE);
    for (i = 0; i < s_nt; i++) ShowWindow(s_tpage[i], page == PAGE_TEXTURES ? SW_SHOW : SW_HIDE);
    if (page == PAGE_TEXTURES) {
        status(s_tex_installed
               ? "Textures: the hi-res pack is installed. Changes apply the next time the game starts."
               : "Textures: the hi-res texture pack is not installed. Download it with the button, run its installer, then open this program again.");
        tex_sync();
        return;
    }
    status(page == PAGE_VIDEO
           ? "Video: these are also in the game's F1 menu. Changes apply the next time the game starts."
           : "Controls: select an action, click Change key (or double-click it), then press the key you want.");
}

/* The switch shows the stored choice; it is greyed out, with the download button shown, when there is no pack */
static void tex_sync(void)
{
    SendMessageA(s_tmaster, BM_SETCHECK, s_tex_on ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(s_tmaster, s_tex_installed);
    ShowWindow(s_tdl, (s_page == PAGE_TEXTURES && !s_tex_installed) ? SW_SHOW : SW_HIDE);
}

static void do_reset(void)
{
    int i;
    if (s_page == PAGE_VIDEO) {
        for (i = 0; i < NVIDEO; i++) {
            s_vsel[i] = VIDEO[i].def;
            SendMessageA(s_vcombo[i], CB_SETCURSEL, (WPARAM)s_vsel[i], 0);
        }
        status("Default video settings restored. Click Save to keep them.");
    } else if (s_page == PAGE_TEXTURES) {
        s_tex_on = 1;
        tex_sync();
        status("Hi-res textures switched on. Click Save to keep it.");
    } else {
        for (i = 0; i < MVC2_KEYMAP_COUNT; i++) { s_key[i][0] = DEFAULTS[i][0]; s_key[i][1] = DEFAULTS[i][1]; }
        refresh_all();
        s_wait_row = -1;
        status("Default keys restored. Click Save to keep them.");
    }
    s_dirty = 1;
}

static int do_save(void)
{
    if (!save_video() || !save_keys() || !save_textures()) {
        MessageBoxA(s_main, "The settings could not be saved.", TITLE, MB_ICONERROR);
        return 0;
    }
    s_dirty = 0;
    status("Saved. The new settings apply the next time the game starts.");
    return 1;
}

/* ---- window ----------------------------------------------------------------------------------- */
static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | style, x, y, w, h, s_main, (HMENU)(INT_PTR)id, NULL, NULL);
    if (f) SendMessageA(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}

static void make_ui(HWND h)
{
    LVCOLUMNA c;
    LVITEMA it;
    TCITEMA ti;
    int i;
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    struct { int id; const char *t; int x; int w; } cb[] = {
        { ID_CHANGE, "Change key", 20, 100 }, { ID_ALT, "Change alternate", 126, 120 }, { ID_CLEAR, "Clear", 252, 70 } };
    struct { int id; const char *t; int x; int w; } bb[] = {
        { ID_RESET, "Reset to defaults", 10, 130 }, { ID_REPORT, "Report a problem...", 150, 140 },
        { ID_SAVE, "Save", 444, 70 }, { ID_CLOSE, "Close", 520, 70 } };

    s_tab = CreateWindowExA(0, WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                            10, 10, 580, 540, h, (HMENU)(INT_PTR)ID_TAB, NULL, NULL);
    SendMessageA(s_tab, WM_SETFONT, (WPARAM)f, TRUE);
    memset(&ti, 0, sizeof ti);
    ti.mask = TCIF_TEXT;
    ti.pszText = "Video";    SendMessageA(s_tab, TCM_INSERTITEMA, 0, (LPARAM)&ti);
    ti.pszText = "Controls"; SendMessageA(s_tab, TCM_INSERTITEMA, 1, (LPARAM)&ti);
    ti.pszText = "Textures"; SendMessageA(s_tab, TCM_INSERTITEMA, 2, (LPARAM)&ti);

    /* Video tab: a name and a drop-down for each option */
    s_vpage[s_nv++] = mk("STATIC", "Video options. These are the same as the game's F1 menu, plus the window size; set them here before "
                                   "you start the game.", SS_LEFT, 30, 48, 540, 36, 0, f);
    for (i = 0; i < NVIDEO; i++) {
        int y = 90 + i * 38, j;
        s_vpage[s_nv++] = mk("STATIC", VIDEO[i].label, SS_LEFT, 40, y + 4, 180, 22, 0, f);
        s_vcombo[i] = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 230, y, 320, 200, ID_VIDEO0 + i, f);
        for (j = 0; j < VIDEO[i].count; j++)
            SendMessageA(s_vcombo[i], CB_ADDSTRING, 0, (LPARAM)VIDEO[i].text[j]);
        SendMessageA(s_vcombo[i], CB_SETCURSEL, (WPARAM)s_vsel[i], 0);
        s_vpage[s_nv++] = s_vcombo[i];
    }
    s_vpage[s_nv++] = mk("STATIC", "Render Scale draws the game larger: Original is the Xbox's 640x480, Medium twice that, High three times. "
                                   "Upscaling sharpens the picture when it is shown larger than it is drawn. The sprite filter smooths the "
                                   "characters, with its own Smoothing and Sharpen. Fullscreen starts in a borderless full-screen window; "
                                   "Window size is the window's size otherwise. Stretch image fills the window instead of keeping the 4:3 shape.",
                         SS_LEFT, 30, 436, 540, 100, 0, f);

    /* Controls tab: the key list and its buttons */
    s_list = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "", WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             20, 48, 560, 440, h, (HMENU)(INT_PTR)ID_LIST, NULL, NULL);
    SendMessageA(s_list, WM_SETFONT, (WPARAM)f, TRUE);
    ListView_SetExtendedListViewStyle(s_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    memset(&c, 0, sizeof c);
    c.mask = LVCF_TEXT | LVCF_WIDTH;
    c.pszText = "Action"; c.cx = 210; SendMessageA(s_list, LVM_INSERTCOLUMNA, 0, (LPARAM)&c);
    c.pszText = "Key"; c.cx = 165; SendMessageA(s_list, LVM_INSERTCOLUMNA, 1, (LPARAM)&c);
    c.pszText = "Alternate key"; c.cx = 165; SendMessageA(s_list, LVM_INSERTCOLUMNA, 2, (LPARAM)&c);
    for (i = 0; i < MVC2_KEYMAP_COUNT; i++) {
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (char *)LABELS[i];
        SendMessageA(s_list, LVM_INSERTITEMA, 0, (LPARAM)&it);
    }
    refresh_all();
    ListView_SetItemState(s_list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    s_cpage[s_nc++] = s_list;
    for (i = 0; i < 3; i++)
        s_cpage[s_nc++] = mk("BUTTON", cb[i].t, WS_TABSTOP | BS_PUSHBUTTON, cb[i].x, 498, cb[i].w, 28, cb[i].id, f);

    /* Textures tab: the switch, and the download button when there is no pack */
    s_tinfo = mk("STATIC", s_tex_installed
                 ? "The hi-res texture pack is installed. Switch it on or off here."
                 : "The hi-res texture pack is not installed. It replaces the game's menus, backgrounds and other art with sharper "
                   "versions (a 635 MB download). Click the button to open its download page; after installing it, open this program again.",
                 SS_LEFT, 30, 52, 540, 52, 0, f);
    s_tpage[s_nt++] = s_tinfo;
    s_tmaster = mk("BUTTON", "Use the hi-res textures", WS_TABSTOP | BS_AUTOCHECKBOX, 40, 116, 400, 24, ID_TEX_MASTER, f);
    s_tpage[s_nt++] = s_tmaster;
    s_tdl = mk("BUTTON", "Download the texture pack...", WS_TABSTOP | BS_PUSHBUTTON, 40, 160, 220, 32, ID_TEX_DL, f);
    s_tpage[s_nt++] = s_tdl;
    s_tpage[s_nt++] = mk("STATIC", "The F1 menu in the game has the same switch, Hi-res textures, to turn the pack on or off while you play.",
                         SS_LEFT, 30, 470, 540, 40, 0, f);
    tex_sync();

    /* below the tabs: status line and the buttons for every page */
    s_status = mk("STATIC", "", SS_LEFT | WS_VISIBLE, 10, 558, 580, 34, ID_STATUS, f);
    for (i = 0; i < 4; i++)
        mk("BUTTON", bb[i].t, WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, bb[i].x, 596, bb[i].w, 28, bb[i].id, f);
    show_page(PAGE_VIDEO);
}

/* ---- Report a problem ------------------------------------------------------------------------------------------------
 * Opens the logs folder with the newest log selected, and a new GitHub issue filled in with the game version and the PC.
 * The player describes the problem and drags the log onto the page (GitHub needs a free account). RecompLauncher's own
 * report button sends the same log without either. */
static void url_append(char *out, size_t n, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = strlen(out);
    for (; *s && o + 4 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-_.~", c)) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

static void reg_str(HKEY root, const char *key, const char *name, char *out, DWORD n)
{
    HKEY k;
    if (!RegOpenKeyExA(root, key, 0, KEY_READ, &k)) {
        if (RegQueryValueExA(k, name, NULL, NULL, (BYTE *)out, &n)) out[0] = 0;
        RegCloseKey(k);
    }
}

static void do_report(HWND h)
{
    char base[MAX_PATH], logs[MAX_PATH], newest[MAX_PATH] = "", pat[MAX_PATH], exe[MAX_PATH], *slash;
    char ver[32] = "?", cpu[96] = "?", gpu[128] = "?", osname[64] = "Windows", disp[16] = "", build[16] = "";
    char body[2048], url[8192], arg[MAX_PATH + 16];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    DISPLAY_DEVICEA dev;
    MEMORYSTATUSEX ms;
    DWORD i, sz, hnd;

    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base)) strcpy(base, ".");
    snprintf(logs, sizeof logs, "%s\\MvC2Recomp", base);
    CreateDirectoryA(logs, NULL);
    snprintf(logs, sizeof logs, "%s\\MvC2Recomp\\logs", base);
    CreateDirectoryA(logs, NULL);
    snprintf(pat, sizeof pat, "%s\\mvc2-*.log", logs);
    if ((f = FindFirstFileA(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do { if (strcmp(fd.cFileName, newest) > 0) snprintf(newest, sizeof newest, "%s", fd.cFileName); }
        while (FindNextFileA(f, &fd));
        FindClose(f);
    }

    /* the game's version, from mvc2_recomp.exe beside this program */
    if (GetModuleFileNameA(NULL, exe, sizeof exe) && (slash = strrchr(exe, '\\')) != NULL) {
        strcpy(slash + 1, "mvc2_recomp.exe");
        if ((sz = GetFileVersionInfoSizeA(exe, &hnd)) != 0) {
            void *vi = malloc(sz);
            VS_FIXEDFILEINFO *ff;
            UINT len;
            if (vi && GetFileVersionInfoA(exe, 0, sz, vi) && VerQueryValueA(vi, "\\", (void **)&ff, &len) && ff)
                snprintf(ver, sizeof ver, "%u.%u.%u", HIWORD(ff->dwFileVersionMS), LOWORD(ff->dwFileVersionMS),
                         HIWORD(ff->dwFileVersionLS));
            free(vi);
        }
    }
    reg_str(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString", cpu, sizeof cpu);
    reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", osname, sizeof osname);
    reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion", disp, sizeof disp);
    reg_str(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild", build, sizeof build);
    if (atoi(build) >= 22000 && !strncmp(osname, "Windows 10", 10)) osname[9] = '1';   /* 11 still says "10" there */
    memset(&dev, 0, sizeof dev);
    dev.cb = sizeof dev;
    for (i = 0; EnumDisplayDevicesA(NULL, i, &dev, 0); i++)
        if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { snprintf(gpu, sizeof gpu, "%s", dev.DeviceString); break; }
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);

    snprintf(body, sizeof body,
             "**What happened?**\n\n\n"
             "**What were you doing just before?** (menus, a fight, after losing, changing settings...)\n\n\n"
             "**Log:** drag the selected file from the logs folder that just opened (%s) into this box.\n\n"
             "---\nGame %s | %s %s (build %s) | %s | %s | %llu GB\n",
             newest[0] ? newest : "mvc2-....log", ver, osname, disp, build, cpu, gpu,
             (unsigned long long)((ms.ullTotalPhys + (1ull << 29)) >> 30));
    strcpy(url, "https://github.com/dalves408/mvsc2-recomp/issues/new?title=");
    url_append(url, sizeof url, "Problem: ");
    strcat(url, "&body=");
    url_append(url, sizeof url, body);

    if (newest[0]) {
        snprintf(arg, sizeof arg, "/select,\"%s\\%s\"", logs, newest);
        ShellExecuteA(h, "open", "explorer.exe", arg, NULL, SW_SHOWNORMAL);
    } else {
        ShellExecuteA(h, "open", logs, NULL, NULL, SW_SHOWNORMAL);
    }
    ShellExecuteA(h, "open", url, NULL, NULL, SW_SHOWNORMAL);
    MessageBoxA(h, newest[0]
                ? "Two windows opened:\n\n"
                  "  - your logs folder, with the newest log selected\n"
                  "  - a new problem report on GitHub (it needs a free GitHub account)\n\n"
                  "Say what happened, then drag the selected log file onto the report and submit it.\n\n"
                  "The log is from the last time the game ran, so report right after the problem. It holds no personal "
                  "information: your Windows user name is blanked out.\n\n"
                  "Using RecompLauncher? Its own report button sends the log for you."
                : "Your logs folder and a new problem report on GitHub opened. There is no log yet: play until the problem "
                  "happens, then click Report a problem again.",
                TITLE, MB_ICONINFORMATION);
    status("The logs folder and the problem report page opened.");
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:
        s_main = h;
        make_ui(h);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) >= ID_VIDEO0 && LOWORD(w) < ID_VIDEO0 + NVIDEO) {
            if (HIWORD(w) == CBN_SELCHANGE) {
                int i = LOWORD(w) - ID_VIDEO0;
                s_vsel[i] = (int)SendMessageA(s_vcombo[i], CB_GETCURSEL, 0, 0);
                s_dirty = 1;
                status("Changed. Click Save to keep it.");
            }
            return 0;
        }
        if (LOWORD(w) == ID_TEX_MASTER) {
            if (HIWORD(w) == BN_CLICKED) {
                s_tex_on = SendMessageA(s_tmaster, BM_GETCHECK, 0, 0) == BST_CHECKED;
                tex_sync();
                s_dirty = 1;
                status("Changed. Click Save to keep it.");
            }
            return 0;
        }
        switch (LOWORD(w)) {
        case ID_TEX_DL:
            ShellExecuteA(h, "open", TEX_URL, NULL, NULL, SW_SHOWNORMAL);
            status("The download page opened in your browser.");
            return 0;
        case ID_CHANGE: begin_wait(0); return 0;
        case ID_ALT:    begin_wait(1); return 0;
        case ID_CLEAR: {
            int row = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
            if (row < 0) { status("Select an action in the list first."); return 0; }
            s_key[row][0] = s_key[row][1] = 0;
            refresh_row(row);
            s_dirty = 1;
            status("Cleared. That action has no key until you assign one.");
            return 0;
        }
        case ID_REPORT: do_report(h); return 0;
        case ID_RESET: do_reset(); return 0;
        case ID_SAVE:  do_save(); return 0;
        case ID_CLOSE: PostMessageA(h, WM_CLOSE, 0, 0); return 0;
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)l)->idFrom == ID_TAB && ((NMHDR *)l)->code == TCN_SELCHANGE) {
            show_page((int)SendMessageA(s_tab, TCM_GETCURSEL, 0, 0));
        } else if (((NMHDR *)l)->idFrom == ID_LIST && ((NMHDR *)l)->code == NM_DBLCLK) {
            NMITEMACTIVATE *a = (NMITEMACTIVATE *)l;
            LVHITTESTINFO ht;
            memset(&ht, 0, sizeof ht);
            ht.pt = a->ptAction;
            if (ListView_SubItemHitTest(s_list, &ht) >= 0 && ht.iItem >= 0) {
                ListView_SetItemState(s_list, ht.iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                begin_wait(ht.iSubItem == 2 ? 1 : 0);
            }
        }
        break;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (s_wait_row >= 0) {
            if (!(l & (1L << 30)))
                assign((int)w);
            return 0;                       /* no beeps, no menu keys, nothing leaks through */
        }
        break;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (s_wait_row >= 0) return 0;
        break;
    case WM_CLOSE:
        if (s_dirty) {
            int r = MessageBoxA(h, "Save your changes before closing?", TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES && !do_save()) return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG msg;
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES };
    RECT r = { 0, 0, 600, 636 };
    (void)prev; (void)cmd;

    InitCommonControlsEx(&ic);
    load_keys();
    load_video();
    load_textures();
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MvC2Config";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    CreateWindowExA(0, wc.lpszClassName, TITLE,
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(s_main, show);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (s_wait_row < 0 && IsDialogMessageA(s_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
