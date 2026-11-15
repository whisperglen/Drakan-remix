/*
 * comcensus.c - M0 "COM census" (REMIX.md 5.6): which DirectDraw / Direct3D methods does Drakan really call?
 *
 * Log only, no behaviour change. When DirectDrawCreate returns, we patch the method table (vtable) of the real
 * object (dgVoodoo or the system ddraw) in place. Every later object the game obtains through it (QueryInterface,
 * CreateSurface, GetAttachedSurface, CreateDevice, CreateViewport, ...) has its vtable patched the same way.
 *
 * - Every slot gets a 10-byte stub: "mov eax, id; jmp census_common". census_common counts the call per
 *   (vtable, slot, calling module) and jumps to the original method with the stack untouched.
 * - Selected slots get a typed hook instead, which also logs the arguments (surface descriptions, display
 *   modes, texture formats, render states, primitive types, lock/blt flags...) the first time each
 *   distinct value is seen.
 * - The first call of every method from every module is logged at once ("[com] first ..."). A full
 *   table of counts is logged at every level load and at exit (census_report).
 *
 * Patching is per vtable, not per kind: two classes implementing the same interface have different
 * vtables and different originals, so the originals are stored per patched vtable.
 * Calls made by the DirectDraw implementation itself through a patched table are counted under that
 * DLL's name, so the game's own calls are the "Drakan.exe" / "Dragon.rfl" / "dinput.dll" columns.
 */
#define CINTERFACE
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <intrin.h>
#include <ddraw.h>
#include <d3d.h>
#include "hook.h"

/* ---- interface kinds and method names ---------------------------------------------------------- */
enum { K_DD, K_DD2, K_DD4, K_SURF1, K_SURF4, K_D3D3, K_DEV3, K_VP3, K_TEX2, K_MAT3, K_LIGHT, K_PAL, K_CLIP, K_COUNT };

static const char *const m_dd[] = { "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette",
    "CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps",
    "GetDisplayMode", "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine",
    "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode",
    "WaitForVerticalBlank", "GetAvailableVidMem", "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel",
    "GetDeviceIdentifier" };
static const char *const m_surf[] = { "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect",
    "Blt", "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip",
    "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus",
    "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
    "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock",
    "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder", "GetDDInterface", "PageLock", "PageUnlock",
    "SetSurfaceDesc", "SetPrivateData", "GetPrivateData", "FreePrivateData", "GetUniquenessValue",
    "ChangeUniquenessValue" };
static const char *const m_d3d3[] = { "QueryInterface", "AddRef", "Release", "EnumDevices", "CreateLight",
    "CreateMaterial", "CreateViewport", "FindDevice", "CreateDevice", "CreateVertexBuffer", "EnumZBufferFormats",
    "EvictManagedTextures" };
static const char *const m_dev3[] = { "QueryInterface", "AddRef", "Release", "GetCaps", "GetStats", "AddViewport",
    "DeleteViewport", "NextViewport", "EnumTextureFormats", "BeginScene", "EndScene", "GetDirect3D",
    "SetCurrentViewport", "GetCurrentViewport", "SetRenderTarget", "GetRenderTarget", "Begin", "BeginIndexed",
    "Vertex", "Index", "End", "GetRenderState", "SetRenderState", "GetLightState", "SetLightState", "SetTransform",
    "GetTransform", "MultiplyTransform", "DrawPrimitive", "DrawIndexedPrimitive", "SetClipStatus", "GetClipStatus",
    "DrawPrimitiveStrided", "DrawIndexedPrimitiveStrided", "DrawPrimitiveVB", "DrawIndexedPrimitiveVB",
    "ComputeSphereVisibility", "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState",
    "ValidateDevice" };
static const char *const m_vp3[] = { "QueryInterface", "AddRef", "Release", "Initialize", "GetViewport", "SetViewport",
    "TransformVertices", "LightElements", "SetBackground", "GetBackground", "SetBackgroundDepth",
    "GetBackgroundDepth", "Clear", "AddLight", "DeleteLight", "NextLight", "GetViewport2", "SetViewport2",
    "SetBackgroundDepth2", "GetBackgroundDepth2", "Clear2" };
