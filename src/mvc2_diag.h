/* mvc2_diag.h - what a player's problem report needs, in every build (mvc2_diag.c). */
#ifndef MVC2_DIAG_H
#define MVC2_DIAG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Player build: everything printed from here on also goes to <data_dir>\logs\mvc2-<date>-<time>.log (the last three
 * runs are kept), trimmed and with the Windows user name blanked out, and on to wherever it went before (RecompLauncher
 * captures it). Call before anything is printed. */
void mvc2_diag_log_start(const char *data_dir);

/* One line naming this build (version and symbol-file id) and the PC, for matching a report to its symbols. */
void mvc2_diag_build_line(void);

/* The freeze report: when no frame has been presented for freeze_secs (and no menu is open), every thread's call stack
 * and the streaming worker's state go to the log, once at freeze_secs and once 20 s later. Costs one wake-up a second.
 * verbose adds the long Direct3D and pushbuffer dumps (developer builds, RECOMP_STALL_DUMP) and repeats them. */
void mvc2_diag_start(unsigned freeze_secs, int verbose);

/* "mvc2_recomp+0x1A2B3C", or another module's name and offset, plus the symbol when a .pdb is beside the exe. */
void mvc2_diag_where(const void *addr, char *out, size_t n);

/* A fatal exception that nothing handled: the code, where, and the call stack, then the log is written out. */
void mvc2_diag_install_crash_filter(void);

/* Push everything printed so far into the log before the process ends. */
void mvc2_diag_flush(void);

#ifdef __cplusplus
}
#endif

#endif
