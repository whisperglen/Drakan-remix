/*
 * be_ddraw.c - M1 back end, DirectX 6 side: the fake COM objects Drakan talks to (REMIX.md 5.3, 5.10).
 *
 * [backend] d3d9=1 makes our ddraw.dll's DirectDrawCreate return these objects instead of chaining to
 * dgVoodoo / the system DirectDraw. Rendering goes to be_d9.c.
 *
 * Every interface gets a full vtable. Methods the game uses (the M0 census, F-035/F-036) are implemented;
 * every other slot is a generated stub that logs "[be] not implemented: X::Y" once and returns E_NOTIMPL,
 * with the correct stdcall stack cleanup for that method's argument count.
 *
 * Surfaces (one struct for all roles):
 *   PRIMARY  the screen. Blt(back -> primary) or Flip = Present.
 *   BACK     the 3D render target = the D3D9 back buffer. Lock = read-back; colour-fill Blt = Clear.
 *   ZBUF     depth buffer. Lock returns a constant buffer (lens flares: all "near" = hidden, REMIX.md 5.3).
 *   MEM      system-memory or "video" textures and plain surfaces: real memory the game writes into.
 *            IDirect3DTexture2::Load converts the source's memory to a D3D9 texture of the destination.
 */
#define CINTERFACE
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <ddraw.h>
#include <d3d.h>
#include "hook.h"
#include "be.h"

#define LOG_ONCE(...) do { static int once_; if (!once_++) hook_log(__VA_ARGS__); } while (0)

/* ---- objects ------------------------------------------------------------------------------------- */
enum { R_PRIMARY = 1, R_BACK, R_ZBUF, R_MEM };

typedef struct Obj { void **vtbl; LONG ref; } Obj;

typedef struct Pal { void **vtbl; LONG ref; DWORD caps; PALETTEENTRY e[256]; } Pal;
typedef struct Clip { void **vtbl; LONG ref; HWND hwnd; } Clip;
typedef struct Mat { void **vtbl; LONG ref; D3DMATERIAL m; } Mat;

typedef struct Surf Surf;
typedef struct Sub { void **vtbl; Surf *owner; } Sub;   /* IDirect3DTexture2 / IDirectDrawGammaControl views */
struct Surf {
    void **vtbl; LONG ref;
    Sub tex2, gamma;
    DDSURFACEDESC2 desc;
    int role, bpp, pitch;
    BYTE *mem;
    Pal *pal;
    Surf *back, *z;
    DDCOLORKEY ck; int has_ck;
    void *d9tex; int d9w, d9h, d9fmt;
    DWORD lock_flags;
};

typedef struct Vp { void **vtbl; LONG ref; DWORD x, y, w, h; struct Dev *dev; } Vp;
typedef struct Dev { void **vtbl; LONG ref; Surf *target; Vp *cur; DWORD rs[256]; DWORD tss[8][32]; Sub *tex[8]; } Dev;

static void *vt_dd1[23], *vt_dd4[28], *vt_surf[45], *vt_tex2[6], *vt_gamma[5], *vt_pal[7], *vt_clip[9];
static void *vt_d3d3[12], *vt_dev3[42], *vt_vp3[21], *vt_mat3[6];

/* global display state */
static HWND  g_hwnd;
static int   g_exclusive, g_mode_w, g_mode_h, g_mode_bpp = 32, g_mode_set;
static Surf *g_primary, *g_back_current;

int be_enabled(void)
{
    static int v = -1;
    if (v < 0) v = hook_ini_int("hooks", "enable", 1) && hook_ini_int("backend", "d3d9", 0);
    return v;
}

static void *obj_new(size_t size, void **vtbl)
{
    Obj *o = (Obj *)calloc(1, size);
    if (o) { o->vtbl = vtbl; o->ref = 1; }
    return o;
}

/* ---- generated stubs for methods the game never calls --------------------------------------------- */
static const char *g_stub_names[512];
static BYTE *g_stub_code;
static int g_nstub;

static void __stdcall be_unimpl(DWORD id)
{
    static BYTE seen[512];
    if (id < 512 && !seen[id]++) { hook_log("[be] not implemented: %s\n", g_stub_names[id]); hook_log_flush(); }
}

static void *make_stub(const char *name, int nargs)
{
    BYTE *p;
    if (!g_stub_code) g_stub_code = (BYTE *)VirtualAlloc(NULL, 512 * 20, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_stub_code || g_nstub >= 512) return NULL;
    p = g_stub_code + g_nstub * 20;
    g_stub_names[g_nstub] = name;
    p[0] = 0x68; *(DWORD *)(p + 1) = g_nstub;                                   /* push id            */
    p[5] = 0xE8; *(DWORD *)(p + 6) = (DWORD)be_unimpl - (DWORD)(p + 10);        /* call be_unimpl     */
    p[10] = 0xB8; *(DWORD *)(p + 11) = (DWORD)E_NOTIMPL;                         /* mov eax, E_NOTIMPL */
    p[15] = 0xC2; *(WORD *)(p + 16) = (WORD)(4 * (nargs + 1));                   /* ret 4*(n+1)        */
    g_nstub++;
    return p;
}

typedef struct { const char *name; int nargs; void *fn; } Meth;
static void build_vtbl(void **vt, const Meth *m, int n)
{
    int i;
    for (i = 0; i < n; i++) vt[i] = m[i].fn ? m[i].fn : make_stub(m[i].name, m[i].nargs);
}

/* ---- helpers ------------------------------------------------------------------------------------------ */
static void pf_display(DDPIXELFORMAT *pf)
{
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = DDPF_RGB;
    if (g_mode_bpp == 16) { pf->dwRGBBitCount = 16; pf->dwRBitMask = 0xF800; pf->dwGBitMask = 0x7E0; pf->dwBBitMask = 0x1F; }
    else { pf->dwRGBBitCount = 32; pf->dwRBitMask = 0xFF0000; pf->dwGBitMask = 0xFF00; pf->dwBBitMask = 0xFF; }
}

static void desktop_size(int *w, int *h) { *w = GetSystemMetrics(SM_CXSCREEN); *h = GetSystemMetrics(SM_CYSCREEN); }

static void mode_size(int *w, int *h)
{
    if (g_exclusive && g_mode_set) { *w = g_mode_w; *h = g_mode_h; } else desktop_size(w, h);
}