static const char *const m_tex2[] = { "QueryInterface", "AddRef", "Release", "GetHandle", "PaletteChanged", "Load" };
static const char *const m_mat3[] = { "QueryInterface", "AddRef", "Release", "SetMaterial", "GetMaterial", "GetHandle" };
static const char *const m_light[] = { "QueryInterface", "AddRef", "Release", "Initialize", "SetLight", "GetLight" };
static const char *const m_pal[] = { "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize",
    "SetEntries" };
static const char *const m_clip[] = { "QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd", "Initialize",
    "IsClipListChanged", "SetClipList", "SetHWnd" };

static const struct { const char *name; const char *const *m; int slots; } K[K_COUNT] = {
    { "IDirectDraw", m_dd, 23 },          { "IDirectDraw2", m_dd, 24 },        { "IDirectDraw4", m_dd, 28 },
    { "IDirectDrawSurface", m_surf, 36 }, { "IDirectDrawSurface4", m_surf, 45 },
    { "IDirect3D3", m_d3d3, 12 },         { "IDirect3DDevice3", m_dev3, 42 },  { "IDirect3DViewport3", m_vp3, 21 },
    { "IDirect3DTexture2", m_tex2, 6 },   { "IDirect3DMaterial3", m_mat3, 6 }, { "IDirect3DLight", m_light, 6 },
    { "IDirectDrawPalette", m_pal, 7 },   { "IDirectDrawClipper", m_clip, 9 },
};

/* ---- patched vtables, originals, counters --------------------------------------------------------- */
#define MAXVT   48
#define SLOTS   64
#define MAXMOD  12
static void  **g_vt[MAXVT];
static int     g_vtkind[MAXVT];
static int     g_nvt;
static void   *g_orig[MAXVT * SLOTS];          /* referenced from census_common */
static DWORD   g_count[MAXVT * SLOTS][MAXMOD];
static HMODULE g_mod[MAXMOD];
static char    g_modname[MAXMOD][32];
static int     g_nmod;
static BYTE   *g_stubs;
static int     g_on;

static int vt_index(void *obj)
{
    void **vt = obj ? *(void ***)obj : NULL;
    int i;
    for (i = 0; i < g_nvt; i++) if (g_vt[i] == vt) return i;
    return -1;
}

static void *orig_of(void *obj, int slot) { int v = vt_index(obj); return v < 0 ? NULL : g_orig[v * SLOTS + slot]; }

static int module_index(DWORD addr)
{
    HMODULE h = NULL;
    int i;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)addr, &h);
    for (i = 0; i < g_nmod; i++) if (g_mod[i] == h) return i;
    if (g_nmod >= MAXMOD) return MAXMOD - 1;
    g_mod[g_nmod] = h;
    if (!h) strcpy(g_modname[g_nmod], "?");
    else if (h == hook_self_module()) strcpy(g_modname[g_nmod], "drakan_hook");   /* ours, also named ddraw.dll */
    else {
        char path[MAX_PATH], *s;
        GetModuleFileNameA(h, path, sizeof(path));
        s = strrchr(path, '\\');
        strncpy(g_modname[g_nmod], s ? s + 1 : path, 31); g_modname[g_nmod][31] = 0;
    }
    return g_nmod++;
}

static void __stdcall census_note(DWORD id, DWORD ret)
{
    int m = module_index(ret), v = id / SLOTS, s = id % SLOTS;
    if (++g_count[id][m] == 1) {
        HMODULE h = g_mod[m];
        hook_log("[com] first %s::%s (vt%d) from %s+0x%X\n", K[g_vtkind[v]].name, K[g_vtkind[v]].m[s], v,
                 g_modname[m], h ? ret - (DWORD)h : ret);
    }
}

static void note(void *obj, int slot, void *ret)
{
    int v = vt_index(obj);
    if (v >= 0) census_note(v * SLOTS + slot, (DWORD)ret);
}

__declspec(naked) static void census_common(void)
{
    __asm {
        pushad
        push dword ptr [esp + 32]       /* caller's return address */
        push eax                        /* id = vtable index * 64 + slot */
        call census_note                /* stdcall, pops both */
        popad
        jmp dword ptr [g_orig + eax * 4]
    }
}

