/*
 * be_d9.c - M1 back end, D3D9 side (see be.h). Only this file includes d3d9.h.
 *
 * - The device is windowed, always. The game's "fullscreen" is a borderless window over the monitor
 *   (d9_window_mode), its back buffer stays at the game's resolution and Present scales it (letterboxed).
 * - The game's draws are all triangle fans of pre-transformed vertices (FVF 0x1C4 / 0x4C4, F-036).
 *   Consecutive fans with the same FVF and states are merged into one triangle list (REMIX.md 5.10),
 *   flushed on any real state change, Clear, EndScene, Present or read-back.
 * - Render-state and texture-stage numbers are DX6's; most are the same in D3D9. The exceptions are mapped
 *   here (ZBIAS, the sampler states, mip-filter values) or ignored (ANTIALIAS, TEXTUREPERSPECTIVE, SUBPIXEL...).
 * - All states are cached, so redundant sets don't break batches, and the cache is re-applied after Reset.
 */
#define CINTERFACE
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <d3d9.h>
#include "hook.h"
#include "be.h"

typedef IDirect3D9 *(WINAPI *create9_fn)(UINT);

static IDirect3D9       *g_d3d;
static IDirect3DDevice9 *g_dev;
static HWND   g_hwnd;
static int    g_w, g_h, g_in_scene, g_lost, g_fullscreen;
static LONG   g_saved_style, g_saved_exstyle;
static RECT   g_saved_rect;

/* state cache (DX6 numbering for render states; D3D9 numbering for stage and sampler states) */
#define NRS 256
static DWORD g_rs[NRS];   static BYTE g_rs_set[NRS];
static DWORD g_tss[8][33]; static BYTE g_tss_set[8][33];
static DWORD g_samp[8][14]; static BYTE g_samp_set[8][14];
static void *g_tex[8];

/* batch */
#define BATCH_BYTES (1 << 21)
static BYTE  *g_batch;
static DWORD  g_batch_fvf, g_batch_stride, g_batch_verts;

static DWORD fvf_stride(DWORD fvf)
{
    DWORD s = 0;
    if ((fvf & 0x0E) == 0x04) s += 16;          /* XYZRHW */
    else if ((fvf & 0x0E) == 0x02) s += 12;     /* XYZ */
    if (fvf & 0x10) s += 12;                    /* NORMAL */
    if (fvf & 0x40) s += 4;                     /* DIFFUSE */
    if (fvf & 0x80) s += 4;                     /* SPECULAR */
    s += ((fvf >> 8) & 0xF) * 8;                /* TEXn, 2 floats each */
    return s;
}

static void flush(void)
{
    if (!g_dev || !g_batch_verts) { g_batch_verts = 0; return; }
    IDirect3DDevice9_SetFVF(g_dev, g_batch_fvf);
    IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, g_batch_verts / 3, g_batch, g_batch_stride);
    g_batch_verts = 0;
}

/* ---- device ------------------------------------------------------------------------------------- */
static void apply_rs(DWORD st, DWORD v);
static void apply_cache(void)
{
    int i, s;
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CLIPPING, TRUE);
    for (i = 0; i < NRS; i++) if (g_rs_set[i]) apply_rs(i, g_rs[i]);
    for (s = 0; s < 8; s++) {
        for (i = 0; i < 33; i++)
            if (g_tss_set[s][i]) IDirect3DDevice9_SetTextureStageState(g_dev, s, (D3DTEXTURESTAGESTATETYPE)i, g_tss[s][i]);
        for (i = 0; i < 14; i++)
            if (g_samp_set[s][i]) IDirect3DDevice9_SetSamplerState(g_dev, s, (D3DSAMPLERSTATETYPE)i, g_samp[s][i]);
        IDirect3DDevice9_SetTexture(g_dev, s, (IDirect3DBaseTexture9 *)g_tex[s]);
    }
}

