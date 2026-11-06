/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <windows.h>

/* ?????? ICALL trace ring buffer ??????????????????????????????????????????????????????????????????????????????????????????????????? */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ?????? Register state (defined in xbox_memory_layout.c) ???????????????????????? */

extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

/* ?????? Manual function overrides ????????????????????????????????????????????????????????????????????????????????????????????? */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
extern void sub_001FFFF0(void);
static void idle_thread_nospin(void)
{
    volatile uint32_t *flag = (volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x593BC8u);
    while (*flag == 0) Sleep(1);
    sub_001FFFF0();
}

/* ---- D3D KickOff (sub_002893C0): no spin on the one guest core ------------
 *
 * The XDK's KickOff sets bit 16 of NV2A 0x100410 (PFB flush) and spins until
 * it clears, THEN writes DMA_PUT. The generated code spins on the guest core
 * until the toolkit's pushbuffer thread notices; with every guest thread
 * pinned to one host core (RECOMP_GUEST_CPUS=one) that spin was 65% of the
 * guest main thread, ~14 kickoffs per fight frame, each waiting on the pushbuffer
 * thread's polling latency. Here the flush is acknowledged at once by the same
 * routine the pushbuffer thread uses (xbox_Nv2aAckBusyBits), and the pushbuffer
 * thread is woken by an event after DMA_PUT is written.
 * Same memory writes in the same order as the generated function otherwise.
 * OPT-IN via RECOMP_KICK_FAST=1 (60 FPS through menus, but deadlocks at fight load). */
extern void sub_00286960(void);
extern void xbox_Nv2aAckBusyBits(void);
extern void *g_nv2a_kick_event;
#define KM32(a) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(a) + g_xbox_mem_offset))
static void kickoff_fast(void)
{
    uint32_t self = g_ecx;
    uint32_t flags = KM32(self + 8);
    uint32_t put = (flags & 4u) ? KM32(self + 0x358) : KM32(self);
    g_esp -= 4; KM32(g_esp) = self;          /* the original pushes ecx; callees run above it */

    if (!(flags & 0x2000u)) {
        uint32_t regs = KM32(KM32(0x2930F8u) + 0x17C0u);
        volatile uint32_t *fl = &KM32(regs + 0x100410u);
        unsigned n = 0;
        *fl = *fl | 0x10000u;
        /* Clear just the flush bit. Calling xbox_Nv2aAckBusyBits() from a guest
         * thread also clears PCRTC/PMC vblank-pending bits the vblank DPC has not
         * consumed yet, which loses a vblank and stalls the title. */
        InterlockedAnd((volatile LONG *)fl, (LONG)~0x10000u);
        while (*fl & 0x10000u) {
            if (++n > 1000u) SwitchToThread();
            if (n % 200000u == 0) { fprintf(stderr, "[KICK] flush wait stuck n=%u self=%08X regs=%08X flag=%08X\n", n, self, regs, *fl); fflush(stderr); }
        }
        put &= 0xFFFFFFFu;
        KM32(KM32(self + 0x17BC) + 0x40u) = put;
        { extern void mvc2_kick_put(uint32_t, uint32_t); mvc2_kick_put(KM32(self + 0x17BC), put); }
        g_ecx = self; g_edx = put;
        if (g_nv2a_kick_event) SetEvent((HANDLE)g_nv2a_kick_event);
        g_eax = KM32(0x292C50u);
        if (g_eax != 0) {
            do {
                g_esp -= 4; KM32(g_esp) = 0x0028942Du;
                sub_00286960();
            } while (g_eax != 0);
            g_eax = self;
            KM32(self + 8) = KM32(self + 8) | 0x2000u;   /* only when the call loop ran (orig jumps to the ret otherwise) */
        }
    } else {
        uint32_t d = 0x292C40u - 0x40u;
        uint32_t v;
        KM32(self + 0x17BC) = d;
        v = KM32(self) & 0xFFFFFFFu;
        KM32(d + 0x40u) = v;
        KM32(KM32(self + 0x17BC) + 0x44u) = v;
        KM32(KM32(self + 0x30)) = KM32(self + 0x2C) - 2u;
        KM32(self + 0x197C) = KM32(self + 0x2010);
        KM32(self + 0x1FD8) = KM32(self + 0x3C);
        g_edx = KM32(self + 0x3C);
        g_eax = self;
    }
    g_ecx = self;
    g_esp += 8;   /* pop the saved ecx, then ret */
}