/* distinct-value filter: returns 1 the first time (tag, a, b, c) is seen; at most `cap` per tag */
#define SEEN 4096
static DWORD g_seen[SEEN][4];
static int   g_seen_used[SEEN];
static int   g_tagcount[64];
static int first_seen(int tag, DWORD a, DWORD b, DWORD c, int cap)
{
    DWORD h = ((tag * 2654435761u) ^ (a * 40503u) ^ (b * 2246822519u) ^ (c * 3266489917u)) % SEEN;
    int n;
    for (n = 0; n < SEEN; n++, h = (h + 1) % SEEN) {
        if (!g_seen_used[h]) break;
        if (g_seen[h][0] == (DWORD)tag && g_seen[h][1] == a && g_seen[h][2] == b && g_seen[h][3] == c) return 0;
    }
    if (n == SEEN || g_tagcount[tag & 63] >= cap) return 0;
    g_seen_used[h] = 1; g_seen[h][0] = tag; g_seen[h][1] = a; g_seen[h][2] = b; g_seen[h][3] = c;
    if (++g_tagcount[tag & 63] == cap) hook_log("[com] (no more distinct values logged for tag %d)\n", tag);
    return 1;
}
enum { T_QI = 1, T_SURF, T_RS, T_TSS, T_LS, T_DP, T_LOCK, T_BLT, T_CLEAR, T_VP, T_FMT, T_ZFMT, T_DEV, T_MODE, T_FLIP };

/* ---- attaching ---------------------------------------------------------------------------------- */
static void *typed_hook(int kind, int slot);
static HRESULT __stdcall H_QI(void *self, REFIID riid, void **ppv);

static void census_attach(void *obj, int kind)
{
    void **vt;
    DWORD old;
    int v, s;
    if (!g_on || !obj || kind < 0) return;
    __try { vt = *(void ***)obj; } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    for (v = 0; v < g_nvt; v++) {
        if (g_vt[v] != vt) continue;
        if (vt[0] == (void *)H_QI) {            /* still patched */
            if (g_vtkind[v] != kind)
                hook_log("[com] note: vtable %p already patched as %s, now also seen as %s\n", vt,
                         K[g_vtkind[v]].name, K[kind].name);
            return;
        }
        /* Same address but the patch is gone: the DLL that owns it was unloaded and loaded again at the
         * same base (dgVoodoo's D3DImm.dll does this between the device listing and the real device). */
        hook_log("[com] vtable %p (%s) was restored by a DLL reload: patching it again\n", vt, K[kind].name);
        break;
    }
    if (v == g_nvt && g_nvt >= MAXVT) { hook_log("[com] vtable table full, %s %p not patched\n", K[kind].name, obj); return; }
    if (!VirtualProtect(vt, K[kind].slots * sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        hook_log("[com] VirtualProtect failed for %s vtable %p\n", K[kind].name, vt);
        return;
    }
    for (s = 0; s < K[kind].slots; s++) {
        BYTE *stub = g_stubs + (v * SLOTS + s) * 10;
        void *t = typed_hook(kind, s);
        g_orig[v * SLOTS + s] = vt[s];
        stub[0] = 0xB8; *(DWORD *)(stub + 1) = v * SLOTS + s;                    /* mov eax, id */
        stub[5] = 0xE9; *(DWORD *)(stub + 6) = (DWORD)census_common - (DWORD)(stub + 10);  /* jmp common */
        vt[s] = t ? t : stub;
    }
    VirtualProtect(vt, K[kind].slots * sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), g_stubs, MAXVT * SLOTS * 10);
    g_vt[v] = vt; g_vtkind[v] = kind; if (v == g_nvt) g_nvt++;
    hook_log("[com] patched %s vtable %p as vt%d (object %p)\n", K[kind].name, vt, v, obj);
}

static int kind_of(void *obj) { int v = vt_index(obj); return v < 0 ? -1 : g_vtkind[v]; }

