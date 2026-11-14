/*
 * surfspy.c - who touches DirectDraw surfaces outside the 3D device? (R.10, REMIX.md section 4 q4)
 *
 * Drakan wraps every IDirectDrawSurface4 in a small C++ object (vtable 0x479B00, the surface pointer at +8,
 * created by IDirectDraw4 wrapper slot 0x439410 -> CreateSurface). Pixel access and 2D copies go through
 * three of its methods, which we hook (log only, the original always runs):
 *   0x439850 Blt (dstRect, srcWrapper, srcRect, flags, fx)  ret 0x14  -> IDirectDrawSurface4::Blt  (+0x14)
 *   0x439930 Lock(rect, desc, flags, event)                  ret 0x10  -> IDirectDrawSurface4::Lock (+0x64)
 *   0x4398E0 Flip(targetWrapper, flags)                      ret 8     -> IDirectDrawSurface4::Flip (+0x2C)
 * Each distinct caller (return address) is logged the first time, then again at 10, 100, 1000... calls,
 * with the surface size, bit depth and caps, so we can tell textures, the back buffer and the primary apart.
 * Direct COM calls that bypass the wrapper (e.g. texture upload 0x43E390) are not seen here.
 */
#define CINTERFACE
#include <stdio.h>
#include <string.h>
#include <intrin.h>
#include <ddraw.h>
#include "hook.h"

#define A_SBLT   0x00439850
#define A_SLOCK  0x00439930
#define A_SFLIP  0x004398E0

typedef HRESULT (__fastcall *sblt_fn)(void *w, void *edx, RECT *dr, void *src, RECT *sr, DWORD fl, void *fx);
typedef HRESULT (__fastcall *slock_fn)(void *w, void *edx, RECT *r, DDSURFACEDESC2 *d, DWORD fl, HANDLE ev);
typedef HRESULT (__fastcall *sflip_fn)(void *w, void *edx, void *target, DWORD fl);
static sblt_fn O_sblt; static slock_fn O_slock; static sflip_fn O_sflip;

#define MAXC 128
static struct { DWORD ret; char op; DWORD n; } g_c[MAXC];
static int g_nc;
static DWORD g_flips;

/* returns the new count for (op, caller), 0 if the table is full */
static DWORD bump(char op, DWORD ret)
{
    int i;
    for (i = 0; i < g_nc; i++)
        if (g_c[i].ret == ret && g_c[i].op == op) return ++g_c[i].n;
    if (g_nc >= MAXC) return 0;
    g_c[g_nc].ret = ret; g_c[g_nc].op = op; g_c[g_nc].n = 1;
    return g_c[g_nc++].n;
}

static int worth_logging(DWORD n)
{
    return n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000;
}

static void desc_of(void *wrapper, DDSURFACEDESC2 *sd)
{
    IDirectDrawSurface4 *s;
    memset(sd, 0, sizeof(*sd)); sd->dwSize = sizeof(*sd);
    __try {
        s = wrapper ? *(IDirectDrawSurface4 **)((BYTE *)wrapper + 8) : NULL;
        if (s) s->lpVtbl->GetSurfaceDesc(s, sd);
    } __except (EXCEPTION_EXECUTE_HANDLER) { sd->dwWidth = 0; }
}

