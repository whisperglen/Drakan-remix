/*
 * be.h - M1 back end: our own DirectDraw 6 / Direct3D 3 implementation on Direct3D 9 (REMIX.md section 5).
 *
 * Two translation units, because ddraw.h + d3d.h (DX6) and d3d9.h define the same names differently:
 *   be_ddraw.c  the fake DX6 COM objects the game talks to (IDirectDraw/4, surfaces, palette, clipper,
 *               IDirect3D3, IDirect3DDevice3, viewport, texture, material, gamma control)
 *   be_d9.c     everything that touches D3D9: device, swap chain, state mapping, fan batching, textures
 * This header is the plain-C interface between the two (no DirectX types).
 */
#pragma once
#include <windows.h>

/* be_ddraw.c */
int     be_enabled(void);                                  /* [backend] d3d9=1 */
HRESULT be_DirectDrawCreate(GUID *guid, void **out, void *outer);
HRESULT be_DirectDrawEnumerateA(void *cb, void *ctx);

/* be_d9.c */
int   d9_device(HWND hwnd, int w, int h);                 /* create the device, or Reset it to w x h; 1 = ok */
int   d9_ready(void);
void  d9_window_mode(HWND hwnd, int fullscreen);           /* borderless "fullscreen" / back to the saved window */
void  d9_begin_scene(void);
void  d9_end_scene(void);
void  d9_present(void);
void  d9_clear(DWORD flags, DWORD color, float z, DWORD stencil);   /* flags: 1 target, 2 z, 4 stencil */
void  d9_viewport(DWORD x, DWORD y, DWORD w, DWORD h);
void  d9_render_state(DWORD dx6_state, DWORD value);
void  d9_stage_state(DWORD stage, DWORD dx6_type, DWORD value);
void  d9_set_texture(DWORD stage, void *tex);
void  d9_draw(DWORD prim, DWORD fvf, const void *verts, DWORD count);

/* textures: fmt 0 = A8R8G8B8 (data = 32-bit ARGB), 1 = V8U8, 2 = L6V5U5, 3 = X8L8V8U8 (data = raw rows) */
void *d9_tex_create(int w, int h, int fmt);
void  d9_tex_update(void *tex, const void *data, int pitch, int w, int h, int bytes_per_pixel);
void  d9_tex_release(void *tex);

/* back buffer <-> memory (X8R8G8B8 rows, w*4 pitch) for screenshots and the saved menu background */
int   d9_read_back(DWORD *out, int w, int h);
int   d9_write_back(const DWORD *in, int w, int h);
