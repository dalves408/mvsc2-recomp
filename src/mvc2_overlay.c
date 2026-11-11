/*
 * The ESC "Do you want to quit?" prompt and the F1 settings menu, after NHL Hitz 2003's.
 *
 * A layered, non-activating popup owned by the game window and sized to its client area: the last frame stays
 * underneath, dimmed. While it is up the keyboard and controller 1 go to the overlay and the game is paused: its
 * pushbuffer thread waits in the flip (mvc2_overlay_flip_wait), the sound holds its place, and every pad reads neutral.
 *
 * Choices are saved at once in %LOCALAPPDATA%\MvC2Recomp\settings.ini, [display]. Render scale and hi-res textures
 * apply when the game resumes; the others at once.
 *
 * Keys: F1 settings, Esc quit prompt. In them: Up/Down choose, Left/Right change, Enter confirms, Esc closes; Y and N in
 * the prompt. Controller 1: Back + Start opens settings, D-pad or left stick moves, A confirms, B or Start closes.
 */
#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video/gpu_present.h"

extern HWND xbox_fb_window(void);
extern int  xbox_fb_is_fullscreen(void);
extern void xbox_fb_toggle_fullscreen(void);
extern int  (*xbox_fb_key_hook)(unsigned vk, long lparam, int down);
extern void (*xbox_fb_tick_hook)(void);
extern void xbox_InputKeysClear(void);
extern void xa2_set_muted(int muted);
extern volatile int xbox_input_block, xbox_input_back_start, xbox_apu_hold;
extern void mvc2_video_request(int scale, int hires, int sprite_filter, int smooth, int sharpen);
extern int mvc2_pack_count(void);

static volatile int s_active;
static int s_settings, s_sel, s_armed, s_quit_yes;
static HWND s_win;
static int s_cw, s_ch, s_ox, s_oy;
static int s_pad_valid, s_unblock_wait, s_chord_held;
static DWORD s_unblock_t;

static int s_scale = 1, s_stretch, s_vsync = 1, s_upscaling, s_hires = 1, s_sprite, s_smooth, s_sharpen;

/* ---- settings.ini ------------------------------------------------------- */

static int cfg_path(char *out, size_t n)
{
    char base[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 40)
        return 0;
    snprintf(out, n, "%s\\MvC2Recomp", base);
    CreateDirectoryA(out, NULL);
    snprintf(out, n, "%s\\MvC2Recomp\\settings.ini", base);
    return 1;
}

static int cfg_get(const char *key, int def)
{
    char path[MAX_PATH];
    return cfg_path(path, sizeof path) ? (int)GetPrivateProfileIntA("display", key, def, path) : def;
}

static void cfg_put(const char *key, int v)
{
    char path[MAX_PATH], val[16];
    if (!cfg_path(path, sizeof path))
        return;
    snprintf(val, sizeof val, "%d", v);
    WritePrivateProfileStringA("display", key, val, path);
}

/* ---- the menu's lines ---------------------------------------------------- */

enum { IT_RES, IT_UP, IT_SPRITE, IT_SMOOTH, IT_SHARPEN, IT_FS, IT_STRETCH, IT_VSYNC, IT_HIRES, IT_CLOSE, IT_QUIT, SET_ITEMS };

static const char *const SET_LABEL[SET_ITEMS] = {
    "Render Scale", "Upscaling", "Sprite filter", "Smoothing", "Sharpen", "Fullscreen", "Stretch image", "VSync", "Hi-res textures", "Close menu", "Quit game" };
static const char *const SET_RES[3] = { "Original", "Medium", "High" };
static const char *const SET_OFFON[2] = { "Off", "On" };
static const char *const SET_SPRITE[4] = { "Off", "xBR", "OmniScale", "ScaleFX" };
static const char *const SET_LEVEL[3] = { "Off", "Low", "High" };
static const char *const SET_STRETCH[2] = { "Off (4:3)", "On (fills window)" };

static int set_count(int i)
{
    switch (i) {
    case IT_RES: return 3;
    case IT_SPRITE: return 4;
    case IT_SMOOTH: case IT_SHARPEN: return s_sprite ? 3 : 0;
    case IT_UP: case IT_FS: case IT_STRETCH: case IT_VSYNC: return 2;
    case IT_HIRES: return mvc2_pack_count() ? 2 : 0;
    }
    return 0;
}

