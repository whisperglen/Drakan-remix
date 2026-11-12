/* detour.c - minimal inline hook: overwrite the first `stolen` bytes of a function with
 * `jmp detour` and build a trampoline (copied bytes + `jmp target+stolen`). */
#include <string.h>
#include "hook.h"

void *detour_install(DWORD target, const BYTE *expect, int stolen, void *detour)
{
    BYTE *t = (BYTE *)target, *tramp;
    DWORD old;
    int i;

    if (stolen < 5 || memcmp(t, expect, stolen) != 0) {
        hook_log("[detour] 0x%08X: unexpected bytes, NOT hooked (", target);
        for (i = 0; i < stolen; i++) hook_log("%02X", t[i]);
        hook_log(")\n");
        return NULL;
    }
    tramp = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;
    memcpy(tramp, t, stolen);
    tramp[stolen] = 0xE9;                                            /* jmp back */
    *(DWORD *)(tramp + stolen + 1) = (target + stolen) - ((DWORD)tramp + stolen + 5);

    if (!VirtualProtect(t, stolen, PAGE_EXECUTE_READWRITE, &old)) return NULL;
    t[0] = 0xE9;                                                     /* jmp detour */
    *(DWORD *)(t + 1) = (DWORD)detour - (target + 5);
    for (i = 5; i < stolen; i++) t[i] = 0x90;                        /* nop the rest */
    VirtualProtect(t, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, stolen);
    hook_log("[detour] 0x%08X hooked (%d bytes), trampoline %p\n", target, stolen, tramp);
    return tramp;
}