/* ---- Flip drain: the frame is drawn before the title starts the next one ----
 *
 * MvC2 writes its sprite tiles with the CPU into a few shared 128x128 P8 pages and draws them
 * straight away, with no fence: on the console the GPU finishes a 2D frame within about a
 * millisecond of the Swap's kickoff, long before the next frame's tiles are written. Our
 * executor needs several ms per frame (hundreds of D3D11 calls) while the recompiled CPU runs
 * faster than the console's, so the title rewrote pages the executor had not drawn yet: Ryu's
 * Shinku Hadoken body segments came out as copies of the beam's head ("tiled chevrons").
 * Called right after KickOff writes DMA_PUT (patch_gen.ps1 'mvc2_kick_put'): when the segment
 * just submitted holds a FLIP_STALL (the end of a frame), wait until the executor has consumed
 * it, i.e. DMA_GET == PUT. The KickOffs inside a frame do not wait.
 * RESULT (10-27): did NOT change the beam (DEL capture on the drained build still shows the "D" segments),
 * so the race is not the cause. Kept OPT-IN (RECOMP_FLIP_DRAIN=1) as a diagnostic: ~3.5 ms wait per frame. */
static int seg_has_flip(uint32_t a, uint32_t b)
{
    uint32_t p = a, n = 0;
    while (p != b && n++ < 400000u) {
        uint32_t w = KM32(0x80000000u | p), q = p;
        if ((w & 0xE0000003u) == 0x20000000u) { p = w & 0x1FFFFFFCu; continue; }    /* old-style jump */
        if ((w & 3u) == 1u) { p = w & 0x0FFFFFFCu; continue; }                         /* jump */
        if ((w & 3u) == 2u || (w & 0xFFFF0003u) == 0x00020000u) { p += 4; continue; } /* call / return: not followed */
        if ((w & 0x00030003u) == 0u) {
            uint32_t count = (w >> 18) & 0x7FFu, subch = (w >> 13) & 7u, method = w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;
            if (subch == 0 && count && (noninc ? method == 0x0130u : (method <= 0x0130u && 0x0130u < method + 4u * count)))
                return 1;                                                              /* NV097_FLIP_STALL */
            p += 4u + 4u * count;
        } else
            p += 4;
        p &= 0x0FFFFFFCu;
        if (q < b && p > b) break;                                                     /* ran past PUT: lost sync */
    }
    return 0;
}