static int kind_from_iid(REFIID r)
{
    if (IsEqualIID(r, &IID_IDirectDraw))         return K_DD;
    if (IsEqualIID(r, &IID_IDirectDraw2))        return K_DD2;
    if (IsEqualIID(r, &IID_IDirectDraw4))        return K_DD4;
    if (IsEqualIID(r, &IID_IDirectDrawSurface))  return K_SURF1;
    if (IsEqualIID(r, &IID_IDirectDrawSurface4)) return K_SURF4;
    if (IsEqualIID(r, &IID_IDirect3D3))          return K_D3D3;
    if (IsEqualIID(r, &IID_IDirect3DDevice3))    return K_DEV3;
    if (IsEqualIID(r, &IID_IDirect3DViewport3))  return K_VP3;
    if (IsEqualIID(r, &IID_IDirect3DTexture2))   return K_TEX2;
    if (IsEqualIID(r, &IID_IDirect3DMaterial3))  return K_MAT3;
    if (IsEqualIID(r, &IID_IDirectDrawPalette))  return K_PAL;
    if (IsEqualIID(r, &IID_IDirectDrawClipper))  return K_CLIP;
    return -1;
}

static void log_desc(const char *what, const DDSURFACEDESC2 *d, HRESULT hr, void *out)
{
    const DDPIXELFORMAT *pf = &d->ddpfPixelFormat;
    hook_log("[com] %s: flags %08X caps %08X %ux%u backbuffers %u mips %u | pf flags %08X fourcc %08X %u bpp "
             "R %08X G %08X B %08X A %08X -> hr %08X %p\n", what, d->dwFlags, d->ddsCaps.dwCaps, d->dwWidth,
             d->dwHeight, d->dwBackBufferCount, d->dwMipMapCount, pf->dwFlags, pf->dwFourCC, pf->dwRGBBitCount,
             pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask, pf->dwRGBAlphaBitMask, (DWORD)hr, out);
}

/* ---- typed hooks ------------------------------------------------------------------------------- */
typedef HRESULT (__stdcall *f3)(void *, void *, void *);
typedef HRESULT (__stdcall *f4)(void *, void *, void *, void *);
typedef HRESULT (__stdcall *f5)(void *, void *, void *, void *, void *);
typedef HRESULT (__stdcall *f6)(void *, void *, void *, void *, void *, void *);
typedef HRESULT (__stdcall *f7)(void *, void *, void *, void *, void *, void *, void *);
#define ORIG(T, slot) ((T)orig_of(self, slot))

/* slot 0 of every kind */
static HRESULT __stdcall H_QI(void *self, REFIID riid, void **ppv)
{
    HRESULT hr;
    int k;
    note(self, 0, _ReturnAddress());
    hr = ORIG(f3, 0)(self, (void *)riid, ppv);
    k = kind_from_iid(riid);
    if (first_seen(T_QI, kind_of(self), riid->Data1, (DWORD)hr, 64))
        hook_log("[com] %s::QueryInterface {%08lX-%04X-%04X-...} (%s) -> hr %08X\n", K[kind_of(self)].name,
                 riid->Data1, riid->Data2, riid->Data3, k >= 0 ? K[k].name : "not tracked", (DWORD)hr);
    if (SUCCEEDED(hr) && ppv && *ppv) census_attach(*ppv, k);
    return hr;
}