static DWORD expand(DWORD v, DWORD mask)
{
    int shift = 0, bits = 0;
    if (!mask) return 255;
    while (!((mask >> shift) & 1)) shift++;
    while (shift + bits < 32 && ((mask >> (shift + bits)) & 1)) bits++;
    v = (v & mask) >> shift;
    if (bits >= 8) return v >> (bits - 8);
    return (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
}

static DWORD pack(DWORD argb, const DDPIXELFORMAT *pf)     /* 8888 -> the surface's RGB format */
{
    DWORD out = 0, ch, k;
    const DWORD masks[4] = { pf->dwRGBAlphaBitMask, pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask };
    for (k = 0; k < 4; k++) {
        DWORD m = masks[k], shift = 0, bits = 0;
        if (!m) continue;
        while (!((m >> shift) & 1)) shift++;
        while (shift + bits < 32 && ((m >> (shift + bits)) & 1)) bits++;
        ch = (argb >> (24 - 8 * k)) & 0xFF;
        out |= ((bits >= 8 ? ch << (bits - 8) : ch >> (8 - bits)) << shift) & m;
    }
    return out;
}

static DWORD read_px(const BYTE *row, int x, int bpp)
{
    switch (bpp) {
    case 8:  return row[x];
    case 16: return ((const WORD *)row)[x];
    case 24: return row[3 * x] | (row[3 * x + 1] << 8) | (row[3 * x + 2] << 16);
    default: return ((const DWORD *)row)[x];
    }
}

static void write_px(BYTE *row, int x, int bpp, DWORD v)
{
    switch (bpp) {
    case 8:  row[x] = (BYTE)v; break;
    case 16: ((WORD *)row)[x] = (WORD)v; break;
    case 24: row[3 * x] = (BYTE)v; row[3 * x + 1] = (BYTE)(v >> 8); row[3 * x + 2] = (BYTE)(v >> 16); break;
    default: ((DWORD *)row)[x] = v;
    }
}

/* surface memory -> 32-bit ARGB rows (out: w*h); pal for 8-bit palettised */
static void to_argb(const Surf *s, const Pal *pal, DWORD *out)
{
    const DDPIXELFORMAT *pf = &s->desc.ddpfPixelFormat;
    int x, y, w = s->desc.dwWidth, h = s->desc.dwHeight;
    for (y = 0; y < h; y++) {
        const BYTE *row = s->mem + y * s->pitch;
        DWORD *o = out + y * w;
        if (pf->dwFlags & DDPF_PALETTEINDEXED8) {
            for (x = 0; x < w; x++) {
                const PALETTEENTRY *e = pal ? &pal->e[row[x]] : NULL;
                o[x] = e ? 0xFF000000 | (e->peRed << 16) | (e->peGreen << 8) | e->peBlue : 0xFF000000 | row[x] * 0x010101;
            }
        } else if (s->bpp == 32 && pf->dwRBitMask == 0xFF0000 && pf->dwGBitMask == 0xFF00 && pf->dwBBitMask == 0xFF) {
            const DWORD *r = (const DWORD *)row;
            DWORD a = (pf->dwFlags & DDPF_ALPHAPIXELS) ? 0 : 0xFF000000;
            for (x = 0; x < w; x++) o[x] = r[x] | a;
        } else {
            for (x = 0; x < w; x++) {
                DWORD v = read_px(row, x, s->bpp);
                DWORD a = (pf->dwFlags & DDPF_ALPHAPIXELS) ? expand(v, pf->dwRGBAlphaBitMask) : 255;
                o[x] = (a << 24) | (expand(v, pf->dwRBitMask) << 16) | (expand(v, pf->dwGBitMask) << 8) | expand(v, pf->dwBBitMask);
            }
        }
    }
}

static void from_argb(Surf *s, const DWORD *in)
{
    int x, y, w = s->desc.dwWidth, h = s->desc.dwHeight;
    for (y = 0; y < h; y++) {
        BYTE *row = s->mem + y * s->pitch;
        if (s->bpp == 32) memcpy(row, in + y * w, w * 4);
        else for (x = 0; x < w; x++) write_px(row, x, s->bpp, pack(in[y * w + x], &s->desc.ddpfPixelFormat));
    }
}

/* ---- palette ------------------------------------------------------------------------------------------ */
static HRESULT __stdcall pal_QI(Pal *p, REFIID r, void **out)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectDrawPalette)) { p->ref++; *out = p; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG __stdcall gen_AddRef(Obj *o) { return ++o->ref; }
static ULONG __stdcall gen_Release(Obj *o) { LONG r = --o->ref; if (r <= 0) { free(o); return 0; } return r; }
static HRESULT __stdcall pal_GetCaps(Pal *p, DWORD *caps) { if (caps) *caps = p->caps; return DD_OK; }
static HRESULT __stdcall pal_GetEntries(Pal *p, DWORD fl, DWORD start, DWORD n, PALETTEENTRY *e)
{
    if (!e || start + n > 256) return DDERR_INVALIDPARAMS;
    memcpy(e, p->e + start, n * sizeof(*e)); return DD_OK;
}
static HRESULT __stdcall pal_SetEntries(Pal *p, DWORD fl, DWORD start, DWORD n, PALETTEENTRY *e)
{
    if (!e || start + n > 256) return DDERR_INVALIDPARAMS;
    memcpy(p->e + start, e, n * sizeof(*e)); return DD_OK;
}

/* ---- clipper ------------------------------------------------------------------------------------------- */
static HRESULT __stdcall clip_QI(Clip *c, REFIID r, void **out)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectDrawClipper)) { c->ref++; *out = c; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static HRESULT __stdcall clip_GetHWnd(Clip *c, HWND *w) { if (w) *w = c->hwnd; return DD_OK; }
static HRESULT __stdcall clip_SetHWnd(Clip *c, DWORD fl, HWND w) { c->hwnd = w; if (!g_hwnd) g_hwnd = w; return DD_OK; }
static HRESULT __stdcall clip_IsClipListChanged(Clip *c, BOOL *b) { if (b) *b = FALSE; return DD_OK; }

/* ---- material ------------------------------------------------------------------------------------------- */
static HRESULT __stdcall mat_QI(Mat *m, REFIID r, void **out) { m->ref++; *out = m; return S_OK; }
static HRESULT __stdcall mat_SetMaterial(Mat *m, D3DMATERIAL *d) { if (d) m->m = *d; return D3D_OK; }
static HRESULT __stdcall mat_GetMaterial(Mat *m, D3DMATERIAL *d) { if (d) *d = m->m; return D3D_OK; }
static HRESULT __stdcall mat_GetHandle(Mat *m, void *dev, D3DMATERIALHANDLE *h) { if (h) *h = (D3DMATERIALHANDLE)m; return D3D_OK; }

/* ---- surfaces ------------------------------------------------------------------------------------------- */
static Surf *surf_new(const DDSURFACEDESC2 *d, int role, int w, int h)
{
    Surf *s = (Surf *)obj_new(sizeof(Surf), vt_surf);
    if (!s) return NULL;
    s->tex2.vtbl = vt_tex2; s->tex2.owner = s;
    s->gamma.vtbl = vt_gamma; s->gamma.owner = s;
    s->desc = *d;
    s->desc.dwSize = sizeof(DDSURFACEDESC2);
    s->role = role;
    s->desc.dwWidth = w; s->desc.dwHeight = h;
    if (!(d->dwFlags & DDSD_PIXELFORMAT) || role == R_PRIMARY || role == R_BACK) pf_display(&s->desc.ddpfPixelFormat);
    s->bpp = s->desc.ddpfPixelFormat.dwRGBBitCount;
    if (s->bpp == 0 && (s->desc.ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8)) s->bpp = 8;
    if (s->bpp == 0) s->bpp = 32;
    s->pitch = ((w * s->bpp / 8) + 3) & ~3;
    s->desc.lPitch = s->pitch;
    s->desc.dwFlags |= DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
    s->desc.dwFlags &= ~DDSD_LPSURFACE;
    s->desc.lpSurface = NULL;
    if (!(s->desc.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) s->desc.ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    if (role != R_PRIMARY) s->mem = (BYTE *)calloc(1, s->pitch * h + 16);
    if (role == R_ZBUF && s->mem)   /* lens flares: 0 = everything in front, so the sun is "hidden" (default) */
        memset(s->mem, hook_ini_int("backend", "lens_flares", 0) ? 0xFF : 0x00, s->pitch * h);
    return s;
}

static ULONG __stdcall surf_Release(Surf *s);
static void surf_free(Surf *s)
{
    if (s->back) surf_Release(s->back);
    if (s->z) surf_Release(s->z);
    if (s->pal) gen_Release((Obj *)s->pal);
    if (s->d9tex) d9_tex_release(s->d9tex);
    if (s == g_primary) g_primary = NULL;
    if (s == g_back_current) g_back_current = NULL;
    free(s->mem);
    free(s);
}

static HRESULT __stdcall surf_QI(Surf *s, REFIID r, void **out)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectDrawSurface4)) { s->ref++; *out = s; return S_OK; }
    if (IsEqualIID(r, &IID_IDirect3DTexture2)) { s->ref++; *out = &s->tex2; return S_OK; }
    if (IsEqualIID(r, &IID_IDirectDrawGammaControl)) { s->ref++; *out = &s->gamma; return S_OK; }
    hook_log("[be] Surface QueryInterface {%08lX-...}: not supported\n", r->Data1);
    *out = NULL; return E_NOINTERFACE;
}
static ULONG __stdcall surf_AddRef(Surf *s) { return ++s->ref; }
static ULONG __stdcall surf_Release(Surf *s) { LONG r = --s->ref; if (r <= 0) { surf_free(s); return 0; } return r; }

static HRESULT __stdcall surf_GetSurfaceDesc(Surf *s, DDSURFACEDESC2 *d)
{
    DWORD size;
    if (!d) return DDERR_INVALIDPARAMS;
    size = d->dwSize ? d->dwSize : sizeof(*d);
    if (size > sizeof(*d)) size = sizeof(*d);
    memcpy((BYTE *)d + 4, (BYTE *)&s->desc + 4, size - 4);
    d->dwSize = size;
    return DD_OK;
}
static HRESULT __stdcall surf_GetCaps(Surf *s, DDSCAPS2 *c) { if (c) *c = s->desc.ddsCaps; return DD_OK; }
static HRESULT __stdcall surf_GetPixelFormat(Surf *s, DDPIXELFORMAT *pf) { if (pf) *pf = s->desc.ddpfPixelFormat; return DD_OK; }
static HRESULT __stdcall surf_ok0(Surf *s) { return DD_OK; }   /* IsLost, Restore */

static HRESULT __stdcall surf_AddAttachedSurface(Surf *s, Surf *a)
{
    if (!a) return DDERR_INVALIDPARAMS;
    if (a->role == R_ZBUF) { a->ref++; if (s->z) surf_Release(s->z); s->z = a; return DD_OK; }
    LOG_ONCE("[be] AddAttachedSurface of a non-z surface ignored\n");
    return DD_OK;
}
static HRESULT __stdcall surf_DeleteAttachedSurface(Surf *s, DWORD fl, Surf *a)
{
    if (a && s->z == a) { s->z = NULL; surf_Release(a); }
    return DD_OK;
}
static HRESULT __stdcall surf_GetAttachedSurface(Surf *s, DDSCAPS2 *caps, Surf **out)
{
    Surf *r = NULL;
    if (!caps || !out) return DDERR_INVALIDPARAMS;
    if ((caps->dwCaps & DDSCAPS_BACKBUFFER) && s->back) r = s->back;
    else if ((caps->dwCaps & DDSCAPS_ZBUFFER) && s->z) r = s->z;
    if (!r) { *out = NULL; return DDERR_NOTFOUND; }
    r->ref++; *out = r; return DD_OK;
}

