/* The keyboard actions the player can remap, in the order of xbox_key_map[] (the toolkit's keyboard.c).
 * Shared by the game (mvc2_keymap.c reads them) and config.exe (writes them).
 * X(id, label, default key, default alternate key); keys are Windows virtual-key codes, 0 = none.
 * They are kept in %LOCALAPPDATA%\MvC2Recomp\settings.ini:
 *     [keys]
 *     start=13        ; the key
 *     start_alt=0     ; an alternate key for the same action */
#ifndef MVC2_KEYMAP_H
#define MVC2_KEYMAP_H

#define MVC2_KEYMAP(X) \
    X(dpad_up,      "D-pad up",          0x26, 0)    \
    X(dpad_down,    "D-pad down",        0x28, 0)    \
    X(dpad_left,    "D-pad left",        0x25, 0)    \
    X(dpad_right,   "D-pad right",       0x27, 0)    \
    X(start,        "Start",             0x0D, 0)    \
    X(back,         "Back",              0x08, 0)    \
    X(lthumb,       "Left stick click",  0x10, 0)    \
    X(rthumb,       "Right stick click", 0x11, 0)    \
    X(a,            "A",                 'Z',  0)    \
    X(b,            "B",                 'X',  0)    \
    X(x,            "X",                 'C',  0)    \
    X(y,            "Y",                 'V',  0)    \
    X(white,        "White",             'Q',  0)    \
    X(black,        "Black",             'E',  0)    \
    X(ltrigger,     "Left trigger",      '1',  0)    \
    X(rtrigger,     "Right trigger",     '3',  0)    \
    X(lstick_up,    "Left stick up",     'W',  0x68) \
    X(lstick_down,  "Left stick down",   'S',  0x62) \
    X(lstick_left,  "Left stick left",   'A',  0x64) \
    X(lstick_right, "Left stick right",  'D',  0x66) \
    X(rstick_up,    "Right stick up",    'I',  0)    \
    X(rstick_down,  "Right stick down",  'K',  0)    \
    X(rstick_left,  "Right stick left",  'J',  0)    \
    X(rstick_right, "Right stick right", 'L',  0)

#define MVC2_KEYMAP_COUNT 24

#endif
