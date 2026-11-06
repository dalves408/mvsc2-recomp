/* mvc2_debug.h - MvC2 glue for the xrdbg debug kit (tools/xrdbg). Only compiled into the debug flavour
 * (cmake -DMVC2_DEBUGKIT=ON, which defines XRDBG); in a normal build both macros are empty. */
#ifndef MVC2_DEBUG_H
#define MVC2_DEBUG_H

#ifdef XRDBG
void mvc2_debug_init(void);        /* first thing in WinMain: log, crash/hang capture, banner */
#include <stdint.h>
/* once per presented frame (gpu_backend.c): DEL key, due screenshots of the frame at guest VA va (bpp 4: X8R8G8B8, 2: R5G6B5) */
void mvc2_debug_on_flip(uint32_t va, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp);
#define MVC2_DEBUG_INIT()     mvc2_debug_init()
#define MVC2_DEBUG_ON_FLIP(va, w, h, pitch, bpp)  mvc2_debug_on_flip(va, w, h, pitch, bpp)
#else
#define MVC2_DEBUG_INIT()     ((void)0)
#define MVC2_DEBUG_ON_FLIP(va, w, h, pitch, bpp)  ((void)0)
#endif

#endif