/* IDirectDraw*: CreateClipper(4) CreatePalette(5) CreateSurface(6) SetCooperativeLevel(20) SetDisplayMode(21) */
static HRESULT __stdcall H_DD_CreateClipper(void *self, DWORD fl, void **out, void *outer)
{
    HRESULT hr;
    note(self, 4, _ReturnAddress());
    hr = ORIG(f4, 4)(self, (void *)fl, out, outer);
    if (SUCCEEDED(hr) && out) census_attach(*out, K_CLIP);
    return hr;
}
static HRESULT __stdcall H_DD_CreatePalette(void *self, DWORD fl, void *entries, void **out, void *outer)
{
    HRESULT hr;
    note(self, 5, _ReturnAddress());
    hr = ORIG(f5, 5)(self, (void *)fl, entries, out, outer);
    hook_log("[com] CreatePalette flags %08X -> hr %08X\n", fl, (DWORD)hr);
    if (SUCCEEDED(hr) && out) census_attach(*out, K_PAL);
    return hr;
}
static HRESULT __stdcall H_DD_CreateSurface(void *self, DDSURFACEDESC2 *d, void **out, void *outer)
{
    HRESULT hr;
    int k = kind_of(self);
    note(self, 6, _ReturnAddress());
    hr = ORIG(f4, 6)(self, d, out, outer);
    if (d && first_seen(T_SURF, d->ddsCaps.dwCaps, (d->dwWidth << 16) | d->dwHeight,
                        d->ddpfPixelFormat.dwRGBBitCount ^ d->ddpfPixelFormat.dwRGBAlphaBitMask ^ d->dwFlags, 120))
        log_desc("CreateSurface", d, hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out) census_attach(*out, k == K_DD4 ? K_SURF4 : K_SURF1);
    return hr;
}
static HRESULT __stdcall H_DD_SetCooperativeLevel(void *self, HWND w, DWORD fl)
{
    HRESULT hr;
    note(self, 20, _ReturnAddress());
    hr = ORIG(f3, 20)(self, w, (void *)fl);
    hook_log("[com] %s::SetCooperativeLevel hwnd %p flags %08X -> hr %08X\n", K[kind_of(self)].name, w, fl, (DWORD)hr);
    return hr;
}
static HRESULT __stdcall H_DD1_SetDisplayMode(void *self, DWORD w, DWORD h, DWORD bpp)
{
    HRESULT hr;
    note(self, 21, _ReturnAddress());
    hr = ORIG(f4, 21)(self, (void *)w, (void *)h, (void *)bpp);
    hook_log("[com] IDirectDraw::SetDisplayMode %ux%u %u bpp -> hr %08X\n", w, h, bpp, (DWORD)hr);
    return hr;
}
static HRESULT __stdcall H_DD2_SetDisplayMode(void *self, DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD fl)
{
    HRESULT hr;
    note(self, 21, _ReturnAddress());
    hr = ORIG(f6, 21)(self, (void *)w, (void *)h, (void *)bpp, (void *)hz, (void *)fl);
    hook_log("[com] %s::SetDisplayMode %ux%u %u bpp %u Hz flags %08X -> hr %08X\n", K[kind_of(self)].name,
             w, h, bpp, hz, fl, (DWORD)hr);
    return hr;
}
static HRESULT __stdcall H_DD_RestoreDisplayMode(void *self)
{
    typedef HRESULT (__stdcall *f1)(void *);
    HRESULT hr;
    note(self, 19, _ReturnAddress());
    hr = ORIG(f1, 19)(self);
    hook_log("[com] %s::RestoreDisplayMode -> hr %08X\n", K[kind_of(self)].name, (DWORD)hr);
    return hr;
}

/* surfaces: Blt(5) Flip(11) GetAttachedSurface(12) Lock(25) */
static HRESULT __stdcall H_S_Blt(void *self, RECT *dr, void *src, RECT *sr, DWORD fl, void *fx)
{
    note(self, 5, _ReturnAddress());
    if (first_seen(T_BLT, fl, src != NULL, (DWORD)_ReturnAddress(), 32)) {
        DDSURFACEDESC2 d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        if (kind_of(self) == K_SURF4) IDirectDrawSurface4_GetSurfaceDesc((IDirectDrawSurface4 *)self, &d);
        hook_log("[com] Blt flags %08X src %s, dst caps %08X %ux%u, from %s (%p)\n", fl, src ? "surface" : "none",
                 d.ddsCaps.dwCaps, d.dwWidth, d.dwHeight, g_modname[module_index((DWORD)_ReturnAddress())], _ReturnAddress());
    }
    return ORIG(f6, 5)(self, dr, src, sr, (void *)fl, fx);
}
static HRESULT __stdcall H_S_Flip(void *self, void *target, DWORD fl)
{
    note(self, 11, _ReturnAddress());
    if (first_seen(T_FLIP, fl, target != NULL, 0, 8)) hook_log("[com] Flip flags %08X target %p\n", fl, target);
    return ORIG(f3, 11)(self, target, (void *)fl);
}
static HRESULT __stdcall H_S_GetAttachedSurface(void *self, DDSCAPS2 *caps, void **out)
{
    HRESULT hr;
    note(self, 12, _ReturnAddress());
    hr = ORIG(f3, 12)(self, caps, out);
    hook_log("[com] GetAttachedSurface caps %08X -> hr %08X %p\n", caps ? caps->dwCaps : 0, (DWORD)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out) census_attach(*out, kind_of(self));
    return hr;
}
static HRESULT __stdcall H_S_Lock(void *self, RECT *r, DDSURFACEDESC2 *d, DWORD fl, HANDLE ev)
{
    HRESULT hr;
    note(self, 25, _ReturnAddress());
    hr = ORIG(f5, 25)(self, r, d, (void *)fl, ev);
    if (d && first_seen(T_LOCK, d->ddsCaps.dwCaps, fl, (DWORD)_ReturnAddress(), 64))
        hook_log("[com] Lock flags %08X rect %s, surface caps %08X %ux%u %u bpp, from %s (%p) -> hr %08X\n", fl,
                 r ? "yes" : "whole", d->ddsCaps.dwCaps, d->dwWidth, d->dwHeight, d->ddpfPixelFormat.dwRGBBitCount,
                 g_modname[module_index((DWORD)_ReturnAddress())], _ReturnAddress(), (DWORD)hr);
    return hr;
}

