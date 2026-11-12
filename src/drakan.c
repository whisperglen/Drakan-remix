/*
 * drakan.c - Drakan.exe (2018 build, PE checksum 0x89E91) render hooks + frame capture log.
 *
 * Addresses / layouts come from FINDINGS.md (F-013 .. F-018). Nothing here writes game
 * state: the hooks call the original function and only READ memory for the log.
 *
 * Capture: press F10 in game (or auto: [capture] first_frame / every_frames in the ini).
 * During one captured frame we log the camera, projection parameters, world summary,
 * the first N objects through 0x447690 and the first N polygons through the draw calls,
 * plus self-checks that recompute the game's results from our understanding of the math.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "hook.h"

/* ---- addresses (Drakan.exe, image base 0x400000) ---------------------------------- */
#define A_FRAME        0x0043BEB0   /* void frame(void)                                  */
#define A_XFORM        0x00447690   /* int __thiscall xform(obj, int zero)  ret 4         */
#define A_FAN1         0x0043C3C0   /* int __thiscall fan(r, Vertex**, n, tex) 1 UV  ret 0xC */
#define A_FAN4         0x0043C630   /*                         4 UV (water/multitex)       */
#define A_FANQ         0x0043C7D0   /*                         queued/sorted               */
#define G_CAMERA       0x004879B0   /* camera: t[3], V[3][3] row-major at +0x0C, pos +0x6C */
#define G_DISPLAY_PTR  0x00487A2C   /* display object pointer                              */
#define G_RENDERER_PTR 0x00487A28   /* D3D renderer object pointer                         */
#define G_VBUF_PTR     0x004885A8   /* transformed-vertex buffer (stride 0x38)             */
#define G_WORLD        0x00484348   /* world / level object                                */
#define G_VIEWCELLS    0x00487C58   /* view range in terrain cells                         */
#define EXE_CHECKSUM   0x00089E91

#define F(p, off)  (*(float *)((BYTE *)(p) + (off)))
#define D(p, off)  (*(DWORD *)((BYTE *)(p) + (off)))
#define P(p, off)  (*(BYTE **)((BYTE *)(p) + (off)))

typedef void (__cdecl    *frame_fn)(void);
typedef int  (__fastcall *xform_fn)(void *obj, void *edx, int zero);
typedef int  (__fastcall *fan_fn)(void *r, void *edx, BYTE **v, int n, void *tex);

static frame_fn O_frame;
static xform_fn O_xform;
static fan_fn   O_fan1, O_fan4, O_fanq;

/* ---- capture state ---------------------------------------------------------------- */
static int   g_frame, g_capturing, g_capture_no;
static int   cfg_first, cfg_every, cfg_max_obj, cfg_max_poly, cfg_vpo;
static int   n_xform, n_xform_verts, n_fan[3], n_fan_verts[3], logged_obj, logged_poly;
static float e_xform_view, e_world_cam, e_screen, e_rhw, e_poly_screen;
static int   n_checked_v, n_poly_screen_mismatch, n_poly_checked;

static float fmaxf_(float a, float b) { return a > b ? a : b; }

static void log_vec(const char *label, const float *v, int n)
{
    int i;
    hook_log("%s", label);
    for (i = 0; i < n; i++) hook_log(" %12.4f", v[i]);
    hook_log("\n");
}

/* view = V * w + t   (camera, F-015) */
static void cam_world_to_view(const float *w, float *out)
{
    const float *c = (const float *)G_CAMERA;
    int i;
    for (i = 0; i < 3; i++)
        out[i] = c[3 + 3 * i] * w[0] + c[4 + 3 * i] * w[1] + c[5 + 3 * i] * w[2] + c[i];
}
/* world = V^T * (view - t) */
static void cam_view_to_world(const float *v, float *out)
{
    const float *c = (const float *)G_CAMERA;
    float d[3];
    int i;
    for (i = 0; i < 3; i++) d[i] = v[i] - c[i];
    for (i = 0; i < 3; i++) out[i] = c[3 + i] * d[0] + c[6 + i] * d[1] + c[9 + i] * d[2];
}