static void fill_pp(D3DPRESENT_PARAMETERS *pp)
{
    memset(pp, 0, sizeof(*pp));
    pp->BackBufferWidth = g_w; pp->BackBufferHeight = g_h;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    pp->BackBufferCount = 1;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->hDeviceWindow = g_hwnd;
    pp->Windowed = TRUE;
    pp->EnableAutoDepthStencil = TRUE;
    pp->AutoDepthStencilFormat = D3DFMT_D24S8;
    pp->PresentationInterval = hook_ini_int("backend", "vsync", 1) ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

static int do_reset(void)
{
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    fill_pp(&pp);
    hr = IDirect3DDevice9_Reset(g_dev, &pp);
    hook_log("[d9] Reset %dx%d -> hr %08X\n", g_w, g_h, (DWORD)hr);
    if (FAILED(hr)) { g_lost = 1; return 0; }
    g_lost = 0;
    apply_cache();
    return 1;
}

int d9_ready(void) { return g_dev != NULL; }

int d9_device(HWND hwnd, int w, int h)
{
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    if (w <= 0 || h <= 0) return 0;
    if (g_dev) {
        if (w == g_w && h == g_h && hwnd == g_hwnd) return 1;
        flush();
        if (g_in_scene) { IDirect3DDevice9_EndScene(g_dev); g_in_scene = 0; }
        g_w = w; g_h = h;
        if (hwnd != g_hwnd) hook_log("[d9] note: device window changes %p -> %p (kept the first)\n", g_hwnd, hwnd);
        return do_reset();
    }
    if (!g_d3d) {
        char path[MAX_PATH];
        HMODULE m;
        create9_fn create;
        /* [backend] d3d9_path: empty = the system d3d9.dll; otherwise relative to the game folder or absolute */
        hook_ini_path("backend", "d3d9_path", "d3d9.dll", path, sizeof(path));
        m = LoadLibraryA(path);
        create = m ? (create9_fn)GetProcAddress(m, "Direct3DCreate9") : NULL;
        g_d3d = create ? create(D3D_SDK_VERSION) : NULL;
        hook_log("[d9] d3d9 = %s -> %s\n", path, g_d3d ? "ok" : "FAILED");
        if (!g_d3d) return 0;
    }
    if (!g_batch) g_batch = (BYTE *)VirtualAlloc(NULL, BATCH_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_hwnd = hwnd; g_w = w; g_h = h;
    fill_pp(&pp);
    hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev);
    hook_log("[d9] CreateDevice %dx%d on hwnd %p -> hr %08X\n", w, h, hwnd, (DWORD)hr);
    hook_log_flush();
    if (FAILED(hr)) { g_dev = NULL; return 0; }
    apply_cache();
    return 1;
}

void d9_window_mode(HWND hwnd, int fullscreen)
{
    if (!hwnd || fullscreen == g_fullscreen) return;
    if (fullscreen) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        g_saved_style = GetWindowLongA(hwnd, GWL_STYLE);
        g_saved_exstyle = GetWindowLongA(hwnd, GWL_EXSTYLE);
        GetWindowRect(hwnd, &g_saved_rect);
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(hwnd, GWL_STYLE, (g_saved_style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP);
        SetWindowLongA(hwnd, GWL_EXSTYLE, g_saved_exstyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, g_saved_style);
        SetWindowLongA(hwnd, GWL_EXSTYLE, g_saved_exstyle);
        SetWindowPos(hwnd, HWND_NOTOPMOST, g_saved_rect.left, g_saved_rect.top, g_saved_rect.right - g_saved_rect.left,
                     g_saved_rect.bottom - g_saved_rect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_fullscreen = fullscreen;
    hook_log("[d9] window mode: %s\n", fullscreen ? "borderless fullscreen" : "windowed");
}

static int check_lost(void)
{
    HRESULT hr;
    if (!g_dev) return 1;
    if (!g_lost) return 0;
    hr = IDirect3DDevice9_TestCooperativeLevel(g_dev);
    if (hr == D3DERR_DEVICENOTRESET) do_reset();
    return g_lost;
}

void d9_begin_scene(void)
{
    if (check_lost() || g_in_scene) return;
    if (SUCCEEDED(IDirect3DDevice9_BeginScene(g_dev))) g_in_scene = 1;
}

void d9_end_scene(void)
{
    if (!g_dev) return;
    flush();
    if (g_in_scene) { IDirect3DDevice9_EndScene(g_dev); g_in_scene = 0; }
}

void d9_present(void)
{
    HRESULT hr;
    RECT dst, *pdst = NULL;
    if (!g_dev) return;
    d9_end_scene();
    if (g_fullscreen) {   /* letterbox the game's resolution into the borderless window */
        RECT c;
        int cw, ch, w, h;
        GetClientRect(g_hwnd, &c);
        cw = c.right; ch = c.bottom;
        if (cw > 0 && ch > 0) {
            if (cw * g_h > ch * g_w) { h = ch; w = ch * g_w / g_h; } else { w = cw; h = cw * g_h / g_w; }
            dst.left = (cw - w) / 2; dst.top = (ch - h) / 2; dst.right = dst.left + w; dst.bottom = dst.top + h;
            pdst = &dst;
        }
    }
    hr = IDirect3DDevice9_Present(g_dev, NULL, pdst, NULL, NULL);
    if (hr == D3DERR_DEVICELOST) g_lost = 1;
}

void d9_clear(DWORD flags, DWORD color, float z, DWORD stencil)
{
    DWORD f = 0;
    if (check_lost()) return;
    flush();
    if (flags & 1) f |= D3DCLEAR_TARGET;
    if (flags & 2) f |= D3DCLEAR_ZBUFFER;
    if (flags & 4) f |= D3DCLEAR_STENCIL;
    if (f) IDirect3DDevice9_Clear(g_dev, 0, NULL, f, color, z, stencil);
}

void d9_viewport(DWORD x, DWORD y, DWORD w, DWORD h)
{
    D3DVIEWPORT9 vp;
    if (!g_dev) return;
    flush();
    if (x + w > (DWORD)g_w) w = g_w - x;
    if (y + h > (DWORD)g_h) h = g_h - y;
    vp.X = x; vp.Y = y; vp.Width = w; vp.Height = h; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    IDirect3DDevice9_SetViewport(g_dev, &vp);
}

/* ---- states --------------------------------------------------------------------------------------- */
static void apply_rs(DWORD st, DWORD v)
{
    switch (st) {
    case 1: case 2: case 3: case 4: case 5: case 6: case 10: case 11: case 12: case 13: case 17: case 18:
    case 21: case 30: case 31: case 32: case 33: case 39: case 41: case 43: case 44: case 45: case 46:
    case 49: case 50: case 51:
        return;                                     /* DX6 legacy, no D3D9 equivalent (or done elsewhere) */
    case 47: {                                      /* ZBIAS 0..16 -> DEPTHBIAS (negative = closer) */
        float b = -(float)v * 0.00002f;
        IDirect3DDevice9_SetRenderState(g_dev, D3DRS_DEPTHBIAS, *(DWORD *)&b);
        return;
    }
    default:
        IDirect3DDevice9_SetRenderState(g_dev, (D3DRENDERSTATETYPE)st, v);
    }
}

void d9_render_state(DWORD st, DWORD v)
{
    if (st >= NRS) return;
    if (g_rs_set[st] && g_rs[st] == v) return;
    flush();
    g_rs[st] = v; g_rs_set[st] = 1;
    if (g_dev) apply_rs(st, v);
}

static void set_samp(DWORD stage, DWORD type, DWORD v)
{
    if (g_samp_set[stage][type] && g_samp[stage][type] == v) return;
    flush();
    g_samp[stage][type] = v; g_samp_set[stage][type] = 1;
    if (g_dev) IDirect3DDevice9_SetSamplerState(g_dev, stage, (D3DSAMPLERSTATETYPE)type, v);
}

void d9_stage_state(DWORD stage, DWORD t, DWORD v)
{
    if (stage >= 8) return;
    switch (t) {
    case 12: set_samp(stage, D3DSAMP_ADDRESSU, v); set_samp(stage, D3DSAMP_ADDRESSV, v); return;
    case 13: set_samp(stage, D3DSAMP_ADDRESSU, v); return;
    case 14: set_samp(stage, D3DSAMP_ADDRESSV, v); return;
    case 15: set_samp(stage, D3DSAMP_BORDERCOLOR, v); return;
    case 16: set_samp(stage, D3DSAMP_MAGFILTER, v == 5 ? D3DTEXF_ANISOTROPIC : v >= 2 ? D3DTEXF_LINEAR : D3DTEXF_POINT); return;
    case 17: set_samp(stage, D3DSAMP_MINFILTER, v == 3 ? D3DTEXF_ANISOTROPIC : v >= 2 ? D3DTEXF_LINEAR : D3DTEXF_POINT); return;
    case 18: set_samp(stage, D3DSAMP_MIPFILTER, v <= 1 ? D3DTEXF_NONE : v == 2 ? D3DTEXF_POINT : D3DTEXF_LINEAR); return;
    case 19: set_samp(stage, D3DSAMP_MIPMAPLODBIAS, v); return;
    case 20: set_samp(stage, D3DSAMP_MAXMIPLEVEL, v); return;
    case 21: set_samp(stage, D3DSAMP_MAXANISOTROPY, v); return;
    }
    if (t >= 33) return;
    if (g_tss_set[stage][t] && g_tss[stage][t] == v) return;
    flush();
    g_tss[stage][t] = v; g_tss_set[stage][t] = 1;
    if (g_dev) IDirect3DDevice9_SetTextureStageState(g_dev, stage, (D3DTEXTURESTAGESTATETYPE)t, v);
}

void d9_set_texture(DWORD stage, void *tex)
{
    if (stage >= 8 || g_tex[stage] == tex) return;
    flush();
    g_tex[stage] = tex;
    if (g_dev) IDirect3DDevice9_SetTexture(g_dev, stage, (IDirect3DBaseTexture9 *)tex);
}

void d9_draw(DWORD prim, DWORD fvf, const void *verts, DWORD n)
{
    DWORD stride = fvf_stride(fvf), i;
    const BYTE *v = (const BYTE *)verts;
    if (!g_dev || g_lost || !verts || n < 3) return;
    if (!g_in_scene) d9_begin_scene();
    if (prim != D3DPT_TRIANGLEFAN) {              /* not seen in Drakan (F-036): draw directly */
        DWORD tris = prim == D3DPT_TRIANGLELIST ? n / 3 : prim == D3DPT_TRIANGLESTRIP ? n - 2 : 0;
        flush();
        if (!tris) return;
        IDirect3DDevice9_SetFVF(g_dev, fvf);
        IDirect3DDevice9_DrawPrimitiveUP(g_dev, (D3DPRIMITIVETYPE)prim, tris, verts, stride);
        return;
    }
    if (g_batch_verts && (fvf != g_batch_fvf || (g_batch_verts + (n - 2) * 3) * stride > BATCH_BYTES)) flush();
    if ((n - 2) * 3 * stride > BATCH_BYTES) return;
    g_batch_fvf = fvf; g_batch_stride = stride;
    for (i = 1; i + 1 < n; i++) {                  /* fan (0, i, i+1) -> list */
        BYTE *dst = g_batch + g_batch_verts * stride;
        memcpy(dst, v, stride);
        memcpy(dst + stride, v + i * stride, stride);
        memcpy(dst + 2 * stride, v + (i + 1) * stride, stride);
        g_batch_verts += 3;
    }
}

/* ---- textures --------------------------------------------------------------------------------------- */
void *d9_tex_create(int w, int h, int fmt)
{
    IDirect3DTexture9 *t = NULL;
    D3DFORMAT f = fmt == 1 ? D3DFMT_V8U8 : fmt == 2 ? D3DFMT_L6V5U5 : fmt == 3 ? D3DFMT_X8L8V8U8 : D3DFMT_A8R8G8B8;
    HRESULT hr;
    if (!g_dev) return NULL;
    hr = IDirect3DDevice9_CreateTexture(g_dev, w, h, 1, 0, f, D3DPOOL_MANAGED, &t, NULL);
    if (FAILED(hr)) {
        static int logged;
        if (logged++ < 8) hook_log("[d9] CreateTexture %dx%d fmt %d -> hr %08X\n", w, h, fmt, (DWORD)hr);
        return NULL;
    }
    return t;
}

void d9_tex_update(void *tex, const void *data, int pitch, int w, int h, int bpp)
{
    D3DLOCKED_RECT lr;
    int y;
    if (!tex || !data) return;
    if (FAILED(IDirect3DTexture9_LockRect((IDirect3DTexture9 *)tex, 0, &lr, NULL, 0))) return;
    for (y = 0; y < h; y++)
        memcpy((BYTE *)lr.pBits + y * lr.Pitch, (const BYTE *)data + y * pitch, w * bpp);
    IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)tex, 0);
}

void d9_tex_release(void *tex)
{
    int s;
    if (!tex) return;
    for (s = 0; s < 8; s++) if (g_tex[s] == tex) { flush(); g_tex[s] = NULL; if (g_dev) IDirect3DDevice9_SetTexture(g_dev, s, NULL); }
    IDirect3DTexture9_Release((IDirect3DTexture9 *)tex);
}

/* ---- back buffer <-> memory --------------------------------------------------------------------------- */
int d9_read_back(DWORD *out, int w, int h)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DLOCKED_RECT lr;
    int y, ok = 0, cw = w < g_w ? w : g_w, ch = h < g_h ? h : g_h;
    if (!g_dev || g_lost) return 0;
    flush();
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) &&
        SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, g_w, g_h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_dev, bb, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        for (y = 0; y < ch; y++) memcpy(out + y * w, (BYTE *)lr.pBits + y * lr.Pitch, cw * 4);
        IDirect3DSurface9_UnlockRect(sys);
        ok = 1;
    }
    if (sys) IDirect3DSurface9_Release(sys);
    if (bb) IDirect3DSurface9_Release(bb);
    return ok;
}

int d9_write_back(const DWORD *in, int w, int h)
{
    IDirect3DSurface9 *bb = NULL, *off = NULL;
    D3DLOCKED_RECT lr;
    int y, ok = 0, was = g_in_scene;
    if (!g_dev || g_lost) return 0;
    flush();
    if (was) { IDirect3DDevice9_EndScene(g_dev); g_in_scene = 0; }
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) &&
        SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, w, h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &off, NULL)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(off, &lr, NULL, 0))) {
        for (y = 0; y < h; y++) memcpy((BYTE *)lr.pBits + y * lr.Pitch, in + y * w, w * 4);
        IDirect3DSurface9_UnlockRect(off);
        ok = SUCCEEDED(IDirect3DDevice9_StretchRect(g_dev, off, NULL, bb, NULL, D3DTEXF_LINEAR));
    }
    if (off) IDirect3DSurface9_Release(off);
    if (bb) IDirect3DSurface9_Release(bb);
    if (was) d9_begin_scene();
    return ok;
}