void mvc2_kick_put(uint32_t blk, uint32_t put)
{
    static int on = -1;
    static uint32_t prev;
    static unsigned waits, timeouts;
    static unsigned long long wait_us;
    uint32_t p0 = prev;
    LARGE_INTEGER f, t0, t1;
    if (on < 0) { const char *e = getenv("RECOMP_FLIP_DRAIN"); on = e && e[0] == '1'; }
    prev = put;
    if (!on || !p0 || p0 == put || !seg_has_flip(p0, put))
        return;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    {
        extern void xbox_Nv2aKickHint(void);
        unsigned n = 0;
        xbox_Nv2aKickHint();
        while ((KM32(blk + 0x44u) & 0x0FFFFFFCu) != (put & 0x0FFFFFFCu)) {     /* DMA_GET */
            QueryPerformanceCounter(&t1);
            if ((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart > 250) { timeouts++; break; }
            if (++n < 2000u) SwitchToThread(); else Sleep(0);
        }
    }
    QueryPerformanceCounter(&t1);
    waits++; wait_us += (unsigned long long)((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
    if (waits % 1800u == 0)
        fprintf(stderr, "[FLIPDRAIN] %u frames, avg wait %llu us, timeouts %u\n", waits, wait_us / waits, timeouts);
}

/* ---- DrawVerticesUP probe (sub_002870F0): who draws what -------------------
 * While the RECOMP_FRAME_TRACE flag file exists (scripts/capture_key.ps1: DEL), log every call's guest call chain
 * (return address, then [ebp+4] up the frame-pointer chain) and its first vertices, so a sprite seen in the frame
 * trace (e.g. Ryu's beam tiles) can be tied to the game code that built it. patch_gen.ps1 'mvc2_dvup_probe'.
 * Called at function entry: [esp] = return address, [esp+4..] = PrimitiveType, VertexCount, pVertexData, Stride. */
extern __declspec(thread) uint32_t g_ebp;
void mvc2_dvup_probe(void)
{
    static const char *flag = (const char *)-1;
    static DWORD checked;
    static int on;
    static unsigned n;
    DWORD now;
    uint32_t esp = g_esp, ebp = g_ebp, prim, count, data, stride, i, k;
    if (flag == (const char *)-1) flag = getenv("RECOMP_FRAME_TRACE");
    if (!flag) return;
    now = GetTickCount();
    if (now - checked > 50) { checked = now; on = GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES; if (on) n = 0; }
    if (!on || ++n > 4000) return;
    prim = KM32(esp + 4); count = KM32(esp + 8); data = KM32(esp + 12); stride = KM32(esp + 16);
    fprintf(stderr, "[DVUP] ret %08X chain", KM32(esp));
    (void)ebp;
    /* No frame pointers (fpo): scan the stack for return addresses -- code addresses whose preceding bytes are a CALL. */
    for (k = 1, i = 0; k < 160 && i < 10; k++) {
        uint32_t a = KM32(esp + 4 * k);
        const uint8_t *p;
        if (a < 0x00011007u || a >= 0x002C7000u) continue;
        p = (const uint8_t *)((uintptr_t)a + g_xbox_mem_offset);
        if (p[-5] == 0xE8 || (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) || (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2)
                || (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) || (p[-7] == 0xFF && ((p[-6] >> 3) & 7) == 2)) {
            fprintf(stderr, " %08X@%X", a, 4 * k); i++;
        }
    }
    fprintf(stderr, " | prim %u n %u stride %u", prim, count, stride);
    for (i = 0; i < count && i < 4 && data; i++) {
        const uint32_t *w = (const uint32_t *)((uintptr_t)(data + i * stride) + g_xbox_mem_offset);
        float f[8]; memcpy(f, w, sizeof f);
        fprintf(stderr, " | v%u %.1f %.1f %g %g %08X %g %g", i, f[0], f[1], f[2], f[3], w[4], f[5], f[6]);
    }
    fputc('\n', stderr);
}

/* Sprite renderer probe (sub_001F6FD0, the cell layout behind every sprite tile): while the frame-trace flag exists, log its
 * first 8 stack arguments and return address, to see which sprite/frame the game asks for per beam segment.
 * patch_gen.ps1 'mvc2_spr_probe'. */
static uint32_t s_spr_frame;   /* frame argument of the sprite being laid out (for mvc2_tile_probe) */
static int s_spr_on;
void mvc2_spr_probe(void);
/* After the tile-cache lookup in sub_001F6FD0 (loc_001F7242): eax = slot (bit 31 = hit, no upload), [esp] = cell key.
 * patch_gen.ps1 'mvc2_tile_probe'. Logged while the sprite probe is active. */
void mvc2_tile_probe(uint32_t eax_)
{
    static unsigned n;
    if (!s_spr_on || ++n > 20000) return;
    fprintf(stderr, "[TILE] frame %X key %X slot %u %s\n", s_spr_frame, KM32(g_esp), eax_ & 0xFFFFu, (eax_ & 0x80000000u) ? "HIT" : "miss");
}
void mvc2_spr_probe(void)
{
    static const char *flag = (const char *)-1;
    static DWORD checked;
    static int on;
    static unsigned n;
    DWORD now;
    uint32_t esp = g_esp;
    if (flag == (const char *)-1) flag = getenv("RECOMP_FRAME_TRACE");
    if (!flag) return;
    now = GetTickCount();
    if (now - checked > 50) { checked = now; on = GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES; if (on) n = 0; }
    {   /* RECOMP_RAM_DUMP=<prefix>: on each rising edge of the DEL flag, write the 64 MB of guest physical RAM (contiguous
         * window 0x80000000) to <prefix>_<n>.ram -- same layout as xemu's pmemsave (scripts/xemu_ramdump.ps1). */
        static const char *rd = (const char *)-1;
        static int was, nd;
        if (rd == (const char *)-1) rd = getenv("RECOMP_RAM_DUMP");
        if (rd && on && !was) {
            char path[512];
            FILE *f;
            snprintf(path, sizeof path, "%s_%d.ram", rd, ++nd);
            if ((f = fopen(path, "wb")) != NULL) {
                fwrite((const void *)((uintptr_t)0x80000000u + g_xbox_mem_offset), 1, 64u << 20, f);
                fclose(f);
                fprintf(stderr, "[RAMDUMP] %s\n", path);
            }
            snprintf(path, sizeof path, "%s_%d.va", rd, nd);   /* game image + data by VA, as xemu's memsave 0x10000 0x800000 */
            if ((f = fopen(path, "wb")) != NULL) {
                fwrite((const void *)((uintptr_t)0x10000u + g_xbox_mem_offset), 1, 0x800000u, f);
                fclose(f);
            }
        }
        was = on;
    }
    s_spr_on = on; s_spr_frame = KM32(esp + 8);
    if (!on || ++n > 3000) return;
    fprintf(stderr, "[SPR] ret %08X args %08X %08X %08X %08X %08X %08X %08X %08X\n", KM32(esp),
            KM32(esp + 4), KM32(esp + 8), KM32(esp + 12), KM32(esp + 16), KM32(esp + 20), KM32(esp + 24), KM32(esp + 28), KM32(esp + 32));
}

/* Sprite object probe (sub_0004EDE0, draws one sprite object; [esp+4] = object): while the frame-trace flag exists, log the
 * object, its position (+0xF0/+0xF4), scale (+0xFC/+0x100), angle (+0xF8), frame (+0x154) and a few flags, in draw order.
 * patch_gen.ps1 'mvc2_obj_probe'. */
void mvc2_obj_probe(void)
{
    static const char *flag = (const char *)-1;
    static DWORD checked;
    static int on;
    static unsigned n;
    DWORD now;
    uint32_t o = KM32(g_esp + 4), k, i;
    float f[5];
    if (flag == (const char *)-1) flag = getenv("RECOMP_FRAME_TRACE");
    if (!flag || !o) return;
    now = GetTickCount();
    if (now - checked > 50) { checked = now; on = GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES; if (on) n = 0; }
    if (!on || ++n > 3000) return;
    for (i = 0; i < 5; i++) { uint32_t w = KM32(o + 0xF0 + 4 * i); memcpy(&f[i], &w, 4); }
    fprintf(stderr, "[OBJ] ret %08X obj %08X pos %.1f %.1f ang %g scale %g %g frame %08X f114 %08X f120 %08X b170 %02X pri31 %d layer24 %d",
            KM32(g_esp), o, f[0], f[1], f[2], f[3], f[4], KM32(o + 0x154), KM32(o + 0x114), KM32(o + 0x120), KM32(o + 0x170) & 0xFFu, (int)(int8_t)(KM32(o + 0x31) & 0xFF), (int)(int8_t)(KM32(o + 0x24) & 0xFF));
    for (k = 1, i = 0; k < 120 && i < 4; k++) {       /* callers: return addresses found on the stack */
        uint32_t a = KM32(g_esp + 4 * k);
        const uint8_t *p;
        if (a < 0x00011007u || a >= 0x002C7000u) continue;
        p = (const uint8_t *)((uintptr_t)a + g_xbox_mem_offset);
        if (p[-5] == 0xE8 || (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) || (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2)
                || (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2)) { fprintf(stderr, " %08X", a); i++; }
    }
    fputc('\n', stderr);
}

/* Object-list insert probe (sub_001E2830 = link at HEAD, sub_001E2890 = link at TAIL of list [esp+8]; object [esp+4]).
 * RECOMP_OBJ_SPAWNLOG=1: log every insert with the object's world position (+0x34/+0x38/+0x3C), to see in which order
 * and where the beam's segment objects are created. patch_gen.ps1 'mvc2_list_probe'. */
void mvc2_list_probe(int tail)
{
    static int on = -1;
    uint32_t o = KM32(g_esp + 4), lst = KM32(g_esp + 8) & 0xFF, w[3], i;
    float f[3];
    if (on < 0) on = getenv("RECOMP_OBJ_SPAWNLOG") != NULL;
    if (!on || !o) return;
    for (i = 0; i < 3; i++) { w[i] = KM32(o + 0x34 + 4 * i); memcpy(&f[i], &w[i], 4); }
    fprintf(stderr, "[LIST] t=%lu %s list %u obj %08X ret %08X pos %.1f %.1f %.1f layer %d id %02X%02X\n", GetTickCount(),
            tail ? "TAIL" : "HEAD", lst, o, KM32(g_esp), f[0], f[1], f[2], (int)(int8_t)(KM32(o + 0x24) & 0xFF),
            KM32(o) & 0xFF, (KM32(o) >> 8) & 0xFF);
}

/* Animation-setter probe (sub_0004F1A0(obj, anim, sub) / sub_0004F280(obj, anim, sub, start)): RECOMP_OBJ_SPAWNLOG=1 logs
 * every call with its arguments, caller and the frame it lands on. xemu shows Ryu's beam body segments on "cap frame + 1";
 * ours all stay on the cap frame. patch_gen.ps1 'mvc2_anim_probe'. */
void mvc2_anim_probe(int which)
{
    static int on = -1;
    uint32_t o = KM32(g_esp + 4);
    if (on < 0) on = getenv("RECOMP_OBJ_SPAWNLOG") != NULL;
    if (!on || !o) return;
    fprintf(stderr, "[ANIM] t=%lu %s obj %08X anim %u sub %u start %d ret %08X frame_before %08X layer %d x %.1f\n", GetTickCount(),
            which ? "set4" : "set3", o, KM32(g_esp + 8) & 0xFF, KM32(g_esp + 12) & 0xFF, which ? (int)KM32(g_esp + 16) : -1,
            KM32(g_esp), KM32(o + 0x154), (int)(int8_t)(KM32(o + 0x24) & 0xFF), *(volatile float *)((uintptr_t)(o + 0x34) + g_xbox_mem_offset));
}

/* Called from the top of the generated sub_002893C0 (direct calls bypass
 * recomp_lookup_manual, so the generated function is patched to ask here;
 * scripts/regen.ps1 re-applies that one-line patch after a regeneration). */
int recomp_kick_fast(void)
{
    static int mode = -1;
    if (mode < 0) mode = (getenv("RECOMP_KICK_FAST") && !getenv("RECOMP_KICK_ORIG") && !getenv("RECOMP_FLIP_PACING")) ? 1 : 0; /* opt-in: stalls at fight load (see HANDOFF) */
    if (!mode) { extern void xbox_Nv2aKickHint(void); xbox_Nv2aKickHint(); return 0; }
    kickoff_fast();
    return 1;
}

/* ---- jump-table dispatch thunks the disassembler never made functions ------
 *
 * `mov r,[esp+4]; movzx r2,byte [r+k]; mov [esp+..],..; jmp [r2*4+tbl]` thunks
 * right after a ret/jmp get folded into the function before them, so an
 * indirect call to one failed to resolve and was dropped. scripts/
 * scan_jt_thunks.py finds 10; config/seed_functions.json seeds 0x00022C30,
 * 0x001766E0 and 0x00197D70. The four below are written out by hand: same
 * register and stack writes, then the table target is tail-called the way
 * RECOMP_ITAIL does it. (0x000B74D0/0x000B7530/0x000B7590 have the same shape
 * but start with `pop ebx`: shared function tails, not call targets.) */
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
void recomp_icall_fail_log(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
#define KM8(a) (*(volatile uint8_t *)((uintptr_t)(uint32_t)(a) + g_xbox_mem_offset))
/* First call of each thunk, so a normal run shows whether they are ever reached ([JT] lines). */
static void jt_seen(uint32_t thunk, uint32_t target)
{
    static volatile LONG seen[4];
    int k = thunk == 0x000281A0u ? 0 : thunk == 0x00028AB0u ? 1 : thunk == 0x00065B2Bu ? 2 : 3;
    if (InterlockedExchange(&seen[k], 1) == 0)
        fprintf(stderr, "[JT] first call of thunk 0x%08X -> 0x%08X\n", thunk, target);
}
static void jt_tail(uint32_t va)
{
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    if (fn) fn();
    else { recomp_icall_fail_log(va); g_esp += 4; g_eax = 0; }
}
/* mov eax,[esp+4]; movzx edx,byte [eax+4]; mov [esp+4],eax; jmp [edx*4+tbl] */
#define JT_EAX4_EDX(name, va, tbl) static void name(void) { \
    uint32_t a = KM32(g_esp + 4), i = KM8(a + 4), t = KM32(i * 4u + (tbl)); \
    g_eax = a; g_edx = i; KM32(g_esp + 4) = a; jt_seen((va), t); jt_tail(t); }
JT_EAX4_EDX(jt_000281A0, 0x000281A0u, 0x002E6D84u)
JT_EAX4_EDX(jt_00028AB0, 0x00028AB0u, 0x002E6DA4u)
/* mov eax,[esp+4]; movzx edx,byte [eax+6]; mov [esp+8],ecx; mov [esp+4],eax; jmp [edx*4+0x2EFCF0] */
static void jt_00065B2B(void)
{
    uint32_t a = KM32(g_esp + 4), i = KM8(a + 6), t = KM32(i * 4u + 0x002EFCF0u);
    g_eax = a; g_edx = i; KM32(g_esp + 8) = g_ecx; KM32(g_esp + 4) = a;
    jt_seen(0x00065B2Bu, t); jt_tail(t);
}
/* mov ecx,[esp+4]; movzx edx,byte [ecx+7]; mov [esp+8],eax; mov [esp+4],ecx; jmp [edx*4+0x2F1310] */
static void jt_0007651E(void)
{
    uint32_t c = KM32(g_esp + 4), i = KM8(c + 7), t = KM32(i * 4u + 0x002F1310u);
    g_ecx = c; g_edx = i; KM32(g_esp + 8) = g_eax; KM32(g_esp + 4) = c;
    jt_seen(0x0007651Eu, t); jt_tail(t);
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    /* The idle thread (sub_001FFFF0) spins on a flag; on one pinned core that
     * spin steals ~half the core from the real guest threads. Sleep instead. */
    if (xbox_va == 0x001FFFF0u && !getenv("RECOMP_IDLE_SPIN")) return idle_thread_nospin;
    { static int no_jt = -1; if (no_jt < 0) no_jt = getenv("RECOMP_NO_JT") != NULL; if (no_jt) return (recomp_func_t)0; }
    switch (xbox_va) {
    case 0x000281A0u: return jt_000281A0;
    case 0x00028AB0u: return jt_00028AB0;
    case 0x00065B2Bu: return jt_00065B2B;
    case 0x0007651Eu: return jt_0007651E;
    }
    return (recomp_func_t)0;
}

/* ?????? ICALL failure logging ????????????????????????????????????????????????????????????????????????????????????????????????????????? */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ?????? Untranslated instructions ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ---- ring-buffer consistency diagnostics (sub_002011D0) ---- */
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
static uint32_t g_rl[256][14]; static unsigned g_rln; static volatile long g_rl_dumped;
void mvc2_ringlog(int tag, const uint32_t *v)
{
    unsigned k = (unsigned)InterlockedIncrement((volatile LONG*)&g_rln) - 1; uint32_t *e = g_rl[k & 255];
    e[0] = (uint32_t)tag; for (int i = 0; i < 12; i++) e[1+i] = v[i]; e[13] = GetCurrentThreadId();
    if (tag == 9 && InterlockedExchange(&g_rl_dumped, 1) == 0) {
        fprintf(stderr, "[RINGLOG] MISMATCH tag9 total=%u: cols tag obj tot14 A B base S ptr n tot12 esi esp cb tid\n", k+1);
        for (unsigned q = (k >= 40 ? k-40 : 0); q <= k; q++) { uint32_t *x = g_rl[q & 255];
            fprintf(stderr, "[RINGLOG] #%u t=%u obj=%08X tot10=%08X A=%08X B=%08X base=%08X S=%08X ptr=%08X n=%08X tot0C=%08X esi=%08X cb=%08X tid=%u ptroff=%08X\n", q, x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8], x[9], x[10], x[12], x[13], (x[7]-x[5])); }
        fflush(stderr);
    }
}

#define MVC2_G8(a) (*(volatile uint8_t *)((uint8_t *)g_xbox_mem_offset + (uint32_t)(a)))
extern ptrdiff_t g_xbox_mem_offset;
/* RECOMP_NO_CANCEL_GUARD=1 turns the P0 check off (A/B tests only). */
int mvc2_p0_off(void) { static int v = -1; if (v < 0) v = getenv("RECOMP_NO_CANCEL_GUARD") != NULL; return v; }
/* ---- P1 guards for the USB cancel DPC (sub_002C6908; see scripts/patch_gen.ps1) ---- */
void mvc2_cancel_notfound(uint32_t req, uint32_t ed)
{
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    if (k <= 20) {
        fprintf(stderr, "  [CANCELGUARD] req=%08X flags22=%02X ed=%08X ed_looks_real=%d: no TD for this request on the ED; cancel skipped (#%ld)\n", req, MVC2_G8(req + 0x22), ed, (ed >= 0x10000u && ed < 0x04000000u && !(ed & 15u)), (long)k);
        fflush(stderr);
    }
}
void mvc2_cancel_selflink(uint32_t req)
{
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    if (k <= 20) {
        fprintf(stderr, "  [CANCELGUARD] req=%08X flags22=%02X is linked to itself on the cancel list (same request queued twice); list ended (#%ld)\n", req, MVC2_G8(req + 0x22), (long)k);
        fflush(stderr);
    }
}

void mvc2_cancel_completed(uint32_t req)
{
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    if (k <= 20) {
        fprintf(stderr, "  [CANCELDONE] req=%08X already completed when XpCancelRequest ran; not queued on the cancel list (#%ld)\n", req, (long)k);
        fflush(stderr);
    }
}

void mvc2_cancel_seen(uint32_t req, uint32_t flags)
{
    static volatile LONG n;
    LONG k = InterlockedIncrement(&n);
    if (k <= 30) {
        fprintf(stderr, "  [XPCANCEL] #%ld req=%08X flags22=%02X\n", (long)k, req, flags);
        fflush(stderr);
    }
}

/* Diagnostic (patch_gen.ps1 -Hoplat): time from the controller publishing a done queue to the guest's completion routine (sub_002C685C) running. */
extern volatile ULONGLONG g_ohci_pub_tick;
void mvc2_done_hook(uint32_t td)
{
    static volatile LONG n;
    ULONGLONG d = GetTickCount64() - g_ohci_pub_tick;
    if (d >= 20 && InterlockedIncrement(&n) <= 40) {
        fprintf(stderr, "  [HOPLAT] td=%08X publish->guest completion %llu ms tid=%lu\n", td, (unsigned long long)d, (unsigned long)GetCurrentThreadId());
        fflush(stderr);
    }
}