static HRESULT __stdcall surf_Lock(Surf *s, RECT *r, DDSURFACEDESC2 *d, DWORD fl, HANDLE ev)
{
    BYTE *p;
    if (!d) return DDERR_INVALIDPARAMS;
    if (!s->mem) return DDERR_GENERIC;
    if (s->role == R_BACK) {   /* screenshot / debug icons: read the D3D9 back buffer into our memory */
        DWORD *tmp = (DWORD *)malloc(s->desc.dwWidth * s->desc.dwHeight * 4);
        if (tmp && d9_read_back(tmp, s->desc.dwWidth, s->desc.dwHeight)) from_argb(s, tmp);
        free(tmp);
    }
    s->lock_flags = fl;
    surf_GetSurfaceDesc(s, d);
    p = s->mem;
    if (r) p += r->top * s->pitch + r->left * (s->bpp / 8);
    d->lpSurface = p;
    d->lPitch = s->pitch;
    d->dwFlags |= DDSD_LPSURFACE | DDSD_PITCH;
    return DD_OK;
}
static HRESULT __stdcall surf_Unlock(Surf *s, RECT *r)
{
    if (s->role == R_BACK && !(s->lock_flags & DDLOCK_READONLY)) {
        DWORD *tmp = (DWORD *)malloc(s->desc.dwWidth * s->desc.dwHeight * 4);
        if (tmp) { to_argb(s, NULL, tmp); d9_write_back(tmp, s->desc.dwWidth, s->desc.dwHeight); free(tmp); }
    }
    s->lock_flags = 0;
    return DD_OK;
}

static void fill_mem(Surf *s, RECT *r, DWORD color)
{
    int x, y, x0 = 0, y0 = 0, x1 = s->desc.dwWidth, y1 = s->desc.dwHeight;
    if (!s->mem) return;
    if (r) { x0 = r->left; y0 = r->top; x1 = r->right; y1 = r->bottom; }
    for (y = y0; y < y1; y++) for (x = x0; x < x1; x++) write_px(s->mem + y * s->pitch, x, s->bpp, color);
}

static HRESULT __stdcall surf_Blt(Surf *dst, RECT *dr, Surf *src, RECT *sr, DWORD fl, DDBLTFX *fx)
{
    if (fl & DDBLT_COLORFILL) {
        DWORD c = fx ? fx->dwFillColor : 0;
        if (dst->role == R_BACK) {
            DWORD argb = 0;
            if (dst->bpp == 32) argb = c; else { DWORD *t = &argb; Surf tmp = *dst; tmp.mem = (BYTE *)&c; tmp.pitch = 4;
                tmp.desc.dwWidth = 1; tmp.desc.dwHeight = 1; to_argb(&tmp, NULL, t); }
            d9_clear(1, argb, 1.0f, 0);
        } else if (dst->role == R_MEM) fill_mem(dst, dr, c);
        return DD_OK;
    }
    if (fl & DDBLT_DEPTHFILL) { d9_clear(2, 0, 1.0f, 0); return DD_OK; }
    if (!src) { LOG_ONCE("[be] Blt without source, flags %08X: ignored\n", fl); return DD_OK; }
    if (dst->role == R_PRIMARY && src->role == R_BACK) { d9_present(); return DD_OK; }
    if (src->role == R_BACK && dst->role == R_MEM) {          /* save the frame (menu background) */
        DWORD *tmp = (DWORD *)malloc(dst->desc.dwWidth * dst->desc.dwHeight * 4);
        if (tmp && d9_read_back(tmp, dst->desc.dwWidth, dst->desc.dwHeight)) from_argb(dst, tmp);
        free(tmp);
        return DD_OK;
    }
    if (src->role == R_MEM && dst->role == R_BACK) {          /* restore it */
        DWORD *tmp = (DWORD *)malloc(src->desc.dwWidth * src->desc.dwHeight * 4);
        if (tmp) { to_argb(src, src->pal, tmp); d9_write_back(tmp, src->desc.dwWidth, src->desc.dwHeight); free(tmp); }
        return DD_OK;
    }
    if (src->role == R_MEM && dst->role == R_MEM && src->bpp == dst->bpp && src->mem && dst->mem) {
        RECT a = { 0, 0, (LONG)src->desc.dwWidth, (LONG)src->desc.dwHeight }, b = { 0, 0, (LONG)dst->desc.dwWidth, (LONG)dst->desc.dwHeight };
        int y, bpp = src->bpp / 8, w, h;
        if (sr) a = *sr;
        if (dr) b = *dr;
        w = min(a.right - a.left, b.right - b.left); h = min(a.bottom - a.top, b.bottom - b.top);
        for (y = 0; y < h; y++)
            memcpy(dst->mem + (b.top + y) * dst->pitch + b.left * bpp, src->mem + (a.top + y) * src->pitch + a.left * bpp, w * bpp);
        return DD_OK;
    }
    LOG_ONCE("[be] Blt role %d -> role %d flags %08X: not handled\n", src->role, dst->role, fl);
    return DD_OK;
}
static HRESULT __stdcall surf_BltFast(Surf *dst, DWORD x, DWORD y, Surf *src, RECT *sr, DWORD fl)
{
    RECT dr;
    if (!src) return DDERR_INVALIDPARAMS;
    dr.left = x; dr.top = y;
    dr.right = x + (sr ? sr->right - sr->left : (LONG)src->desc.dwWidth);
    dr.bottom = y + (sr ? sr->bottom - sr->top : (LONG)src->desc.dwHeight);
    return surf_Blt(dst, &dr, src, sr, 0, NULL);
}
static HRESULT __stdcall surf_Flip(Surf *s, Surf *target, DWORD fl) { d9_present(); return DD_OK; }

static HRESULT __stdcall surf_SetPalette(Surf *s, Pal *p)
{
    if (p) p->ref++;
    if (s->pal) gen_Release((Obj *)s->pal);
    s->pal = p;
    return DD_OK;
}
static HRESULT __stdcall surf_GetPalette(Surf *s, Pal **out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    if (!s->pal) { *out = NULL; return DDERR_NOPALETTEATTACHED; }
    s->pal->ref++; *out = s->pal; return DD_OK;
}
static HRESULT __stdcall surf_SetColorKey(Surf *s, DWORD fl, DDCOLORKEY *k)
{
    if (fl & DDCKEY_SRCBLT) {
        if (k) { s->ck = *k; s->has_ck = 1; s->desc.dwFlags |= DDSD_CKSRCBLT; s->desc.ddckCKSrcBlt = *k; }
        else { s->has_ck = 0; s->desc.dwFlags &= ~DDSD_CKSRCBLT; }
    }
    return DD_OK;
}
static HRESULT __stdcall surf_GetColorKey(Surf *s, DWORD fl, DDCOLORKEY *k)
{
    if (!(fl & DDCKEY_SRCBLT) || !s->has_ck) return DDERR_NOCOLORKEY;
    if (k) *k = s->ck;
    return DD_OK;
}
static HRESULT __stdcall surf_SetClipper(Surf *s, Clip *c) { if (c && c->hwnd && !g_hwnd) g_hwnd = c->hwnd; return DD_OK; }
static HRESULT __stdcall surf_GetDDInterface(Surf *s, void **out) { if (out) *out = NULL; return DDERR_UNSUPPORTED; }
static HRESULT __stdcall surf_GetUniquenessValue(Surf *s, DWORD *v) { if (v) *v = 1; return DD_OK; }

/* IDirect3DTexture2 view */
static HRESULT __stdcall tex2_QI(Sub *t, REFIID r, void **out) { return surf_QI(t->owner, r, out); }
static ULONG __stdcall tex2_AddRef(Sub *t) { return surf_AddRef(t->owner); }
static ULONG __stdcall tex2_Release(Sub *t) { return surf_Release(t->owner); }
static HRESULT __stdcall tex2_GetHandle(Sub *t, void *dev, D3DTEXTUREHANDLE *h) { if (h) *h = (D3DTEXTUREHANDLE)t; return D3D_OK; }
static HRESULT __stdcall tex2_PaletteChanged(Sub *t, DWORD a, DWORD b) { return D3D_OK; }
static HRESULT __stdcall tex2_Load(Sub *t, Sub *srct)
{
    Surf *d = t->owner, *s = srct ? srct->owner : NULL;
    const DDPIXELFORMAT *pf;
    int w, h, fmt = 0;
    if (!s || !s->mem) return DDERR_INVALIDPARAMS;
    w = s->desc.dwWidth; h = s->desc.dwHeight;
    pf = &s->desc.ddpfPixelFormat;
    if (pf->dwFlags & DDPF_BUMPDUDV) fmt = (pf->dwFlags & DDPF_BUMPLUMINANCE) ? (s->bpp == 32 ? 3 : 2) : 1;
    if (!d->d9tex || d->d9w != w || d->d9h != h || d->d9fmt != fmt) {
        if (d->d9tex) d9_tex_release(d->d9tex);
        d->d9tex = d9_tex_create(w, h, fmt);
        d->d9w = w; d->d9h = h; d->d9fmt = fmt;
    }
    if (!d->d9tex) return D3D_OK;
    if (fmt) d9_tex_update(d->d9tex, s->mem, s->pitch, w, h, s->bpp / 8);
    else {
        DWORD *tmp = (DWORD *)malloc(w * h * 4);
        if (!tmp) return DDERR_OUTOFMEMORY;
        to_argb(s, s->pal ? s->pal : d->pal, tmp);
        d9_tex_update(d->d9tex, tmp, w * 4, w, h, 4);
        free(tmp);
    }
    if (d->mem && d != s && d->pitch == s->pitch && d->desc.dwHeight == s->desc.dwHeight) memcpy(d->mem, s->mem, s->pitch * h);
    return D3D_OK;
}

