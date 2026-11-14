#pragma once
#include <windows.h>

/* main.c */
HMODULE hook_self_module(void);
void    hook_chain_path(char *out, DWORD size);
void    hook_log(const char *fmt, ...);
void    hook_log_flush(void);
int     hook_ini_int(const char *section, const char *key, int def);

/* detour.c: 5+ byte jmp patch with a trampoline. `stolen` must be the exact length of
 * whole instructions at the target (checked against `expect`), and those instructions
 * must not be relative branches. Returns the trampoline (call it to run the original)
 * or NULL if the target bytes don't match. */
void   *detour_install(DWORD target, const BYTE *expect, int stolen, void *detour);

/* drakan.c */
int     drakan_install_hooks(void);   /* returns number of hooks installed */

/* texdump.c: write world+0x2838 textures as DDS + manifest.txt into dir */
void    texdump_world(BYTE *world, const char *dir);

/* freecam.c (called from drakan_install_hooks after the host check) */
int     freecam_install(void);