static void log_surf(const char *what, DWORD n, DWORD ret, void *wrapper, const DDSURFACEDESC2 *sd, const char *extra)
{
    hook_log("[surf] %-4s caller %08X  #%-6u (flip %u)  surface %p %ux%u %ubpp caps %08X%s%s%s%s%s\n",
             what, ret, n, g_flips, wrapper, sd->dwWidth, sd->dwHeight, sd->ddpfPixelFormat.dwRGBBitCount,
             sd->ddsCaps.dwCaps,
             (sd->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) ? " PRIMARY" : "",
             (sd->ddsCaps.dwCaps & DDSCAPS_BACKBUFFER) ? " BACKBUFFER" : "",
             (sd->ddsCaps.dwCaps & DDSCAPS_TEXTURE) ? " TEXTURE" : "",
             (sd->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ? " SYSMEM" : "", extra);
    if (n == 1) hook_log_flush();
}

static HRESULT __fastcall H_sblt(void *w, void *edx, RECT *dr, void *src, RECT *sr, DWORD fl, void *fx)
{
    DWORD ret = (DWORD)_ReturnAddress(), n = bump('B', ret);
    if (worth_logging(n)) {
        DDSURFACEDESC2 sd, ss; char extra[160];
        desc_of(w, &sd); desc_of(src, &ss);
        _snprintf(extra, sizeof(extra), "  flags %08X dst %ld,%ld-%ld,%ld  src %p %ux%u caps %08X", fl,
                  dr ? dr->left : -1, dr ? dr->top : -1, dr ? dr->right : -1, dr ? dr->bottom : -1,
                  src, ss.dwWidth, ss.dwHeight, ss.ddsCaps.dwCaps);
        extra[sizeof(extra) - 1] = 0;
        log_surf("Blt", n, ret, w, &sd, extra);
    }
    return O_sblt(w, edx, dr, src, sr, fl, fx);
}

static HRESULT __fastcall H_slock(void *w, void *edx, RECT *r, DDSURFACEDESC2 *d, DWORD fl, HANDLE ev)
{
    DWORD ret = (DWORD)_ReturnAddress(), n = bump('L', ret);
    HRESULT hr = O_slock(w, edx, r, d, fl, ev);
    if (worth_logging(n)) {
        DDSURFACEDESC2 sd; char extra[64];
        desc_of(w, &sd);
        _snprintf(extra, sizeof(extra), "  flags %08X hr %08X", fl, (DWORD)hr); extra[sizeof(extra) - 1] = 0;
        log_surf("Lock", n, ret, w, &sd, extra);
    }
    return hr;
}

static HRESULT __fastcall H_sflip(void *w, void *edx, void *target, DWORD fl)
{
    DWORD ret = (DWORD)_ReturnAddress(), n = bump('F', ret);
    g_flips++;
    if (worth_logging(n)) {
        DDSURFACEDESC2 sd; char extra[48];
        desc_of(w, &sd);
        _snprintf(extra, sizeof(extra), "  flags %08X", fl); extra[sizeof(extra) - 1] = 0;
        log_surf("Flip", n, ret, w, &sd, extra);
    }
    return O_sflip(w, edx, target, fl);
}

int surfspy_install(void)
{
    static const BYTE b_blt[]  = { 0x8B, 0x54, 0x24, 0x08, 0x85, 0xD2 };        /* mov edx,[esp+8]; test edx,edx */
    static const BYTE b_lock[] = { 0x8B, 0x54, 0x24, 0x10, 0x8B, 0x41, 0x08 };  /* mov edx,[esp+10]; mov eax,[ecx+8] */
    static const BYTE b_flip[] = { 0x8B, 0x54, 0x24, 0x04, 0x85, 0xD2 };        /* mov edx,[esp+4]; test edx,edx */
    int n = 0;
    if (!hook_ini_int("surfspy", "enable", 1)) { hook_log("[surf] disabled\n"); return 0; }
    if ((O_sblt  = (sblt_fn)detour_install(A_SBLT, b_blt, sizeof(b_blt), H_sblt))) n++;
    if ((O_slock = (slock_fn)detour_install(A_SLOCK, b_lock, sizeof(b_lock), H_slock))) n++;
    if ((O_sflip = (sflip_fn)detour_install(A_SFLIP, b_flip, sizeof(b_flip), H_sflip))) n++;
    hook_log("[surf] %d of 3 surface hooks installed (Blt 0x439850, Lock 0x439930, Flip 0x4398E0)\n", n);
    return n;
}