/* IDirectDrawGammaControl view (fullscreen only in the real game; a no-op here) */
static HRESULT __stdcall gamma_GetGammaRamp(Sub *g, DWORD fl, DDGAMMARAMP *r)
{
    int i;
    if (!r) return DDERR_INVALIDPARAMS;
    for (i = 0; i < 256; i++) r->red[i] = r->green[i] = r->blue[i] = (WORD)(i * 257);
    return DD_OK;
}
static HRESULT __stdcall gamma_SetGammaRamp(Sub *g, DWORD fl, DDGAMMARAMP *r) { return DD_OK; }

/* ---- IDirectDraw / IDirectDraw4 --------------------------------------------------------------------------- */
typedef struct DD { void **vtbl; LONG ref; } DD;
static Obj *new_d3d3(void);

static HRESULT __stdcall dd_QI(DD *d, REFIID r, void **out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectDraw)) { *out = obj_new(sizeof(DD), vt_dd1); return *out ? S_OK : E_OUTOFMEMORY; }
    if (IsEqualIID(r, &IID_IDirectDraw4)) { *out = obj_new(sizeof(DD), vt_dd4); return *out ? S_OK : E_OUTOFMEMORY; }
    if (IsEqualIID(r, &IID_IDirect3D3)) { *out = new_d3d3(); return *out ? S_OK : E_OUTOFMEMORY; }
    hook_log("[be] DirectDraw QueryInterface {%08lX-...}: not supported\n", r->Data1);
    *out = NULL; return E_NOINTERFACE;
}

static HRESULT __stdcall dd_GetCaps(DD *d, DDCAPS *drv, DDCAPS *hel)
{
    DDCAPS *c[2] = { drv, hel };
    int i;
    for (i = 0; i < 2; i++) {
        DWORD size;
        DDCAPS full;
        if (!c[i]) continue;
        size = c[i]->dwSize;
        if (size < 4 || size > sizeof(DDCAPS)) size = sizeof(DDCAPS);
        memset(&full, 0, sizeof(full));
        full.dwSize = size;
        full.dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTDEPTHFILL | DDCAPS_BLTSTRETCH |
                      DDCAPS_COLORKEY | DDCAPS_GDI | DDCAPS_PALETTE | DDCAPS_CANBLTSYSMEM | DDCAPS_ZBLTS;
        full.dwCaps2 = DDCAPS2_CANRENDERWINDOWED | DDCAPS2_WIDESURFACES | DDCAPS2_PRIMARYGAMMA | DDCAPS2_NOPAGELOCKREQUIRED;
        full.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
        full.dwPalCaps = DDPCAPS_8BIT | DDPCAPS_ALLOW256;
        full.dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
        full.dwVidMemTotal = full.dwVidMemFree = 512u << 20;
        full.ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER |
                              DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY |
                              DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP | DDSCAPS_LOCALVIDMEM;
        memcpy(c[i], &full, size);
    }
    return DD_OK;
}

static void mode_desc(DDSURFACEDESC2 *d, int w, int h, int bpp)
{
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d->dwWidth = w; d->dwHeight = h; d->lPitch = w * bpp / 8; d->dwRefreshRate = 60;
    d->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d->ddpfPixelFormat.dwFlags = DDPF_RGB;
    d->ddpfPixelFormat.dwRGBBitCount = bpp;
    if (bpp == 16) { d->ddpfPixelFormat.dwRBitMask = 0xF800; d->ddpfPixelFormat.dwGBitMask = 0x7E0; d->ddpfPixelFormat.dwBBitMask = 0x1F; }
    else { d->ddpfPixelFormat.dwRBitMask = 0xFF0000; d->ddpfPixelFormat.dwGBitMask = 0xFF00; d->ddpfPixelFormat.dwBBitMask = 0xFF; }
}

static HRESULT __stdcall dd_GetDisplayMode(DD *d, DDSURFACEDESC2 *out)
{
    DDSURFACEDESC2 m;
    int w, h;
    DWORD size;
    if (!out) return DDERR_INVALIDPARAMS;
    mode_size(&w, &h);
    mode_desc(&m, w, h, g_exclusive && g_mode_set ? g_mode_bpp : 32);
    size = out->dwSize == sizeof(DDSURFACEDESC) ? sizeof(DDSURFACEDESC) : sizeof(DDSURFACEDESC2);
    memcpy(out, &m, size);   /* DDSURFACEDESC is a prefix of DDSURFACEDESC2 up to ddsCaps.dwCaps */
    out->dwSize = size;
    return DD_OK;
}

static HRESULT enum_modes(void *ctx, void *cb, int v1)
{
    DEVMODEA dm;
    DWORD i, n = 0;
    struct { DWORD w, h, bpp; } seen[256];
    for (i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
        DWORD k;
        DDSURFACEDESC2 m;
        if ((dm.dmBitsPerPel != 16 && dm.dmBitsPerPel != 32) || dm.dmPelsWidth < 640) continue;
        for (k = 0; k < n; k++)
            if (seen[k].w == dm.dmPelsWidth && seen[k].h == dm.dmPelsHeight && seen[k].bpp == dm.dmBitsPerPel) break;
        if (k < n || n >= 256) continue;
        seen[n].w = dm.dmPelsWidth; seen[n].h = dm.dmPelsHeight; seen[n].bpp = dm.dmBitsPerPel; n++;
        mode_desc(&m, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel);
        if (v1) m.dwSize = sizeof(DDSURFACEDESC);
        if (((LPDDENUMMODESCALLBACK2)cb)(&m, ctx) == DDENUMRET_CANCEL) break;
    }
    return DD_OK;
}
static HRESULT __stdcall dd1_EnumDisplayModes(DD *d, DWORD fl, void *desc, void *ctx, void *cb) { return cb ? enum_modes(ctx, cb, 1) : DDERR_INVALIDPARAMS; }
static HRESULT __stdcall dd4_EnumDisplayModes(DD *d, DWORD fl, void *desc, void *ctx, void *cb) { return cb ? enum_modes(ctx, cb, 0) : DDERR_INVALIDPARAMS; }

static HRESULT __stdcall dd_SetCooperativeLevel(DD *d, HWND w, DWORD fl)
{
    int was = g_exclusive;
    if (w) g_hwnd = w;
    g_exclusive = (fl & DDSCL_FULLSCREEN) != 0;
    hook_log("[be] SetCooperativeLevel hwnd %p flags %08X -> %s\n", w, fl, g_exclusive ? "fullscreen" : "windowed");
    if (was && !g_exclusive) { d9_window_mode(g_hwnd, 0); g_mode_set = 0; }
    return DD_OK;
}
static HRESULT set_mode(int w, int h, int bpp)
{
    g_mode_w = w; g_mode_h = h; g_mode_bpp = bpp == 16 ? 16 : 32; g_mode_set = 1;
    hook_log("[be] SetDisplayMode %dx%d %d bpp%s\n", w, h, bpp, bpp != 16 && bpp != 32 ? " (reported as 32)" : "");
    if (g_exclusive) d9_window_mode(g_hwnd, 1);
    return DD_OK;
}
static HRESULT __stdcall dd1_SetDisplayMode(DD *d, DWORD w, DWORD h, DWORD bpp) { return set_mode(w, h, bpp); }
static HRESULT __stdcall dd4_SetDisplayMode(DD *d, DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD fl) { return set_mode(w, h, bpp); }
static HRESULT __stdcall dd_RestoreDisplayMode(DD *d) { g_mode_set = 0; return DD_OK; }

static HRESULT __stdcall dd_CreatePalette(DD *d, DWORD fl, PALETTEENTRY *e, Pal **out, void *outer)
{
    Pal *p;
    if (!out) return DDERR_INVALIDPARAMS;
    p = (Pal *)obj_new(sizeof(Pal), vt_pal);
    if (!p) return DDERR_OUTOFMEMORY;
    p->caps = fl;
    if (e) memcpy(p->e, e, ((fl & DDPCAPS_4BIT) ? 16 : 256) * sizeof(PALETTEENTRY));
    *out = p; return DD_OK;
}
static HRESULT __stdcall dd_CreateClipper(DD *d, DWORD fl, Clip **out, void *outer)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = (Clip *)obj_new(sizeof(Clip), vt_clip);
    return *out ? DD_OK : DDERR_OUTOFMEMORY;
}

