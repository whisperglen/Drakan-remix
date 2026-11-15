/*
 * proxy.c - ddraw.dll export forwarding (chain-loading).
 *
 * Every export is a naked stub that makes sure the chained DirectDraw DLL is loaded,
 * then jumps to the same-named export there. The chain target comes from
 * drakan_hook.ini ([chain] ddraw=...), relative to the game folder; by default the
 * system ddraw.dll. Arguments/stack are untouched, so any calling convention works.
 */
#include <windows.h>
#include "hook.h"

/* Keep in sync with ddraw.def: only exports common to dgVoodoo2 and Windows ddraw. */
#define EXPORTS(X) \
    X(AcquireDDThreadLock) X(CompleteCreateSysmemSurface) X(D3DParseUnknownCommand) \
    X(DDInternalLock) X(DDInternalUnlock) \
    X(DirectDrawCreate) X(DirectDrawCreateClipper) X(DirectDrawCreateEx) \
    X(DirectDrawEnumerateA) X(DirectDrawEnumerateExA) X(DirectDrawEnumerateExW) \
    X(DirectDrawEnumerateW) X(DllCanUnloadNow) X(DllGetClassObject) X(ReleaseDDThreadLock)

enum {
#define X(n) E_##n,
    EXPORTS(X)
#undef X
    E_COUNT
};
static const char *g_names[E_COUNT] = {
#define X(n) #n,
    EXPORTS(X)
#undef X
};
static FARPROC g_procs[E_COUNT];
static volatile LONG g_loaded;
static HMODULE g_chain;

/* Missing export in the chained DLL: fail the call instead of crashing.
 * (Only reached for exports Drakan never uses.) */
static HRESULT WINAPI missing_export(void) { return E_NOTIMPL; }

static void __stdcall ensure_chain(void)
{
    char path[MAX_PATH];
    int i;
    if (g_loaded) return;
    hook_chain_path(path, sizeof(path));
    g_chain = LoadLibraryA(path);
    if (g_chain == hook_self_module()) {          /* never chain to ourselves */
        FreeLibrary(g_chain);
        g_chain = NULL;
    }
    hook_log("[proxy] chain ddraw = %s -> %s\n", path, g_chain ? "loaded" : "FAILED");
    for (i = 0; i < E_COUNT; i++) {
        FARPROC p = g_chain ? GetProcAddress(g_chain, g_names[i]) : NULL;
        if (!p) hook_log("[proxy] chained ddraw lacks %s (calls will return E_NOTIMPL)\n", g_names[i]);
        g_procs[i] = p ? p : (FARPROC)missing_export;
    }
    hook_log_flush();
    InterlockedExchange(&g_loaded, 1);
}

/* DirectDrawCreate is a real function, so the M0 census (comcensus.c) can see the object it returns.
 * Every other export is a naked forwarding stub. */
#define X(n) \
    __declspec(naked) void s_##n(void) { \
        __asm pushad \
        __asm call ensure_chain \
        __asm popad \
        __asm jmp dword ptr [g_procs + E_##n * 4] \
    }
X(AcquireDDThreadLock) X(CompleteCreateSysmemSurface) X(D3DParseUnknownCommand)
X(DDInternalLock) X(DDInternalUnlock) X(DirectDrawCreateClipper) X(DirectDrawCreateEx)
X(DirectDrawEnumerateA) X(DirectDrawEnumerateExA) X(DirectDrawEnumerateExW)
X(DirectDrawEnumerateW) X(DllCanUnloadNow) X(DllGetClassObject) X(ReleaseDDThreadLock)
#undef X

HRESULT WINAPI s_DirectDrawCreate(GUID *guid, void **out, void *outer)
{
    typedef HRESULT (WINAPI *ddc_fn)(GUID *, void **, void *);
    HRESULT hr;
    ensure_chain();
    hr = ((ddc_fn)g_procs[E_DirectDrawCreate])(guid, out, outer);
    hook_log("[proxy] DirectDrawCreate -> hr %08X %p\n", (DWORD)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) census_on_directdraw(*out);
    return hr;
}