/* IDirect3D3: EnumDevices(3) CreateLight(4) CreateMaterial(5) CreateViewport(6) CreateDevice(8) EnumZBufferFormats(10) */
static LPD3DENUMDEVICESCALLBACK g_devcb;
static HRESULT CALLBACK devcb(GUID *g, LPSTR desc, LPSTR name, LPD3DDEVICEDESC hw, LPD3DDEVICEDESC hel, LPVOID ctx)
{
    HRESULT r = g_devcb(g, desc, name, hw, hel, ctx);
    if (first_seen(T_DEV, g ? g->Data1 : 0, 0, 0, 16))
        hook_log("[com] EnumDevices: \"%s\" / \"%s\" guid %08lX hw caps %08X -> game says %s\n", name ? name : "",
                 desc ? desc : "", g ? g->Data1 : 0, hw ? hw->dwFlags : 0, r ? "continue" : "stop");
    return r;
}
static HRESULT __stdcall H_D3D_EnumDevices(void *self, LPD3DENUMDEVICESCALLBACK cb, void *ctx)
{
    HRESULT hr;
    LPD3DENUMDEVICESCALLBACK saved = g_devcb;
    note(self, 3, _ReturnAddress());
    g_devcb = cb;
    hr = ORIG(f3, 3)(self, cb ? (void *)devcb : NULL, ctx);
    g_devcb = saved;
    return hr;
}
static HRESULT __stdcall H_D3D_CreateLight(void *self, void **out, void *outer)
{
    HRESULT hr;
    note(self, 4, _ReturnAddress());
    hr = ORIG(f3, 4)(self, out, outer);
    if (SUCCEEDED(hr) && out) census_attach(*out, K_LIGHT);
    return hr;
}
static HRESULT __stdcall H_D3D_CreateMaterial(void *self, void **out, void *outer)
{
    HRESULT hr;
    note(self, 5, _ReturnAddress());
    hr = ORIG(f3, 5)(self, out, outer);
    if (SUCCEEDED(hr) && out) census_attach(*out, K_MAT3);
    return hr;
}
static HRESULT __stdcall H_D3D_CreateViewport(void *self, void **out, void *outer)
{
    HRESULT hr;
    note(self, 6, _ReturnAddress());
    hr = ORIG(f3, 6)(self, out, outer);
    if (SUCCEEDED(hr) && out) census_attach(*out, K_VP3);
    return hr;
}
static HRESULT __stdcall H_D3D_CreateDevice(void *self, REFCLSID cls, void *surf, void **out, void *outer)
{
    HRESULT hr;
    note(self, 8, _ReturnAddress());
    hr = ORIG(f5, 8)(self, (void *)cls, surf, out, outer);
    hook_log("[com] IDirect3D3::CreateDevice guid %08lX on surface %p -> hr %08X %p\n", cls ? cls->Data1 : 0, surf,
             (DWORD)hr, out ? *out : NULL);
    if (SUCCEEDED(hr) && out) census_attach(*out, K_DEV3);
    return hr;
}
static LPD3DENUMPIXELFORMATSCALLBACK g_fmtcb;
static int g_fmttag;
static HRESULT CALLBACK fmtcb(LPDDPIXELFORMAT pf, LPVOID ctx)
{
    HRESULT r = g_fmtcb(pf, ctx);
    if (pf && first_seen(g_fmttag, pf->dwFlags ^ pf->dwRGBBitCount, pf->dwRBitMask ^ pf->dwRGBAlphaBitMask,
                         pf->dwGBitMask ^ pf->dwFourCC, 48))
        hook_log("[com] %s: flags %08X fourcc %08X %u bpp R %08X G %08X B %08X A %08X -> game says %s\n",
                 g_fmttag == T_ZFMT ? "EnumZBufferFormats" : "EnumTextureFormats", pf->dwFlags, pf->dwFourCC,
                 pf->dwRGBBitCount, pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask, pf->dwRGBAlphaBitMask,
                 r ? "continue" : "stop");
    return r;
}
static HRESULT __stdcall H_D3D_EnumZBufferFormats(void *self, REFCLSID cls, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    HRESULT hr;
    note(self, 10, _ReturnAddress());
    g_fmtcb = cb; g_fmttag = T_ZFMT;
    hr = ORIG(f4, 10)(self, (void *)cls, cb ? (void *)fmtcb : NULL, ctx);
    return hr;
}