static HRESULT __stdcall dd4_CreateSurface(DD *d, DDSURFACEDESC2 *desc, Surf **out, void *outer)
{
    DWORD caps;
    int role, w, h;
    Surf *s;
    if (!desc || !out) return DDERR_INVALIDPARAMS;
    caps = desc->ddsCaps.dwCaps;
    w = desc->dwWidth; h = desc->dwHeight;
    if (caps & DDSCAPS_PRIMARYSURFACE) { role = R_PRIMARY; mode_size(&w, &h); }
    else if (caps & DDSCAPS_ZBUFFER) role = R_ZBUF;
    else if ((caps & DDSCAPS_3DDEVICE) && !(caps & DDSCAPS_TEXTURE)) role = R_BACK;
    else role = R_MEM;
    if (w <= 0 || h <= 0) return DDERR_INVALIDPARAMS;
    s = surf_new(desc, role, w, h);
    if (!s) return DDERR_OUTOFMEMORY;
    if (role == R_PRIMARY) {
        g_primary = s;
        if ((caps & DDSCAPS_FLIP) && (desc->dwFlags & DDSD_BACKBUFFERCOUNT) && desc->dwBackBufferCount) {
            DDSURFACEDESC2 bd = *desc;
            bd.ddsCaps.dwCaps = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER)) | DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE;
            bd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
            s->back = surf_new(&bd, R_BACK, w, h);
            s->desc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
            if (g_exclusive) d9_window_mode(g_hwnd, 1);
        }
    }
    if (role == R_BACK || (role == R_PRIMARY && s->back)) g_back_current = role == R_BACK ? s : s->back;
    {   /* one log line per distinct kind of surface */
        static DWORD seen[64][3]; static int n;
        int k;
        DWORD key0 = caps, key1 = (w << 16) | h, key2 = s->desc.ddpfPixelFormat.dwFlags ^ s->bpp;
        for (k = 0; k < n; k++) if (seen[k][0] == key0 && seen[k][1] == key1 && seen[k][2] == key2) break;
        if (k == n && n < 64) {
            seen[n][0] = key0; seen[n][1] = key1; seen[n][2] = key2; n++;
            hook_log("[be] CreateSurface role %d caps %08X %dx%d %d bpp pf %08X%s\n", role, caps, w, h, s->bpp,
                     s->desc.ddpfPixelFormat.dwFlags, s->back ? " (+ back buffer)" : "");
        }
    }
    *out = s; return DD_OK;
}
static HRESULT __stdcall dd1_CreateSurface(DD *d, void *desc, void **out, void *outer)
{
    hook_log("[be] IDirectDraw (v1) CreateSurface: not supported\n");
    return DDERR_UNSUPPORTED;
}

static HRESULT __stdcall dd_GetAvailableVidMem(DD *d, DDSCAPS2 *c, DWORD *total, DWORD *freem)
{
    if (total) *total = 512u << 20;
    if (freem) *freem = 480u << 20;
    return DD_OK;
}
static HRESULT __stdcall dd_GetDeviceIdentifier(DD *d, DDDEVICEIDENTIFIER *id, DWORD fl)
{
    if (!id) return DDERR_INVALIDPARAMS;
    memset(id, 0, sizeof(*id));
    strcpy(id->szDriver, "drakan_hook_d3d9");
    strcpy(id->szDescription, "Drakan hook D3D9 back end");
    return DD_OK;
}
static HRESULT __stdcall dd_ok0(DD *d) { return DD_OK; }                         /* Compact, FlipToGDISurface, TestCooperativeLevel, RestoreAllSurfaces */
static HRESULT __stdcall dd_GetFourCCCodes(DD *d, DWORD *n, DWORD *codes) { if (n) *n = 0; return DD_OK; }
static HRESULT __stdcall dd_GetMonitorFrequency(DD *d, DWORD *f) { if (f) *f = 60; return DD_OK; }
static HRESULT __stdcall dd_GetVerticalBlankStatus(DD *d, BOOL *b) { if (b) *b = TRUE; return DD_OK; }
static HRESULT __stdcall dd_WaitForVerticalBlank(DD *d, DWORD fl, HANDLE h) { return DD_OK; }

/* ---- Direct3D: device caps ---------------------------------------------------------------------------------- */
static void fill_devdesc(D3DDEVICEDESC *d)
{
    D3DPRIMCAPS pc;
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwFlags = D3DDD_COLORMODEL | D3DDD_DEVCAPS | D3DDD_TRANSFORMCAPS | D3DDD_LIGHTINGCAPS | D3DDD_BCLIPPING |
                 D3DDD_LINECAPS | D3DDD_TRICAPS | D3DDD_DEVICERENDERBITDEPTH | D3DDD_DEVICEZBUFFERBITDEPTH |
                 D3DDD_MAXBUFFERSIZE | D3DDD_MAXVERTEXCOUNT;
    d->dcmColorModel = D3DCOLOR_RGB;
    d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                   D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION |
                   D3DDEVCAPS_SEPARATETEXTUREMEMORIES;
    d->dtcTransformCaps.dwSize = sizeof(D3DTRANSFORMCAPS);
    d->dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
    d->bClipping = TRUE;
    d->dlcLightingCaps.dwSize = sizeof(D3DLIGHTINGCAPS);
    d->dlcLightingCaps.dwCaps = D3DLIGHTCAPS_POINT | D3DLIGHTCAPS_SPOT | D3DLIGHTCAPS_DIRECTIONAL;
    d->dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
    d->dlcLightingCaps.dwNumLights = 8;
    memset(&pc, 0, sizeof(pc));
    pc.dwSize = sizeof(pc);
    pc.dwMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW;
    pc.dwRasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_FOGVERTEX |
                      D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZBIAS |
                      D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_FOGRANGE;
    pc.dwZCmpCaps = pc.dwAlphaCmpCaps = 0xFF;
    pc.dwSrcBlendCaps = pc.dwDestBlendCaps = 0x1FFF;
    pc.dwShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND |
                     D3DPSHADECAPS_FOGGOURAUD;
    pc.dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_TRANSPARENCY |
                       D3DPTEXTURECAPS_BORDER | D3DPTEXTURECAPS_ALPHAPALETTE;
    pc.dwTextureFilterCaps = D3DPTFILTERCAPS_NEAREST | D3DPTFILTERCAPS_LINEAR | D3DPTFILTERCAPS_MIPNEAREST |
                             D3DPTFILTERCAPS_MIPLINEAR | D3DPTFILTERCAPS_LINEARMIPNEAREST | D3DPTFILTERCAPS_LINEARMIPLINEAR |
                             D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MIPFPOINT |
                             D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    pc.dwTextureBlendCaps = D3DPTBLENDCAPS_DECAL | D3DPTBLENDCAPS_MODULATE | D3DPTBLENDCAPS_DECALALPHA |
                            D3DPTBLENDCAPS_MODULATEALPHA | D3DPTBLENDCAPS_COPY | D3DPTBLENDCAPS_ADD;
    pc.dwTextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                              D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV;
    d->dpcLineCaps = pc;
    d->dpcTriCaps = pc;
    d->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    d->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24 | DDBD_32;
    d->dwMaxVertexCount = 65535;
    d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
    d->dwMaxTextureWidth = d->dwMaxTextureHeight = 4096;
    d->dwMinStippleWidth = d->dwMinStippleHeight = d->dwMaxStippleWidth = d->dwMaxStippleHeight = 0;
    d->dwMaxTextureRepeat = 8192;
    d->dwMaxTextureAspectRatio = 4096;
    d->dwMaxAnisotropy = 16;
    d->dvGuardBandLeft = -8192.0f; d->dvGuardBandTop = -8192.0f; d->dvGuardBandRight = 8192.0f; d->dvGuardBandBottom = 8192.0f;
    d->dwStencilCaps = 0xFF;
    d->dwFVFCaps = 8;
    d->dwTextureOpCaps = 0x00FFFFFF;
    d->wMaxTextureBlendStages = 8;
    d->wMaxSimultaneousTextures = 4;
}

/* ---- IDirect3D3 ---------------------------------------------------------------------------------------------- */
static Obj *new_d3d3(void) { return (Obj *)obj_new(sizeof(Obj), vt_d3d3); }

static HRESULT __stdcall d3d_QI(Obj *o, REFIID r, void **out)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirect3D3)) { o->ref++; *out = o; return S_OK; }
    if (IsEqualIID(r, &IID_IDirectDraw4)) { *out = obj_new(sizeof(DD), vt_dd4); return *out ? S_OK : E_OUTOFMEMORY; }
    *out = NULL; return E_NOINTERFACE;
}
static HRESULT __stdcall d3d_EnumDevices(Obj *o, LPD3DENUMDEVICESCALLBACK cb, void *ctx)
{
    D3DDEVICEDESC hw, hel;
    GUID g = IID_IDirect3DHALDevice;
    if (!cb) return DDERR_INVALIDPARAMS;
    fill_devdesc(&hw); fill_devdesc(&hel);
    cb(&g, "Direct3D HAL (drakan_hook D3D9)", "Direct3D HAL", &hw, &hel, ctx);
    return D3D_OK;
}
static HRESULT __stdcall d3d_EnumZBufferFormats(Obj *o, REFCLSID cls, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    DDPIXELFORMAT pf[3];
    int i;
    if (!cb) return DDERR_INVALIDPARAMS;
    memset(pf, 0, sizeof(pf));
    for (i = 0; i < 3; i++) { pf[i].dwSize = sizeof(DDPIXELFORMAT); pf[i].dwFlags = DDPF_ZBUFFER; }
    pf[0].dwRGBBitCount = 16; pf[0].dwGBitMask = 0xFFFF;                         /* dwZBitMask */
    pf[1].dwRGBBitCount = 24; pf[1].dwGBitMask = 0xFFFFFF;
    pf[2].dwFlags |= DDPF_STENCILBUFFER; pf[2].dwRGBBitCount = 32; pf[2].dwRBitMask = 8;   /* stencil depth */
    pf[2].dwGBitMask = 0xFFFFFF; pf[2].dwBBitMask = 0xFF000000;                  /* z mask, stencil mask */
    for (i = 0; i < 3; i++) if (cb(&pf[i], ctx) == D3DENUMRET_CANCEL) break;
    return D3D_OK;
}