static int set_get(int i)
{
    switch (i) {
    case IT_RES: return s_scale - 1;
    case IT_UP: return s_upscaling;
    case IT_SPRITE: return s_sprite;
    case IT_SMOOTH: return s_smooth;
    case IT_SHARPEN: return s_sharpen;
    case IT_HIRES: return s_hires;
    case IT_FS: return xbox_fb_is_fullscreen() ? 1 : 0;
    case IT_STRETCH: return s_stretch;
    case IT_VSYNC: return s_vsync;
    }
    return 0;
}

static const char *set_text(int i)
{
    int v = set_get(i);
    switch (i) {
    case IT_RES: return SET_RES[v];
    case IT_SPRITE: return SET_SPRITE[v];
    case IT_SMOOTH: case IT_SHARPEN: return s_sprite ? SET_LEVEL[v] : "Needs a sprite filter";
    case IT_STRETCH: return SET_STRETCH[v];
    case IT_UP: case IT_FS: case IT_VSYNC: return SET_OFFON[v];
    case IT_HIRES: return mvc2_pack_count() ? SET_OFFON[v] : "Not installed";
    }
    return "";
}

static void set_put(int i, int v)
{
    switch (i) {
    case IT_RES: s_scale = v + 1; cfg_put("resolution_scale", s_scale); break;
    case IT_UP: s_upscaling = v; cfg_put("upscaling", v); gpu_present_set_upscale(v); break;
    case IT_SPRITE: s_sprite = v; cfg_put("sprite_filter", v); break;
    case IT_SMOOTH: s_smooth = v; cfg_put("sprite_smoothing", v); break;
    case IT_SHARPEN: s_sharpen = v; cfg_put("sprite_sharpen", v); break;
    case IT_FS: xbox_fb_toggle_fullscreen(); break;
    case IT_STRETCH: s_stretch = v; cfg_put("stretch", v); gpu_present_set_stretch(v); break;
    case IT_VSYNC: s_vsync = v; cfg_put("vsync", v); gpu_present_set_vsync(v); break;
    case IT_HIRES: s_hires = v; cfg_put("hires_textures", v); break;
    }
    mvc2_video_request(s_scale, s_hires, s_sprite, s_smooth, s_sharpen);
}

/* ---- drawing --------------------------------------------------------------- */

typedef struct { HDC sdc, mdc; HBITMAP bm; HGDIOBJ oldbm; uint32_t *px; int cw, ch; POINT o; } Canvas;

static int canvas_begin(Canvas *c)
{
    RECT cr;
    BITMAPINFO bi;
    void *bits = NULL;
    HWND game = xbox_fb_window();
    int i;

    c->o.x = c->o.y = 0;
    if (!s_win || !game || IsIconic(game))
        return 0;
    GetClientRect(game, &cr);
    ClientToScreen(game, &c->o);
    c->cw = cr.right; c->ch = cr.bottom;
    if (c->cw <= 0 || c->ch <= 0)
        return 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = c->cw; bi.bmiHeader.biHeight = -c->ch;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    c->sdc = GetDC(NULL);
    c->mdc = CreateCompatibleDC(c->sdc);
    c->bm = CreateDIBSection(c->sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!c->bm || !bits) {
        if (c->bm) DeleteObject(c->bm);
        DeleteDC(c->mdc);
        ReleaseDC(NULL, c->sdc);
        return 0;
    }
    c->oldbm = SelectObject(c->mdc, c->bm);
    c->px = (uint32_t *)bits;
    for (i = 0; i < c->cw * c->ch; i++)
        c->px[i] = 0x96000000u;                     /* dim: black, alpha 150 */
    SetBkMode(c->mdc, TRANSPARENT);
    return 1;
}

