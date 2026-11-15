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

/* be_remix.c (M2): Remix API from the bridge d3d9.dll */
void        rx_init(void *d3d9_module);           /* after the D3D9 device exists; no-op without the bridge */
int         rx_ready(void);
const void *rx_api(void);                         /* remixapi_Interface *, or NULL */
void        rx_note_frame_event(const char *what);

/* be_d9.c: M3 world drawing (our own static buffers, drawn before any game draw; see remix3d.c) */
extern volatile LONG g_d9_block;                   /* > 0: drop the game's draws (its 3D is replaced) */
void *d9_vb_create(const void *data, DWORD bytes);
void *d9_ib_create(const DWORD *idx, DWORD count);
void  d9_buf_release(void *buf);
int   d9_world_begin(const float view[16], const float proj[16]);   /* 0 = no device / lost */
void  d9_world_draw(void *vb, DWORD stride, DWORD nverts, void *ib, DWORD first, DWORD tris, void *tex);
void  d9_world_end(void);

/* be_ddraw.c: a D3D9 texture (A8R8G8B8) made from one of the game's DirectDraw surfaces (our fake one) */
void *be_texture_from_surface(void *dds4);

/* remix3d.c (M3) */
int   r3d_install(void);                           /* hooks; called from drakan_install_hooks */
void  r3d_level_loaded(void *world);               /* build our terrain buffers + textures */
void  r3d_on_begin_scene(void);                    /* called by be_d9 right after BeginScene, once per frame */
LONG  r3d_enter(int what);                         /* what: 1 objects; returns 1 if blocking was raised */
void  r3d_leave(LONG raised);

void  d9_world_alpha(int on);                      /* M3: alpha-blended pass (z-write stays on, so Remix doesn't see UI) */
void  rx_frame_tick(HWND hwnd);                    /* be_remix.c: per Present; re-activates the game after the Remix menu closes */