/* IDirect3DDevice3: EnumTextureFormats(8) SetRenderState(22) SetLightState(24) DrawPrimitive(28) SetTextureStageState(40) */
static HRESULT __stdcall H_DEV_EnumTextureFormats(void *self, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    note(self, 8, _ReturnAddress());
    g_fmtcb = cb; g_fmttag = T_FMT;
    return ORIG(f3, 8)(self, cb ? (void *)fmtcb : NULL, ctx);
}
static HRESULT __stdcall H_DEV_SetRenderState(void *self, DWORD st, DWORD val)
{
    note(self, 22, _ReturnAddress());
    if (first_seen(T_RS, st, val, 0, 400)) hook_log("[com] SetRenderState %u = %08X\n", st, val);
    return ORIG(f3, 22)(self, (void *)st, (void *)val);
}
static HRESULT __stdcall H_DEV_SetLightState(void *self, DWORD st, DWORD val)
{
    note(self, 24, _ReturnAddress());
    if (first_seen(T_LS, st, val, 0, 64)) hook_log("[com] SetLightState %u = %08X\n", st, val);
    return ORIG(f3, 24)(self, (void *)st, (void *)val);
}
static HRESULT __stdcall H_DEV_DrawPrimitive(void *self, DWORD type, DWORD fvf, void *v, DWORD n, DWORD fl)
{
    note(self, 28, _ReturnAddress());
    if (first_seen(T_DP, type, fvf, fl, 64))
        hook_log("[com] DrawPrimitive type %u fvf %08X flags %08X (first: %u vertices, from %s)\n", type, fvf, fl, n,
                 g_modname[module_index((DWORD)_ReturnAddress())]);
    return ORIG(f6, 28)(self, (void *)type, (void *)fvf, v, (void *)n, (void *)fl);
}
static HRESULT __stdcall H_DEV_SetTextureStageState(void *self, DWORD stage, DWORD type, DWORD val)
{
    note(self, 40, _ReturnAddress());
    if (first_seen(T_TSS, (stage << 16) | type, val, 0, 400))
        hook_log("[com] SetTextureStageState stage %u type %u = %08X\n", stage, type, val);
    return ORIG(f4, 40)(self, (void *)stage, (void *)type, (void *)val);
}

/* IDirect3DViewport3: SetViewport2(17) Clear2(20) */
static HRESULT __stdcall H_VP_SetViewport2(void *self, D3DVIEWPORT2 *vp)
{
    note(self, 17, _ReturnAddress());
    if (vp && first_seen(T_VP, (vp->dwX << 16) | vp->dwY, (vp->dwWidth << 16) | vp->dwHeight,
                         *(DWORD *)&vp->dvMaxZ, 16))
        hook_log("[com] SetViewport2 %u,%u %ux%u clip %.3f %.3f %.3f %.3f z %.3f..%.3f\n", vp->dwX, vp->dwY,
                 vp->dwWidth, vp->dwHeight, vp->dvClipX, vp->dvClipY, vp->dvClipWidth, vp->dvClipHeight,
                 vp->dvMinZ, vp->dvMaxZ);
    return ((HRESULT (__stdcall *)(void *, D3DVIEWPORT2 *))orig_of(self, 17))(self, vp);
}
static HRESULT __stdcall H_VP_Clear2(void *self, DWORD count, D3DRECT *rects, DWORD fl, DWORD color, float z, DWORD stencil)
{
    typedef HRESULT (__stdcall *fc)(void *, DWORD, D3DRECT *, DWORD, DWORD, float, DWORD);
    note(self, 20, _ReturnAddress());
    if (first_seen(T_CLEAR, fl, count, 0, 16))
        hook_log("[com] Clear2 flags %08X rects %u color %08X z %.3f\n", fl, count, color, z);
    return ((fc)orig_of(self, 20))(self, count, rects, fl, color, z, stencil);
}

