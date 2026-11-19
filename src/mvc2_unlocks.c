/* mvc2_unlocks.c - "Unlock everything": every character, extra color, stage and gallery artwork, as if bought in the
 * Secret Factor shop, without changing the player's save.
 *
 * The switch is settings.ini [game] unlock_all (config.exe, the F1 menu, RecompLauncher's settings). It is read once at
 * start; a change takes effect the next time the game starts, and the F1 menu says so.
 *
 * Where the game keeps them (found by buying each kind in the shop and comparing memory):
 *   characters      64 bits at [0x6555A0]+0x30 (bit per character; the 24 starting ones are 0x00FFFF77 / 0x02000004)
 *   extra colors    64 bits at [0x6555A0]+0x38, same layout
 *   gallery         64 bits at 0x676444, same layout
 *   stages          a count at 0x5F91AA, 0..7
 * "All" is 0xF8FFFFFF / 0x07FFFFFF (56 characters), the value character select (sub_0016E2B0) compares against: with
 * characters and colors complete it allows the same character three times on one team by itself.
 *
 * The game's own hooks (scripts/patch_gen.ps1): mvc2_unlocks_after_load runs after the defaults are set and after the
 * save is loaded, keeps what the player really has, and switches everything on; mvc2_unlocks_before_save puts the
 * player's own values back into the save buffer before the game computes its checksums, so the file never records the
 * switch and turning it off returns to the player's own progress. Points are never touched. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

#define G8(a)  (*(volatile uint8_t *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(a)))
#define G32(a) (*(volatile uint32_t *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(a)))

#define SYS_PTR      0x6555A0u      /* -> the game's system data (unlock masks at +0x30) */
#define GALLERY      0x676444u
#define STAGES       0x5F91AAu
#define ALL_LO       0xF8FFFFFFu
#define ALL_HI       0x07FFFFFFu
#define ALL_STAGES   7
/* The save buffer (sub_001E06B0 copies the live values here, then checksums it). */
#define SAVE_MASKS   0x33B8ACu      /* +0x30..+0x3C */
#define SAVE_STAGES  0x33B8BCu
#define SAVE_GALLERY 0x33B8DCu

static int s_active = -1;           /* the setting as the game started */
static int s_have_real;
static uint32_t s_masks[4], s_gallery[2];
static uint8_t s_stages;

static int ini_path(char *out, size_t n)
{
    char base[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof base) || strlen(base) > MAX_PATH - 40)
        return 0;
    snprintf(out, n, "%s\\MvC2Recomp", base);
    CreateDirectoryA(out, NULL);
    snprintf(out, n, "%s\\MvC2Recomp\\settings.ini", base);
    return 1;
}

/* The saved choice (what the next start will use). */
int mvc2_unlocks_setting(void)
{
    char path[MAX_PATH];
    return ini_path(path, sizeof path) && GetPrivateProfileIntA("game", "unlock_all", 0, path) != 0;
}

void mvc2_unlocks_set(int on)
{
    char path[MAX_PATH];
    if (ini_path(path, sizeof path))
        WritePrivateProfileStringA("game", "unlock_all", on ? "1" : "0", path);
}

/* What this run uses. */
int mvc2_unlocks_active(void)
{
    if (s_active < 0)
        s_active = mvc2_unlocks_setting();
    return s_active;
}

void mvc2_unlocks_init(void)
{
    fprintf(stderr, "[UNLOCKS] unlock everything: %s\n", mvc2_unlocks_active() ? "on" : "off");
}

void mvc2_unlocks_after_load(void)
{
    uint32_t sys;

    if (!mvc2_unlocks_active() || !g_xbox_mem_offset)
        return;
    sys = G32(SYS_PTR);
    if (!sys)
        return;
    /* What the player really has, as just set or loaded (anything switched on by us was overwritten by the load). */
    s_masks[0] = G32(sys + 0x30); s_masks[1] = G32(sys + 0x34);
    s_masks[2] = G32(sys + 0x38); s_masks[3] = G32(sys + 0x3C);
    s_gallery[0] = G32(GALLERY); s_gallery[1] = G32(GALLERY + 4);
    s_stages = G8(STAGES);
    s_have_real = 1;

    G32(sys + 0x30) = ALL_LO; G32(sys + 0x34) = ALL_HI;
    G32(sys + 0x38) = ALL_LO; G32(sys + 0x3C) = ALL_HI;
    G32(GALLERY) = ALL_LO; G32(GALLERY + 4) = ALL_HI;
    G8(STAGES) = ALL_STAGES;
    fprintf(stderr, "[UNLOCKS] everything unlocked for this run (own: characters %08X %08X, colors %08X %08X, "
                    "gallery %08X %08X, stages %u)\n", s_masks[0], s_masks[1], s_masks[2], s_masks[3], s_gallery[0],
            s_gallery[1], s_stages);
}

void mvc2_unlocks_before_save(void)
{
    if (!mvc2_unlocks_active() || !s_have_real)
        return;
    G32(SAVE_MASKS) = s_masks[0]; G32(SAVE_MASKS + 4) = s_masks[1];
    G32(SAVE_MASKS + 8) = s_masks[2]; G32(SAVE_MASKS + 12) = s_masks[3];
    G32(SAVE_GALLERY) = s_gallery[0]; G32(SAVE_GALLERY + 4) = s_gallery[1];
    G8(SAVE_STAGES) = s_stages;
}