static void capture_begin(void)
{
    BYTE *disp = *(BYTE **)G_DISPLAY_PTR;
    BYTE *world = (BYTE *)G_WORLD;
    int i;
    g_capturing = 1;
    g_capture_no++;
    n_xform = n_xform_verts = logged_obj = logged_poly = 0;
    n_fan[0] = n_fan[1] = n_fan[2] = n_fan_verts[0] = n_fan_verts[1] = n_fan_verts[2] = 0;
    e_xform_view = e_world_cam = e_screen = e_rhw = e_poly_screen = 0;
    n_checked_v = n_poly_screen_mismatch = n_poly_checked = 0;

    hook_log("\n==================== capture %d, frame %d ====================\n",
             g_capture_no, g_frame);
    __try {
        const float *c = (const float *)G_CAMERA;
        log_vec("camera t      :", c, 3);
        log_vec("camera V row0 :", c + 3, 3);
        log_vec("camera V row1 :", c + 6, 3);
        log_vec("camera V row2 :", c + 9, 3);
        log_vec("camera pos    :", c + 27, 3);          /* +0x6C */
        log_vec("camera +0x30..+0x68:", c + 12, 15);    /* unknown fields, for later */
        hook_log("display %p renderer %p vbuf %p\n", disp, *(void **)G_RENDERER_PTR,
                 *(void **)G_VBUF_PTR);
        if (disp) {
            hook_log("display +0x674..+0x680 (viewport x,y,w,h): %d %d %d %d\n",
                     D(disp, 0x674), D(disp, 0x678), D(disp, 0x67C), D(disp, 0x680));
            log_vec("display +0x688..+0x6BC:", (float *)(disp + 0x688), 14);
            hook_log("  = lodf %.4f  cx %.2f cy %.2f  [694] %.4f [698] %.4f  kx %.4f ky %.4f"
                     "  frustum %.4f %.4f %.4f  Sx %.4f Sy %.4f  [6B8] %.4f [6BC] %.4f\n",
                     F(disp, 0x688), F(disp, 0x68C), F(disp, 0x690), F(disp, 0x694), F(disp, 0x698),
                     F(disp, 0x69C), F(disp, 0x6A0), F(disp, 0x6A4), F(disp, 0x6A8), F(disp, 0x6AC),
                     F(disp, 0x6B0), F(disp, 0x6B4), F(disp, 0x6B8), F(disp, 0x6BC));
        }
        hook_log("world 0x%08X first 16 dwords:", G_WORLD);
        for (i = 0; i < 16; i++) hook_log(" %08X", D(world, 4 * i));
        hook_log("\nview range cells: %d\n", *(int *)G_VIEWCELLS);
        if (P(world, 0xC)) {
            BYTE *s = P(world, 0xC);
            hook_log("sector[0] @%p: type %d  origin %.1f %.1f %.1f  grid %d,%d  size %dx%d  cells %p  verts %p\n",
                     s, D(s, 40), F(s, 4), F(s, 8), F(s, 12), D(s, 16), D(s, 20), D(s, 24), D(s, 28),
                     P(s, 32), P(s, 36));
            hook_log("sector[0] raw:");
            for (i = 0; i < 0xA8 / 4; i++) hook_log("%s%08X", (i % 8) ? " " : "\n   ", D(s, 4 * i));
            hook_log("\n");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hook_log("[capture] exception while reading globals\n");
    }
}

static void capture_end(void)
{
    hook_log("---- frame %d summary ----\n", g_frame);
    hook_log("xform calls %d (verts %d); fan1 %d (%d v), fan4 %d (%d v), fanq %d (%d v)\n",
             n_xform, n_xform_verts, n_fan[0], n_fan_verts[0], n_fan[1], n_fan_verts[1],
             n_fan[2], n_fan_verts[2]);
    hook_log("self-check over %d object vertices:\n"
             "  max |game view' - Sx,Sy*(MV*p)|     = %g\n"
             "  max |V*(R*p+pos)+t - MV*p|          = %g   (world matrix + camera interpretation)\n"
             "  max |game sx,sy - recomputed|       = %g   (unclipped vertices)\n"
             "  max |game rhw - 1/(-z)|             = %g\n",
             n_checked_v, e_xform_view, e_world_cam, e_screen, e_rhw);
    hook_log("  polygon vertices checked %d: max screen err %g, %d mismatches > 0.5 px (clip-edge vertices expected)\n",
             n_poly_checked, e_poly_screen, n_poly_screen_mismatch);
    hook_log_flush();
    g_capturing = 0;
}

/* ---- hooks -------------------------------------------------------------------------- */
static void __cdecl H_frame(void)
{
    int trigger;
    g_frame++;
    trigger = (GetAsyncKeyState(VK_F10) & 1) ||
              (cfg_first && g_frame == cfg_first) ||
              (cfg_every && g_frame % cfg_every == 0);
    if (trigger) capture_begin();
    O_frame();
    if (g_capturing) capture_end();
}

static int __fastcall H_xform(BYTE *obj, void *edx, int zero)
{
    float mv[12], wm[12];
    BYTE *mesh, *anim, *src = NULL;
    int count = 0, n, i, logit;

    if (!g_capturing) return O_xform(obj, edx, zero);

    n_xform++;
    logit = logged_obj < cfg_max_obj;
    __try {
        memcpy(mv, obj + 0x44, sizeof(mv));     /* model-view: t[3] @0x44, M[3][3] @0x50 */
        memcpy(wm, obj + 0x08, sizeof(wm));     /* world: pos[3] @0x08, R[3][3] @0x14   */
        mesh = P(obj, 0x74);
        anim = P(obj, 0x78);
        if (anim) src = P(anim, 0xA4);
        else if (mesh) { src = P(mesh, 0x14); count = (int)D(mesh, 0x0C); }
        if (logit) {
            hook_log("\n[obj %d] %p flags4 %08X flags8c %08X mesh %p anim %p src %p count %d\n",
                     logged_obj, obj, D(obj, 4), D(obj, 0x8C), mesh, anim, src, count);
            log_vec("  world pos   :", wm, 3);
            log_vec("  world R0    :", wm + 3, 3);
            log_vec("  world R1    :", wm + 6, 3);
            log_vec("  world R2    :", wm + 9, 3);
            log_vec("  modelview t :", mv, 3);
            log_vec("  modelview M0:", mv + 3, 3);
            log_vec("  modelview M1:", mv + 6, 3);
            log_vec("  modelview M2:", mv + 9, 3);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hook_log("[obj] exception reading object %p\n", obj);
        return O_xform(obj, edx, zero);
    }

    n = O_xform(obj, edx, zero);
    n_xform_verts += n;

    __try {
        BYTE *disp = *(BYTE **)G_DISPLAY_PTR, *buf = *(BYTE **)G_VBUF_PTR;
        float Sx = F(disp, 0x6B0), Sy = F(disp, 0x6B4);
        if (src && buf) {
            for (i = 0; i < n; i++) {
                const float *p = (const float *)(src + 12 * i);
                BYTE *o = buf + 0x38 * i;
                float v[3], w[3], vc[3];
                int k;
                for (k = 0; k < 3; k++)
                    v[k] = mv[3 + 3 * k] * p[0] + mv[4 + 3 * k] * p[1] + mv[5 + 3 * k] * p[2] + mv[k];
                e_xform_view = fmaxf_(e_xform_view, (float)fabs(F(o, 0x28) - Sx * v[0]));
                e_xform_view = fmaxf_(e_xform_view, (float)fabs(F(o, 0x2C) - Sy * v[1]));
                e_xform_view = fmaxf_(e_xform_view, (float)fabs(F(o, 0x30) - v[2]));
                for (k = 0; k < 3; k++)
                    w[k] = wm[3 + 3 * k] * p[0] + wm[4 + 3 * k] * p[1] + wm[5 + 3 * k] * p[2] + wm[k];
                cam_world_to_view(w, vc);
                for (k = 0; k < 3; k++) e_world_cam = fmaxf_(e_world_cam, (float)fabs(vc[k] - v[k]));
                if (D(o, 0x34) == 0) {
                    float rhw = 1.0f / -F(o, 0x30);
                    float sx = F(disp, 0x68C) + F(disp, 0x69C) * F(o, 0x28) * rhw;
                    float sy = F(disp, 0x690) - F(disp, 0x6A0) * F(o, 0x2C) * rhw;
                    e_screen = fmaxf_(e_screen, fmaxf_((float)fabs(sx - F(o, 0)), (float)fabs(sy - F(o, 4))));
                    e_rhw = fmaxf_(e_rhw, (float)fabs(rhw - F(o, 0x0C)));
                }
                n_checked_v++;
                if (logit && i < cfg_vpo) {
                    hook_log("  v%-3d model %10.3f %10.3f %10.3f | world %10.3f %10.3f %10.3f |"
                             " view' %10.3f %10.3f %10.3f | scr %8.2f %8.2f z %.5f rhw %.6f clip %02X\n",
                             i, p[0], p[1], p[2], w[0], w[1], w[2], F(o, 0x28), F(o, 0x2C), F(o, 0x30),
                             F(o, 0), F(o, 4), F(o, 8), F(o, 0x0C), D(o, 0x34));
                }
            }
        }
        if (logit) hook_log("  -> %d vertices transformed\n", n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hook_log("[obj] exception checking object %p\n", obj);
    }
    if (logit) logged_obj++;
    return n;
}

static void log_poly(int which, BYTE **v, int n, void *tex)
{
    BYTE *disp = *(BYTE **)G_DISPLAY_PTR;
    float Sx = F(disp, 0x6B0), Sy = F(disp, 0x6B4);
    int i, logit = logged_poly < cfg_max_poly;
    static const char *names[3] = { "fan1", "fan4", "fanq" };
    n_fan[which]++;
    n_fan_verts[which] += n;
    if (logit) hook_log("[poly %d] %s n=%d tex %p\n", logged_poly, names[which], n, tex);
    for (i = 0; i < n; i++) {
        BYTE *o = v[i];
        float view[3], w[3], rhw, sx, sy, err;
        view[0] = F(o, 0x28) / Sx; view[1] = F(o, 0x2C) / Sy; view[2] = F(o, 0x30);
        cam_view_to_world(view, w);
        rhw = 1.0f / -view[2];
        sx = F(disp, 0x68C) + F(disp, 0x69C) * F(o, 0x28) * rhw;
        sy = F(disp, 0x690) - F(disp, 0x6A0) * F(o, 0x2C) * rhw;
        err = fmaxf_((float)fabs(sx - F(o, 0)), (float)fabs(sy - F(o, 4)));
        n_poly_checked++;
        e_poly_screen = fmaxf_(e_poly_screen, err);
        if (err > 0.5f) n_poly_screen_mismatch++;
        if (logit)
            hook_log("   %d view %10.3f %10.3f %10.3f | world %10.3f %10.3f %10.3f | scr %8.2f %8.2f rhw %.6f uv %.4f %.4f%s\n",
                     i, view[0], view[1], view[2], w[0], w[1], w[2], F(o, 0), F(o, 4), F(o, 0x0C),
                     F(o, 0x18), F(o, 0x1C), err > 0.5f ? "  (screen differs: clip vertex?)" : "");
    }
    if (logit) logged_poly++;
}

#define FAN_HOOK(name, which, orig) \
    static int __fastcall name(void *r, void *edx, BYTE **v, int n, void *tex) { \
        if (g_capturing) { \
            __try { log_poly(which, v, n, tex); } \
            __except (EXCEPTION_EXECUTE_HANDLER) { hook_log("[poly] exception\n"); } \
        } \
        return orig(r, edx, v, n, tex); \
    }
FAN_HOOK(H_fan1, 0, O_fan1)
FAN_HOOK(H_fan4, 1, O_fan4)
FAN_HOOK(H_fanq, 2, O_fanq)

/* ---- install ------------------------------------------------------------------------ */
int drakan_install_hooks(void)
{
    static const BYTE b_frame[] = { 0x8B, 0x0D, 0x2C, 0x7A, 0x48, 0x00 };  /* mov ecx,[487A2C] */
    static const BYTE b_xform[] = { 0x83, 0xEC, 0x28, 0x53, 0x55 };        /* sub esp,28; push ebx; push ebp */
    static const BYTE b_fan1[]  = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00 };
    static const BYTE b_fan4[]  = { 0x81, 0xEC, 0xC0, 0x01, 0x00, 0x00 };  /* sub esp,1C0 */
    static const BYTE b_fanq[]  = { 0x53, 0x55, 0x56, 0x8B, 0xF1 };        /* push ebx/ebp/esi; mov esi,ecx */
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)dos + dos->e_lfanew);
    int n = 0;

    if ((DWORD)dos != 0x400000 || nt->OptionalHeader.CheckSum != EXE_CHECKSUM) {
        hook_log("[drakan] host is not the expected Drakan.exe (base %p, checksum 0x%X): no hooks\n",
                 dos, nt->OptionalHeader.CheckSum);
        return 0;
    }
    cfg_first    = hook_ini_int("capture", "first_frame", 0);
    cfg_every    = hook_ini_int("capture", "every_frames", 0);
    cfg_max_obj  = hook_ini_int("capture", "max_objects", 6);
    cfg_max_poly = hook_ini_int("capture", "max_polys", 12);
    cfg_vpo      = hook_ini_int("capture", "verts_per_object", 4);
    hook_log("[drakan] capture: F10, first_frame=%d every_frames=%d max_objects=%d max_polys=%d verts_per_object=%d\n",
             cfg_first, cfg_every, cfg_max_obj, cfg_max_poly, cfg_vpo);

    if ((O_frame = (frame_fn)detour_install(A_FRAME, b_frame, sizeof(b_frame), H_frame))) n++;
    else return n;     /* without the frame hook nothing would ever be captured */
    if ((O_xform = (xform_fn)detour_install(A_XFORM, b_xform, sizeof(b_xform), H_xform))) n++;
    if ((O_fan1  = (fan_fn)detour_install(A_FAN1, b_fan1, sizeof(b_fan1), H_fan1))) n++;
    if ((O_fan4  = (fan_fn)detour_install(A_FAN4, b_fan4, sizeof(b_fan4), H_fan4))) n++;
    if ((O_fanq  = (fan_fn)detour_install(A_FANQ, b_fanq, sizeof(b_fanq), H_fanq))) n++;
    return n;
}