static void canvas_end(Canvas *c, int dx, int dy, int dw, int dh)
{
    BLENDFUNCTION bf;
    POINT p0 = { 0, 0 };
    SIZE sz;
    int x, y;

    GdiFlush();
    for (y = dy; y < dy + dh; y++)                  /* the dialog itself is opaque */
        for (x = dx; x < dx + dw; x++)
            c->px[(size_t)y * c->cw + x] |= 0xFF000000u;
    bf.BlendOp = AC_SRC_OVER; bf.BlendFlags = 0; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    sz.cx = c->cw; sz.cy = c->ch;
    UpdateLayeredWindow(s_win, c->sdc, &c->o, &sz, c->mdc, &p0, 0, &bf, ULW_ALPHA);
    s_cw = c->cw; s_ch = c->ch; s_ox = c->o.x; s_oy = c->o.y;
    SelectObject(c->mdc, c->oldbm);
    DeleteObject(c->bm);
    DeleteDC(c->mdc);
    ReleaseDC(NULL, c->sdc);
}

static void fill(HDC dc, const RECT *r, COLORREF col)
{
    HBRUSH br = CreateSolidBrush(col);
    FillRect(dc, r, br);
    DeleteObject(br);
}

static HFONT font(int height, int weight, const char *face)
{
    return CreateFontA(-height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, face);
}