static void *typed_hook(int kind, int slot)
{
    if (slot == 0) return (void *)H_QI;
    switch (kind) {
    case K_DD: case K_DD2: case K_DD4:
        if (slot == 4)  return (void *)H_DD_CreateClipper;
        if (slot == 5)  return (void *)H_DD_CreatePalette;
        if (slot == 6)  return (void *)H_DD_CreateSurface;
        if (slot == 19) return (void *)H_DD_RestoreDisplayMode;
        if (slot == 20) return (void *)H_DD_SetCooperativeLevel;
        if (slot == 21) return kind == K_DD ? (void *)H_DD1_SetDisplayMode : (void *)H_DD2_SetDisplayMode;
        break;
    case K_SURF1: case K_SURF4:
        if (slot == 5)  return (void *)H_S_Blt;
        if (slot == 11) return (void *)H_S_Flip;
        if (slot == 12) return (void *)H_S_GetAttachedSurface;
        if (slot == 25) return (void *)H_S_Lock;
        break;
    case K_D3D3:
        if (slot == 3)  return (void *)H_D3D_EnumDevices;
        if (slot == 4)  return (void *)H_D3D_CreateLight;
        if (slot == 5)  return (void *)H_D3D_CreateMaterial;
        if (slot == 6)  return (void *)H_D3D_CreateViewport;
        if (slot == 8)  return (void *)H_D3D_CreateDevice;
        if (slot == 10) return (void *)H_D3D_EnumZBufferFormats;
        break;
    case K_DEV3:
        if (slot == 8)  return (void *)H_DEV_EnumTextureFormats;
        if (slot == 22) return (void *)H_DEV_SetRenderState;
        if (slot == 24) return (void *)H_DEV_SetLightState;
        if (slot == 28) return (void *)H_DEV_DrawPrimitive;
        if (slot == 40) return (void *)H_DEV_SetTextureStageState;
        break;
    case K_VP3:
        if (slot == 17) return (void *)H_VP_SetViewport2;
        if (slot == 20) return (void *)H_VP_Clear2;
        break;
    }
    return NULL;
}

/* ---- public ------------------------------------------------------------------------------------ */
void census_on_directdraw(void *dd)
{
    static int checked;
    if (!checked) {
        checked = 1;
        g_on = hook_ini_int("hooks", "enable", 1) && hook_ini_int("census", "enable", 1);
        if (g_on) {
            g_stubs = (BYTE *)VirtualAlloc(NULL, MAXVT * SLOTS * 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!g_stubs) g_on = 0;
        }
        hook_log("[com] census %s\n", g_on ? "on" : "off");
    }
    census_attach(dd, K_DD);
    hook_log_flush();
}

void census_report(const char *why)
{
    int v, s, m;
    if (!g_on) return;
    hook_log("[com] ---- census (%s): calls per method and calling module ----\n", why);
    for (v = 0; v < g_nvt; v++) {
        const int k = g_vtkind[v];
        for (s = 0; s < K[k].slots; s++) {
            char line[512];
            int len = 0, any = 0;
            for (m = 0; m < g_nmod; m++) {
                if (!g_count[v * SLOTS + s][m]) continue;
                len += _snprintf(line + len, sizeof(line) - len, "  %s %u", g_modname[m], g_count[v * SLOTS + s][m]);
                any = 1;
                if (len >= (int)sizeof(line) - 64) break;
            }
            if (any) hook_log("[com]   vt%-2d %-20s %-28s%s\n", v, K[k].name, K[k].m[s], line);
        }
    }
    hook_log("[com] ---- end of census ----\n");
    hook_log_flush();
}