static HRESULT __stdcall d3d_CreateDevice(Obj *o, REFCLSID cls, Surf *target, Dev **out, void *outer)
{
    Dev *d;
    HWND w = g_hwnd;
    if (!target || !out) return DDERR_INVALIDPARAMS;
    if (!w) { hook_log("[be] CreateDevice without a window\n"); return DDERR_GENERIC; }
    if (!d9_device(w, target->desc.dwWidth, target->desc.dwHeight)) return DDERR_GENERIC;
    d = (Dev *)obj_new(sizeof(Dev), vt_dev3);
    if (!d) return DDERR_OUTOFMEMORY;
    d->target = target; target->ref++;
    g_back_current = target;
    hook_log("[be] CreateDevice on %dx%d -> %p\n", target->desc.dwWidth, target->desc.dwHeight, d);
    hook_log_flush();
    *out = d; return D3D_OK;
}
static HRESULT __stdcall d3d_CreateViewport(Obj *o, Vp **out, void *outer)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = (Vp *)obj_new(sizeof(Vp), vt_vp3);
    return *out ? D3D_OK : DDERR_OUTOFMEMORY;
}
static HRESULT __stdcall d3d_CreateMaterial(Obj *o, Mat **out, void *outer)
{
    if (!out) return DDERR_INVALIDPARAMS;
    *out = (Mat *)obj_new(sizeof(Mat), vt_mat3);
    return *out ? D3D_OK : DDERR_OUTOFMEMORY;
}
static HRESULT __stdcall d3d_EvictManagedTextures(Obj *o) { return D3D_OK; }