static void draw_settings(void)
{
    Canvas c;
    RECT rr, row;
    int rh, dw, dh, dx, dy, i;
    HFONT ft, fr, fh;
    HGDIOBJ oldf;

    if (!canvas_begin(&c))
        return;
    rh = c.ch * 7 / 100; if (rh < 22) rh = 22;
    dw = rh * 17;
    if (dw > c.cw * 95 / 100) { dw = c.cw * 95 / 100; rh = dw / 17; }
    dh = rh * (SET_ITEMS + 3) + rh * 3 / 10;
    if (dh > c.ch) dh = c.ch;
    dx = (c.cw - dw) / 2; dy = (c.ch - dh) / 2;
    SetRect(&rr, dx, dy, dx + dw, dy + dh);
    fill(c.mdc, &rr, RGB(255, 122, 0));
    InflateRect(&rr, -(dh / 40 + 2), -(dh / 40 + 2));
    fill(c.mdc, &rr, RGB(14, 22, 40));

    ft = font(rh * 80 / 100, FW_BOLD, "Impact");
    fr = font(rh * 56 / 100, FW_NORMAL, "Bahnschrift SemiCondensed");
    fh = font(rh * 38 / 100, FW_NORMAL, "Bahnschrift SemiCondensed");
    oldf = SelectObject(c.mdc, ft);
    SetTextColor(c.mdc, RGB(255, 255, 255));
    SetRect(&rr, dx, dy + rh / 4, dx + dw, dy + rh * 13 / 10);
    DrawTextA(c.mdc, "SETTINGS", -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(c.mdc, fr);
    for (i = 0; i < SET_ITEMS; i++) {
        int sel = i == s_sel, y = dy + rh * 14 / 10 + i * rh;
        char buf[96];
        SetRect(&row, dx + rh / 3, y, dx + dw - rh / 3, y + rh * 82 / 100);
        fill(c.mdc, &row, sel ? RGB(255, 122, 0) : ((i == IT_HIRES && !mvc2_pack_count()) || ((i == IT_SMOOTH || i == IT_SHARPEN) && !s_sprite)) ? RGB(28, 34, 46) : RGB(52, 60, 78));
        SetTextColor(c.mdc, sel ? RGB(10, 10, 14) : ((i == IT_HIRES && !mvc2_pack_count()) || ((i == IT_SMOOTH || i == IT_SHARPEN) && !s_sprite)) ? RGB(92, 100, 114) : RGB(200, 210, 225));
        rr = row; rr.left += rh / 3;
        DrawTextA(c.mdc, SET_LABEL[i], -1, &rr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        rr = row; rr.right -= rh / 3;
        if (set_count(i))
            snprintf(buf, sizeof buf, "<  %s  >", set_text(i));
        else if (i == IT_QUIT && s_armed)
            snprintf(buf, sizeof buf, "press again to quit");
        else if (i == IT_HIRES || i == IT_SMOOTH || i == IT_SHARPEN)
            snprintf(buf, sizeof buf, "%s", set_text(i));
        else
            buf[0] = 0;
        DrawTextA(c.mdc, buf, -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(c.mdc, fh);
    SetTextColor(c.mdc, RGB(150, 162, 182));
    SetRect(&rr, dx + rh / 3, dy + rh * 14 / 10 + SET_ITEMS * rh + rh / 6, dx + dw - rh / 3, dy + dh - rh / 8);
    DrawTextA(c.mdc, "Up/Down choose, Left/Right change, Enter or A confirms. Saved automatically. Render scale, sprite "
              "filters and hi-res textures apply when you close this. F1, Esc or B closes.", -1, &rr, DT_LEFT | DT_WORDBREAK);
    SelectObject(c.mdc, oldf);
    DeleteObject(ft); DeleteObject(fr); DeleteObject(fh);
    canvas_end(&c, dx, dy, dw, dh);
}

static void draw_quit(void)
{
    Canvas c;
    RECT dr, btn[2];
    int dw, dh, dx, dy, bw, bh, by, i;
    HFONT ft, fb;
    HGDIOBJ oldf;

    if (!canvas_begin(&c))
        return;
    dw = c.ch * 4 / 5; if (dw > c.cw * 9 / 10) dw = c.cw * 9 / 10;
    dh = dw * 9 / 20;
    dx = (c.cw - dw) / 2; dy = (c.ch - dh) / 2;
    SetRect(&dr, dx, dy, dx + dw, dy + dh);
    fill(c.mdc, &dr, RGB(255, 122, 0));
    InflateRect(&dr, -dh / 40 - 2, -dh / 40 - 2);
    fill(c.mdc, &dr, RGB(14, 22, 40));
    bw = dw * 3 / 10; bh = dh * 28 / 100; by = dy + dh * 58 / 100;
    SetRect(&btn[0], dx + dw / 2 - bw - dw / 25, by, dx + dw / 2 - dw / 25, by + bh);
    SetRect(&btn[1], dx + dw / 2 + dw / 25, by, dx + dw / 2 + dw / 25 + bw, by + bh);
    ft = font(dh * 17 / 100, FW_MEDIUM, "Bahnschrift SemiCondensed");
    fb = font(dh * 15 / 100, FW_MEDIUM, "Bahnschrift SemiCondensed");
    oldf = SelectObject(c.mdc, ft);
    SetTextColor(c.mdc, RGB(255, 255, 255));
    SetRect(&dr, dx, dy + dh * 12 / 100, dx + dw, dy + dh * 50 / 100);
    DrawTextA(c.mdc, "Do you want to quit?", -1, &dr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(c.mdc, fb);
    for (i = 0; i < 2; i++) {
        int on = s_quit_yes == i;
        fill(c.mdc, &btn[i], on ? RGB(255, 122, 0) : RGB(52, 60, 78));
        SetTextColor(c.mdc, on ? RGB(10, 10, 14) : RGB(200, 210, 225));
        DrawTextA(c.mdc, i ? "YES" : "NO", -1, &btn[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(c.mdc, oldf);
    DeleteObject(ft); DeleteObject(fb);
    canvas_end(&c, dx, dy, dw, dh);
}

static void draw(void)
{
    if (s_settings) draw_settings();
    else draw_quit();
}

/* ---- open and close ---------------------------------------------------------- */

static LRESULT CALLBACK win_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST) return HTTRANSPARENT;    /* keyboard and controller only: the window takes no mouse */
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcA(h, m, w, l);
}

static void open_overlay(int settings)
{
    if (!s_win) {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = win_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "Mvc2Overlay";
        RegisterClassA(&wc);
        s_win = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, wc.lpszClassName, "",
                                WS_POPUP, 0, 0, 1, 1, xbox_fb_window(), NULL, wc.hInstance, NULL);
        if (!s_win)
            return;
    }
    xbox_InputKeysClear();                          /* nothing the game saw as held stays held */
    xbox_input_block = 1;
    s_unblock_wait = 0; s_pad_valid = 0;
    s_settings = settings; s_sel = 0; s_armed = 0; s_quit_yes = 0;
    xbox_apu_hold = 1;
    xa2_set_muted(1);
    s_active = 1;                                   /* the next flip waits */
    draw();
    ShowWindow(s_win, SW_SHOWNOACTIVATE);
}

static void close_overlay(void)
{
    if (s_active) {
        s_active = 0;
        xbox_apu_hold = 0;
        xa2_set_muted(0);
        s_unblock_wait = 1;                         /* pads stay neutral until the buttons are let go */
        s_unblock_t = GetTickCount();
    }
    s_settings = 0; s_armed = 0;
    if (s_win) ShowWindow(s_win, SW_HIDE);
}

static void quit_game(void)
{
    extern void mvc2_diag_flush(void);
    mvc2_diag_flush();                              /* the log file gets the last lines first */
    TerminateProcess(GetCurrentProcess(), 0);
}

/* ---- keys -------------------------------------------------------------------- */

static void settings_change(int i, int dir)
{
    int n = set_count(i);
    if (n) {
        int v = (set_get(i) + dir + n) % n;
        set_put(i, v);
        s_armed = 0;
    } else if (dir == 0) {
        if (i == IT_CLOSE) { close_overlay(); return; }
        if (i == IT_QUIT) {
            if (s_armed) quit_game();
            s_armed = 1;
        }
    }
    draw();
}

static int key_hook(unsigned w, long l, int down)
{
    int repeat = (l & (1L << 30)) != 0;

    if (w == VK_F1) {
        if (down && !repeat) {
            if (!s_active) open_overlay(1);
            else if (s_settings) close_overlay();
        }
        return 1;
    }
    if (w == VK_F11 || (w == VK_RETURN && (l & (1L << 29))))
        return 0;                                   /* the window's own fullscreen keys; tick redraws */
    if (s_active && s_settings) {
        if (!down) return 1;
        if (w == VK_ESCAPE) { if (!repeat) close_overlay(); }
        else if (w == VK_UP || w == VK_DOWN) {
            do s_sel = (s_sel + (w == VK_UP ? SET_ITEMS - 1 : 1)) % SET_ITEMS;
            while ((s_sel == IT_HIRES && !mvc2_pack_count()) || ((s_sel == IT_SMOOTH || s_sel == IT_SHARPEN) && !s_sprite));   /* rows with nothing to change are skipped */
            s_armed = 0;
            draw();
        } else if (w == VK_LEFT) settings_change(s_sel, -1);
        else if (w == VK_RIGHT) settings_change(s_sel, +1);
        else if ((w == VK_RETURN || w == VK_SPACE) && !repeat)
            settings_change(s_sel, set_count(s_sel) ? +1 : 0);
        return 1;
    }
    if (w == VK_ESCAPE) {
        if (down && !repeat) {
            if (s_active) close_overlay();
            else open_overlay(0);
        }
        return 1;
    }
    if (!s_active)
        return 0;
    if (down && (w == VK_LEFT || w == VK_RIGHT || w == VK_TAB || w == 'A' || w == 'D')) {
        s_quit_yes = (w == VK_LEFT || w == 'A') ? 0 : (w == VK_RIGHT || w == 'D') ? 1 : !s_quit_yes;
        draw();
    } else if (down && !repeat && (w == VK_RETURN || w == VK_SPACE)) {
        if (s_quit_yes) quit_game();
        close_overlay();
    } else if (down && !repeat && w == 'Y') {
        quit_game();
    } else if (down && !repeat && w == 'N') {
        close_overlay();
    }
    return 1;
}

/* ---- controller 1 and the per-pass work ------------------------------------- */

static void pad_overlay_poll(void)
{
    static WORD prev;
    static DWORD rep;
    XINPUT_STATE st;
    WORD b, press, dirs;
    DWORD now = GetTickCount();

    if (XInputGetState(0, &st) != ERROR_SUCCESS) { prev = 0; s_pad_valid = 1; return; }
    b = st.Gamepad.wButtons;
    if (st.Gamepad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (st.Gamepad.sThumbLX > 16000)  b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (st.Gamepad.sThumbLY > 16000)  b |= XINPUT_GAMEPAD_DPAD_UP;
    if (st.Gamepad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (!s_pad_valid) { prev = b; s_pad_valid = 1; return; }   /* what was held when it opened is not a press */
    press = (WORD)(b & ~prev);
    dirs = (WORD)(b & 0x000F);
    if (press & XINPUT_GAMEPAD_A) key_hook(VK_RETURN, 0, 1);
    if (press & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) key_hook(VK_ESCAPE, 0, 1);
    if (s_active) {
        WORD fire = 0;
        if (press & 0x000F) { fire = (WORD)(press & 0x000F); rep = now + 350; }
        else if (dirs && now >= rep) { fire = dirs; rep = now + 110; }   /* held: repeat */
        if (fire & XINPUT_GAMEPAD_DPAD_UP) key_hook(VK_UP, 0, 1);
        else if (fire & XINPUT_GAMEPAD_DPAD_DOWN) key_hook(VK_DOWN, 0, 1);
        else if (fire & XINPUT_GAMEPAD_DPAD_LEFT) key_hook(VK_LEFT, 0, 1);
        else if (fire & XINPUT_GAMEPAD_DPAD_RIGHT) key_hook(VK_RIGHT, 0, 1);
    }
    prev = b;
}

static void tick(void)
{
    if (s_active) {
        HWND game = xbox_fb_window();
        RECT cr;
        POINT o = { 0, 0 };
        pad_overlay_poll();
        if (!s_active || !game || IsIconic(game))
            return;
        if (s_win && !IsWindowVisible(s_win)) {     /* hidden behind our back: show it again, on top */
            ShowWindow(s_win, SW_SHOWNOACTIVATE);
            SetWindowPos(s_win, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            s_cw = 0;
        }
        GetClientRect(game, &cr);
        ClientToScreen(game, &o);
        if (cr.right != s_cw || cr.bottom != s_ch || o.x != s_ox || o.y != s_oy)
            draw();
        return;
    }
    if (s_unblock_wait) {                           /* the press that closed it is not also a press in the game */
        XINPUT_STATE st;
        if (XInputGetState(0, &st) != ERROR_SUCCESS
            || (st.Gamepad.wButtons == 0 && st.Gamepad.sThumbLX > -16000 && st.Gamepad.sThumbLX < 16000
                && st.Gamepad.sThumbLY > -16000 && st.Gamepad.sThumbLY < 16000)
            || GetTickCount() - s_unblock_t > 2000) {
            xbox_input_block = 0;
            s_unblock_wait = 0;
            s_chord_held = 1;                       /* buttons still down from closing it do not open it again */
        }
    } else {                                        /* Back + Start, either one first: settings */
        XINPUT_STATE st;
        int chord = XInputGetState(0, &st) == ERROR_SUCCESS
                    && (st.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) && (st.Gamepad.wButtons & XINPUT_GAMEPAD_START);
        if (chord && !s_chord_held)
            open_overlay(1);
        s_chord_held = chord;
    }
}

/* The pushbuffer thread, at the end of every flip. */
/* The game is paused while a menu is up, so no new frame then is not a freeze (mvc2_diag.c). */
int mvc2_overlay_active(void) { return s_active; }

void mvc2_overlay_flip_wait(void)
{
    while (s_active)
        Sleep(5);
}

void mvc2_overlay_init(void)
{
    s_scale = cfg_get("resolution_scale", 1);
    if (s_scale < 1) s_scale = 1;
    if (s_scale > 3) s_scale = 3;
    s_stretch = cfg_get("stretch", 0) != 0;
    s_vsync = cfg_get("vsync", 1) != 0;
    if (getenv("RECOMP_PRESENT_VSYNC"))             /* a developer's explicit choice */
        s_vsync = getenv("RECOMP_PRESENT_VSYNC")[0] != '0';
    s_upscaling = cfg_get("upscaling", 0) != 0;
    gpu_present_set_upscale(s_upscaling);
    gpu_present_set_stretch(s_stretch);
    gpu_present_set_vsync(s_vsync);
    s_hires = cfg_get("hires_textures", 1) != 0;
    s_sprite = cfg_get("sprite_filter", 0);
    if (s_sprite < 0 || s_sprite > 3) s_sprite = 0;
    s_smooth = cfg_get("sprite_smoothing", 0);
    if (s_smooth < 0 || s_smooth > 2) s_smooth = 0;
    s_sharpen = cfg_get("sprite_sharpen", 0);
    if (s_sharpen < 0 || s_sharpen > 2) s_sharpen = 0;
    mvc2_video_request(s_scale, s_hires, s_sprite, s_smooth, s_sharpen);
    xbox_input_back_start = 1;
    xbox_fb_key_hook = key_hook;
    xbox_fb_tick_hook = tick;
}