/* ---- IDirect3DDevice3 -------------------------------------------------------------------------------------- */
static HRESULT __stdcall dev_QI(Dev *d, REFIID r, void **out)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirect3DDevice3)) { d->ref++; *out = d; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG __stdcall dev_Release(Dev *d)
{
    LONG r = --d->ref;
    if (r > 0) return r;
    {
        int i;
        for (i = 0; i < 8; i++) if (d->tex[i]) { surf_Release(d->tex[i]->owner); d->tex[i] = NULL; d9_set_texture(i, NULL); }
    }
    if (d->target) surf_Release(d->target);
    if (d->cur) gen_Release((Obj *)d->cur);
    free(d);
    return 0;
}
static HRESULT __stdcall dev_GetCaps(Dev *d, D3DDEVICEDESC *hw, D3DDEVICEDESC *hel)
{
    D3DDEVICEDESC t;
    fill_devdesc(&t);
    if (hw) { DWORD s = hw->dwSize && hw->dwSize < sizeof(t) ? hw->dwSize : sizeof(t); memcpy(hw, &t, s); hw->dwSize = s; }
    if (hel) { DWORD s = hel->dwSize && hel->dwSize < sizeof(t) ? hel->dwSize : sizeof(t); memcpy(hel, &t, s); hel->dwSize = s; }
    return D3D_OK;
}
static void apply_viewport(Vp *v) { if (v && v->w && v->h) d9_viewport(v->x, v->y, v->w, v->h); }
static HRESULT __stdcall dev_AddViewport(Dev *d, Vp *v) { if (v) { v->ref++; v->dev = (struct Dev *)d; } return D3D_OK; }
static HRESULT __stdcall dev_DeleteViewport(Dev *d, Vp *v)
{
    if (!v) return DDERR_INVALIDPARAMS;
    if (d->cur == v) { d->cur = NULL; gen_Release((Obj *)v); }
    v->dev = NULL;
    gen_Release((Obj *)v);
    return D3D_OK;
}
static HRESULT __stdcall dev_SetCurrentViewport(Dev *d, Vp *v)
{
    if (v) v->ref++;
    if (d->cur) gen_Release((Obj *)d->cur);
    d->cur = v;
    apply_viewport(v);
    return D3D_OK;
}
static HRESULT __stdcall dev_GetCurrentViewport(Dev *d, Vp **out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    if (d->cur) d->cur->ref++;
    *out = d->cur; return d->cur ? D3D_OK : D3DERR_NOCURRENTVIEWPORT;
}
static HRESULT __stdcall dev_EnumTextureFormats(Dev *d, LPD3DENUMPIXELFORMATSCALLBACK cb, void *ctx)
{
    /* Same order as dgVoodoo (F-036), without DXT / YUV / luminance. */
    static const DWORD f[][6] = {   /* flags, bpp, R, G, B, A */
        { DDPF_RGB | DDPF_PALETTEINDEXED8, 8, 0, 0, 0, 0 },
        { DDPF_RGB, 16, 0x7C00, 0x3E0, 0x1F, 0 },
        { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7C00, 0x3E0, 0x1F, 0x8000 },
        { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0xF00, 0xF0, 0xF, 0xF000 },
        { DDPF_RGB, 16, 0xF800, 0x7E0, 0x1F, 0 },
        { DDPF_RGB, 32, 0xFF0000, 0xFF00, 0xFF, 0 },
        { DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000 },
        { DDPF_BUMPDUDV, 16, 0xFF, 0xFF00, 0, 0 },
        { DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE, 16, 0x1F, 0x3E0, 0xFC00, 0 },
        { DDPF_BUMPDUDV | DDPF_BUMPLUMINANCE, 32, 0xFF, 0xFF00, 0xFF0000, 0 },
    };
    int i, only32 = hook_ini_int("backend", "textures_32bit", 0);
    if (!cb) return DDERR_INVALIDPARAMS;
    for (i = 0; i < (int)(sizeof(f) / sizeof(f[0])); i++) {
        DDPIXELFORMAT pf;
        if (only32 && (f[i][0] & DDPF_RGB) && f[i][1] != 32) continue;
        memset(&pf, 0, sizeof(pf));
        pf.dwSize = sizeof(pf); pf.dwFlags = f[i][0]; pf.dwRGBBitCount = f[i][1];
        pf.dwRBitMask = f[i][2]; pf.dwGBitMask = f[i][3]; pf.dwBBitMask = f[i][4]; pf.dwRGBAlphaBitMask = f[i][5];
        if (cb(&pf, ctx) == D3DENUMRET_CANCEL) break;
    }
    return D3D_OK;
}
static HRESULT __stdcall dev_BeginScene(Dev *d) { d9_begin_scene(); return D3D_OK; }
static HRESULT __stdcall dev_EndScene(Dev *d) { d9_end_scene(); return D3D_OK; }
static HRESULT __stdcall dev_GetDirect3D(Dev *d, Obj **out) { if (!out) return DDERR_INVALIDPARAMS; *out = new_d3d3(); return D3D_OK; }
static HRESULT __stdcall dev_GetRenderTarget(Dev *d, Surf **out)
{
    if (!out) return DDERR_INVALIDPARAMS;
    if (d->target) d->target->ref++;
    *out = d->target; return D3D_OK;
}
static HRESULT __stdcall dev_GetRenderState(Dev *d, DWORD st, DWORD *v) { if (!v) return DDERR_INVALIDPARAMS; *v = st < 256 ? d->rs[st] : 0; return D3D_OK; }
static HRESULT __stdcall dev_SetRenderState(Dev *d, DWORD st, DWORD v)
{
    if (st < 256) d->rs[st] = v;
    d9_render_state(st, v);
    return D3D_OK;
}
static HRESULT __stdcall dev_GetTextureStageState(Dev *d, DWORD stage, DWORD t, DWORD *v)
{
    if (!v) return DDERR_INVALIDPARAMS;
    *v = stage < 8 && t < 32 ? d->tss[stage][t] : 0; return D3D_OK;
}
static HRESULT __stdcall dev_SetTextureStageState(Dev *d, DWORD stage, DWORD t, DWORD v)
{
    if (stage < 8 && t < 32) d->tss[stage][t] = v;
    d9_stage_state(stage, t, v);
    return D3D_OK;
}
static HRESULT __stdcall dev_SetTexture(Dev *d, DWORD stage, Sub *t)
{
    if (stage >= 8) return DDERR_INVALIDPARAMS;
    if (t) surf_AddRef(t->owner);                 /* DX6 SetTexture keeps a reference */
    if (d->tex[stage]) surf_Release(d->tex[stage]->owner);
    d->tex[stage] = t;
    d9_set_texture(stage, t ? t->owner->d9tex : NULL);
    return D3D_OK;
}
static HRESULT __stdcall dev_GetTexture(Dev *d, DWORD stage, Sub **out)
{
    if (!out || stage >= 8) return DDERR_INVALIDPARAMS;
    *out = d->tex[stage];
    if (*out) surf_AddRef((*out)->owner);
    return D3D_OK;
}
static HRESULT __stdcall dev_DrawPrimitive(Dev *d, D3DPRIMITIVETYPE type, DWORD fvf, void *v, DWORD n, DWORD fl)
{
    int i;
    /* a texture re-uploaded since SetTexture (animated textures) may have a new D3D9 object */
    for (i = 0; i < 2; i++) if (d->tex[i]) d9_set_texture(i, d->tex[i]->owner->d9tex);
    d9_draw(type, fvf, v, n);
    return D3D_OK;
}
static HRESULT __stdcall dev_ValidateDevice(Dev *d, DWORD *passes) { if (passes) *passes = 1; return D3D_OK; }
static HRESULT __stdcall dev_ok2(Dev *d, DWORD a, DWORD b) { return D3D_OK; }   /* SetLightState, SetTransform etc. */

/* ---- IDirect3DViewport3 ----------------------------------------------------------------------------------- */
static HRESULT __stdcall vp_QI(Vp *v, REFIID r, void **out) { v->ref++; *out = v; return S_OK; }
static HRESULT __stdcall vp_SetViewport(Vp *v, D3DVIEWPORT *d)
{
    if (!d) return DDERR_INVALIDPARAMS;
    v->x = d->dwX; v->y = d->dwY; v->w = d->dwWidth; v->h = d->dwHeight;
    { static int n; if (n++ < 4) hook_log("[be] SetViewport %u,%u %ux%u\n", v->x, v->y, v->w, v->h); }
    if (v->dev && ((Dev *)v->dev)->cur == v) apply_viewport(v);
    return D3D_OK;
}
static HRESULT __stdcall vp_GetViewport(Vp *v, D3DVIEWPORT *d)
{
    if (!d) return DDERR_INVALIDPARAMS;
    memset((BYTE *)d + 4, 0, sizeof(*d) - 4);
    d->dwX = v->x; d->dwY = v->y; d->dwWidth = v->w; d->dwHeight = v->h; d->dvMaxZ = 1.0f;
    return D3D_OK;
}
static HRESULT __stdcall vp_SetViewport2(Vp *v, D3DVIEWPORT2 *d)
{
    if (!d) return DDERR_INVALIDPARAMS;
    v->x = d->dwX; v->y = d->dwY; v->w = d->dwWidth; v->h = d->dwHeight;
    if (v->dev && ((Dev *)v->dev)->cur == v) apply_viewport(v);
    return D3D_OK;
}
static HRESULT __stdcall vp_GetViewport2(Vp *v, D3DVIEWPORT2 *d)
{
    if (!d) return DDERR_INVALIDPARAMS;
    memset((BYTE *)d + 4, 0, sizeof(*d) - 4);
    d->dwX = v->x; d->dwY = v->y; d->dwWidth = v->w; d->dwHeight = v->h; d->dvMaxZ = 1.0f;
    return D3D_OK;
}
static HRESULT __stdcall vp_Clear(Vp *v, DWORD n, D3DRECT *r, DWORD fl)
{
    d9_clear((fl & D3DCLEAR_TARGET ? 1 : 0) | (fl & D3DCLEAR_ZBUFFER ? 2 : 0), 0, 1.0f, 0);
    return D3D_OK;
}
static HRESULT __stdcall vp_Clear2(Vp *v, DWORD n, D3DRECT *r, DWORD fl, D3DCOLOR c, D3DVALUE z, DWORD st)
{
    d9_clear((fl & D3DCLEAR_TARGET ? 1 : 0) | (fl & D3DCLEAR_ZBUFFER ? 2 : 0) | (fl & D3DCLEAR_STENCIL ? 4 : 0), c, z, st);
    return D3D_OK;
}
static HRESULT __stdcall vp_ok1(Vp *v, DWORD a) { return D3D_OK; }   /* SetBackground, SetBackgroundDepth(2), Initialize */

/* ---- vtables ------------------------------------------------------------------------------------------------- */
#define M(name, n, fn) { name, n, (void *)(fn) }
static void build_all(void)
{
    static const Meth dd1[23] = {
        M("IDirectDraw::QueryInterface", 2, dd_QI), M("AddRef", 0, gen_AddRef), M("Release", 0, gen_Release),
        M("IDirectDraw::Compact", 0, dd_ok0), M("IDirectDraw::CreateClipper", 3, dd_CreateClipper),
        M("IDirectDraw::CreatePalette", 4, dd_CreatePalette), M("IDirectDraw::CreateSurface", 3, dd1_CreateSurface),
        M("IDirectDraw::DuplicateSurface", 2, 0), M("IDirectDraw::EnumDisplayModes", 4, dd1_EnumDisplayModes),
        M("IDirectDraw::EnumSurfaces", 4, 0), M("IDirectDraw::FlipToGDISurface", 0, dd_ok0),
        M("IDirectDraw::GetCaps", 2, dd_GetCaps), M("IDirectDraw::GetDisplayMode", 1, dd_GetDisplayMode),
        M("IDirectDraw::GetFourCCCodes", 2, dd_GetFourCCCodes), M("IDirectDraw::GetGDISurface", 1, 0),
        M("IDirectDraw::GetMonitorFrequency", 1, dd_GetMonitorFrequency), M("IDirectDraw::GetScanLine", 1, 0),
        M("IDirectDraw::GetVerticalBlankStatus", 1, dd_GetVerticalBlankStatus), M("IDirectDraw::Initialize", 1, 0),
        M("IDirectDraw::RestoreDisplayMode", 0, dd_RestoreDisplayMode),
        M("IDirectDraw::SetCooperativeLevel", 2, dd_SetCooperativeLevel), M("IDirectDraw::SetDisplayMode", 3, dd1_SetDisplayMode),
        M("IDirectDraw::WaitForVerticalBlank", 2, dd_WaitForVerticalBlank),
    };
    static const Meth dd4[28] = {
        M("IDirectDraw4::QueryInterface", 2, dd_QI), M("AddRef", 0, gen_AddRef), M("Release", 0, gen_Release),
        M("IDirectDraw4::Compact", 0, dd_ok0), M("IDirectDraw4::CreateClipper", 3, dd_CreateClipper),
        M("IDirectDraw4::CreatePalette", 4, dd_CreatePalette), M("IDirectDraw4::CreateSurface", 3, dd4_CreateSurface),
        M("IDirectDraw4::DuplicateSurface", 2, 0), M("IDirectDraw4::EnumDisplayModes", 4, dd4_EnumDisplayModes),
        M("IDirectDraw4::EnumSurfaces", 4, 0), M("IDirectDraw4::FlipToGDISurface", 0, dd_ok0),
        M("IDirectDraw4::GetCaps", 2, dd_GetCaps), M("IDirectDraw4::GetDisplayMode", 1, dd_GetDisplayMode),
        M("IDirectDraw4::GetFourCCCodes", 2, dd_GetFourCCCodes), M("IDirectDraw4::GetGDISurface", 1, 0),
        M("IDirectDraw4::GetMonitorFrequency", 1, dd_GetMonitorFrequency), M("IDirectDraw4::GetScanLine", 1, 0),
        M("IDirectDraw4::GetVerticalBlankStatus", 1, dd_GetVerticalBlankStatus), M("IDirectDraw4::Initialize", 1, 0),
        M("IDirectDraw4::RestoreDisplayMode", 0, dd_RestoreDisplayMode),
        M("IDirectDraw4::SetCooperativeLevel", 2, dd_SetCooperativeLevel), M("IDirectDraw4::SetDisplayMode", 5, dd4_SetDisplayMode),
        M("IDirectDraw4::WaitForVerticalBlank", 2, dd_WaitForVerticalBlank),
        M("IDirectDraw4::GetAvailableVidMem", 3, dd_GetAvailableVidMem), M("IDirectDraw4::GetSurfaceFromDC", 2, 0),
        M("IDirectDraw4::RestoreAllSurfaces", 0, dd_ok0), M("IDirectDraw4::TestCooperativeLevel", 0, dd_ok0),
        M("IDirectDraw4::GetDeviceIdentifier", 2, dd_GetDeviceIdentifier),
    };
    static const Meth surf[45] = {
        M("Surface::QueryInterface", 2, surf_QI), M("Surface::AddRef", 0, surf_AddRef), M("Surface::Release", 0, surf_Release),
        M("Surface::AddAttachedSurface", 1, surf_AddAttachedSurface), M("Surface::AddOverlayDirtyRect", 1, 0),
        M("Surface::Blt", 5, surf_Blt), M("Surface::BltBatch", 3, 0), M("Surface::BltFast", 5, surf_BltFast),
        M("Surface::DeleteAttachedSurface", 2, surf_DeleteAttachedSurface), M("Surface::EnumAttachedSurfaces", 2, 0),
        M("Surface::EnumOverlayZOrders", 3, 0), M("Surface::Flip", 2, surf_Flip),
        M("Surface::GetAttachedSurface", 2, surf_GetAttachedSurface), M("Surface::GetBltStatus", 1, 0),
        M("Surface::GetCaps", 1, surf_GetCaps), M("Surface::GetClipper", 1, 0), M("Surface::GetColorKey", 2, surf_GetColorKey),
        M("Surface::GetDC", 1, 0), M("Surface::GetFlipStatus", 1, 0), M("Surface::GetOverlayPosition", 2, 0),
        M("Surface::GetPalette", 1, surf_GetPalette), M("Surface::GetPixelFormat", 1, surf_GetPixelFormat),
        M("Surface::GetSurfaceDesc", 1, surf_GetSurfaceDesc), M("Surface::Initialize", 2, 0),
        M("Surface::IsLost", 0, surf_ok0), M("Surface::Lock", 4, surf_Lock), M("Surface::ReleaseDC", 1, 0),
        M("Surface::Restore", 0, surf_ok0), M("Surface::SetClipper", 1, surf_SetClipper),
        M("Surface::SetColorKey", 2, surf_SetColorKey), M("Surface::SetOverlayPosition", 2, 0),
        M("Surface::SetPalette", 1, surf_SetPalette), M("Surface::Unlock", 1, surf_Unlock),
        M("Surface::UpdateOverlay", 5, 0), M("Surface::UpdateOverlayDisplay", 1, 0), M("Surface::UpdateOverlayZOrder", 2, 0),
        M("Surface::GetDDInterface", 1, surf_GetDDInterface), M("Surface::PageLock", 1, 0), M("Surface::PageUnlock", 1, 0),
        M("Surface::SetSurfaceDesc", 2, 0), M("Surface::SetPrivateData", 4, 0), M("Surface::GetPrivateData", 3, 0),
        M("Surface::FreePrivateData", 1, 0), M("Surface::GetUniquenessValue", 1, surf_GetUniquenessValue),
        M("Surface::ChangeUniquenessValue", 0, surf_ok0),
    };
    static const Meth tex2[6] = {
        M("Texture2::QueryInterface", 2, tex2_QI), M("Texture2::AddRef", 0, tex2_AddRef), M("Texture2::Release", 0, tex2_Release),
        M("Texture2::GetHandle", 2, tex2_GetHandle), M("Texture2::PaletteChanged", 2, tex2_PaletteChanged),
        M("Texture2::Load", 1, tex2_Load),
    };
    static const Meth gam[5] = {
        M("Gamma::QueryInterface", 2, tex2_QI), M("Gamma::AddRef", 0, tex2_AddRef), M("Gamma::Release", 0, tex2_Release),
        M("Gamma::GetGammaRamp", 2, gamma_GetGammaRamp), M("Gamma::SetGammaRamp", 2, gamma_SetGammaRamp),
    };
    static const Meth pal[7] = {
        M("Palette::QueryInterface", 2, pal_QI), M("Palette::AddRef", 0, gen_AddRef), M("Palette::Release", 0, gen_Release),
        M("Palette::GetCaps", 1, pal_GetCaps), M("Palette::GetEntries", 4, pal_GetEntries), M("Palette::Initialize", 3, 0),
        M("Palette::SetEntries", 4, pal_SetEntries),
    };
    static const Meth clip[9] = {
        M("Clipper::QueryInterface", 2, clip_QI), M("Clipper::AddRef", 0, gen_AddRef), M("Clipper::Release", 0, gen_Release),
        M("Clipper::GetClipList", 3, 0), M("Clipper::GetHWnd", 1, clip_GetHWnd), M("Clipper::Initialize", 2, 0),
        M("Clipper::IsClipListChanged", 1, clip_IsClipListChanged), M("Clipper::SetClipList", 2, 0),
        M("Clipper::SetHWnd", 2, clip_SetHWnd),
    };
    static const Meth d3d3[12] = {
        M("D3D3::QueryInterface", 2, d3d_QI), M("D3D3::AddRef", 0, gen_AddRef), M("D3D3::Release", 0, gen_Release),
        M("D3D3::EnumDevices", 2, d3d_EnumDevices), M("D3D3::CreateLight", 2, 0), M("D3D3::CreateMaterial", 2, d3d_CreateMaterial),
        M("D3D3::CreateViewport", 2, d3d_CreateViewport), M("D3D3::FindDevice", 2, 0), M("D3D3::CreateDevice", 4, d3d_CreateDevice),
        M("D3D3::CreateVertexBuffer", 4, 0), M("D3D3::EnumZBufferFormats", 3, d3d_EnumZBufferFormats),
        M("D3D3::EvictManagedTextures", 0, d3d_EvictManagedTextures),
    };
    static const Meth dev3[42] = {
        M("Device3::QueryInterface", 2, dev_QI), M("Device3::AddRef", 0, gen_AddRef), M("Device3::Release", 0, dev_Release),
        M("Device3::GetCaps", 2, dev_GetCaps), M("Device3::GetStats", 1, 0), M("Device3::AddViewport", 1, dev_AddViewport),
        M("Device3::DeleteViewport", 1, dev_DeleteViewport), M("Device3::NextViewport", 3, 0),
        M("Device3::EnumTextureFormats", 2, dev_EnumTextureFormats), M("Device3::BeginScene", 0, dev_BeginScene),
        M("Device3::EndScene", 0, dev_EndScene), M("Device3::GetDirect3D", 1, dev_GetDirect3D),
        M("Device3::SetCurrentViewport", 1, dev_SetCurrentViewport), M("Device3::GetCurrentViewport", 1, dev_GetCurrentViewport),
        M("Device3::SetRenderTarget", 2, 0), M("Device3::GetRenderTarget", 1, dev_GetRenderTarget),
        M("Device3::Begin", 3, 0), M("Device3::BeginIndexed", 5, 0), M("Device3::Vertex", 1, 0), M("Device3::Index", 1, 0),
        M("Device3::End", 1, 0), M("Device3::GetRenderState", 2, dev_GetRenderState),
        M("Device3::SetRenderState", 2, dev_SetRenderState), M("Device3::GetLightState", 2, 0),
        M("Device3::SetLightState", 2, dev_ok2), M("Device3::SetTransform", 2, dev_ok2), M("Device3::GetTransform", 2, 0),
        M("Device3::MultiplyTransform", 2, 0), M("Device3::DrawPrimitive", 5, dev_DrawPrimitive),
        M("Device3::DrawIndexedPrimitive", 7, 0), M("Device3::SetClipStatus", 1, 0), M("Device3::GetClipStatus", 1, 0),
        M("Device3::DrawPrimitiveStrided", 5, 0), M("Device3::DrawIndexedPrimitiveStrided", 7, 0),
        M("Device3::DrawPrimitiveVB", 5, 0), M("Device3::DrawIndexedPrimitiveVB", 6, 0),
        M("Device3::ComputeSphereVisibility", 5, 0), M("Device3::GetTexture", 2, dev_GetTexture),
        M("Device3::SetTexture", 2, dev_SetTexture), M("Device3::GetTextureStageState", 3, dev_GetTextureStageState),
        M("Device3::SetTextureStageState", 3, dev_SetTextureStageState), M("Device3::ValidateDevice", 1, dev_ValidateDevice),
    };
    static const Meth vp3[21] = {
        M("Viewport3::QueryInterface", 2, vp_QI), M("Viewport3::AddRef", 0, gen_AddRef), M("Viewport3::Release", 0, gen_Release),
        M("Viewport3::Initialize", 1, vp_ok1), M("Viewport3::GetViewport", 1, vp_GetViewport),
        M("Viewport3::SetViewport", 1, vp_SetViewport), M("Viewport3::TransformVertices", 4, 0),
        M("Viewport3::LightElements", 2, 0), M("Viewport3::SetBackground", 1, vp_ok1), M("Viewport3::GetBackground", 2, 0),
        M("Viewport3::SetBackgroundDepth", 1, vp_ok1), M("Viewport3::GetBackgroundDepth", 2, 0),
        M("Viewport3::Clear", 3, vp_Clear), M("Viewport3::AddLight", 1, 0), M("Viewport3::DeleteLight", 1, 0),
        M("Viewport3::NextLight", 3, 0), M("Viewport3::GetViewport2", 1, vp_GetViewport2),
        M("Viewport3::SetViewport2", 1, vp_SetViewport2), M("Viewport3::SetBackgroundDepth2", 1, vp_ok1),
        M("Viewport3::GetBackgroundDepth2", 2, 0), M("Viewport3::Clear2", 6, vp_Clear2),
    };
    static const Meth mat3[6] = {
        M("Material3::QueryInterface", 2, mat_QI), M("Material3::AddRef", 0, gen_AddRef), M("Material3::Release", 0, gen_Release),
        M("Material3::SetMaterial", 1, mat_SetMaterial), M("Material3::GetMaterial", 1, mat_GetMaterial),
        M("Material3::GetHandle", 2, mat_GetHandle),
    };
    build_vtbl(vt_dd1, dd1, 23); build_vtbl(vt_dd4, dd4, 28); build_vtbl(vt_surf, surf, 45);
    build_vtbl(vt_tex2, tex2, 6); build_vtbl(vt_gamma, gam, 5); build_vtbl(vt_pal, pal, 7); build_vtbl(vt_clip, clip, 9);
    build_vtbl(vt_d3d3, d3d3, 12); build_vtbl(vt_dev3, dev3, 42); build_vtbl(vt_vp3, vp3, 21); build_vtbl(vt_mat3, mat3, 6);
}

/* ---- exports -------------------------------------------------------------------------------------------------- */
HRESULT be_DirectDrawCreate(GUID *guid, void **out, void *outer)
{
    static int built;
    if (!out) return DDERR_INVALIDPARAMS;
    if (!built) { build_all(); built = 1; hook_log("[be] D3D9 back end active (%d stub methods)\n", g_nstub); }
    *out = obj_new(sizeof(DD), vt_dd1);
    return *out ? DD_OK : DDERR_OUTOFMEMORY;
}

HRESULT be_DirectDrawEnumerateA(void *cb, void *ctx)
{
    if (!cb) return DDERR_INVALIDPARAMS;
    ((LPDDENUMCALLBACKA)cb)(NULL, "Primary Display Driver", "display", ctx);
    return DD_OK;
}
