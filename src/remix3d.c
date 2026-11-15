/*
 * remix3d.c - M3: our own 3D world for Remix, drawn through D3D9 before any game draw (REMIX.md 5, F-040).
 *
 * Remix (through the bridge) has no SetupCamera, so it takes the camera from D3D9 transforms, and it treats
 * everything after the first UI-looking draw as raster. So, once per frame, right after BeginScene:
 *   - set VIEW from the game camera (0x4879B0, the game's own right-handed view space, game units) and
 *     PROJECTION from the display's projection values (F-019), WORLD = identity;
 *   - draw the terrain from our own static vertex/index buffers (one pair per sector, built at level load
 *     from the level in memory, same rules as scripts/dklv_tool.py) with our own copies of the textures.
 * The game's own 3D is dropped while it is drawn (g_d9_block): world render 0x4410E0, objects 0x446E70
 * (sky included; via freecam.c's hook when present), overlays 0x44A360 / 0x4480C0 and the queued-fan flush
 * 0x43C990. Its 2D (HUD, menus, text) still goes through, rasterised by Remix on top.
 * Nothing in the game's data is changed.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <intrin.h>
#include "hook.h"
#include "be.h"

#define G_CAMERA    0x004879B0
#define G_DISPLAYP  0x00487A2C
#define D(p, o) (*(DWORD *)((BYTE *)(p) + (o)))
#define F(p, o) (*(float *)((BYTE *)(p) + (o)))
#define P(p, o) (*(BYTE **)((BYTE *)(p) + (o)))
#define W(p, o) (*(WORD *)((BYTE *)(p) + (o)))

typedef struct { float x, y, z; DWORD c; float u, v; } Vtx;     /* D3DFVF_XYZ | DIFFUSE | TEX1, 24 bytes */
typedef struct { DWORD tex, first, tris; } Range;
typedef struct {
    void *vb, *ib;
    DWORD nverts;
    Range *ranges; int nranges;
    float cx, cz, radius;                                        /* for the distance test */
} Chunk;

static Chunk *g_chunks;
static int    g_nchunks;
static void **g_tex;                                             /* our D3D9 textures, by texture-table index */
static BYTE  *g_texalpha;                                        /* 1 = the game blends this texture (flags +0x14 & 0xF0000) */
static DWORD  g_ntex;
static int    cfg_terrain, cfg_alpha, cfg_block_world, cfg_block_objects, cfg_block_fx, cfg_vcolor;
static float  cfg_radius, cfg_far;
static int    g_logged_frame;

/* ---- blocking -------------------------------------------------------------------------------------------- */
LONG r3d_enter(int what)
{
    int on = what == 1 ? cfg_block_objects : what == 2 ? cfg_block_world : cfg_block_fx;
    if (!on || !g_nchunks) return 0;            /* only once our world replaces the game's */
    InterlockedIncrement(&g_d9_block);
    return 1;
}
void r3d_leave(LONG raised) { if (raised) InterlockedDecrement(&g_d9_block); }

typedef void (__fastcall *thisfn)(void *self, void *edx);
typedef void (__fastcall *thisfn1)(void *self, void *edx, int a);
static thisfn  O_world, O_fx1, O_flush;
static thisfn1 O_fx2;
static void __fastcall H_world(void *s, void *e) { LONG r = r3d_enter(2); O_world(s, e); r3d_leave(r); }
typedef int (__fastcall *objfn)(void *obj, void *edx, float *camera);
static objfn O_obj;
static int __fastcall H_obj(void *o, void *e, float *cam) { LONG r = r3d_enter(1); int v = O_obj(o, e, cam); r3d_leave(r); return v; }
static void __fastcall H_fx1(void *s, void *e)   { LONG r = r3d_enter(3); O_fx1(s, e); r3d_leave(r); }
static void __fastcall H_fx2(void *s, void *e, int a) { LONG r = r3d_enter(3); O_fx2(s, e, a); r3d_leave(r); }
/* the flush also runs elsewhere (end of frame, 2D); only the one in the frame function, right after the
 * 3D overlay passes (call at 0x43C056, returns to 0x43C05B), carries 3D */
static void __fastcall H_flush(void *s, void *e)
{
    LONG r = (DWORD)_ReturnAddress() == 0x0043C05B ? r3d_enter(3) : 0;
    O_flush(s, e);
    r3d_leave(r);
}

/* ---- level -> buffers ---------------------------------------------------------------------------------- */
static void free_level(void)
{
    int i;
    DWORD t;
    for (i = 0; i < g_nchunks; i++) {
        d9_buf_release(g_chunks[i].vb); d9_buf_release(g_chunks[i].ib); free(g_chunks[i].ranges);
    }
    free(g_chunks); g_chunks = NULL; g_nchunks = 0;
    for (t = 0; t < g_ntex; t++) if (g_tex[t]) d9_tex_release(g_tex[t]);
    free(g_tex); g_tex = NULL; g_ntex = 0;
    free(g_texalpha); g_texalpha = NULL;
}

static int vertex_of(BYTE *world, BYTE *s, int c, int r, Vtx *out)
{
    DWORD w = D(s, 0x18);
    BYTE *v = P(s, 0x24) + ((w + 1) * r + c) * 10;
    WORD fl = W(v, 0);
    BYTE *m = world + 0x38 + (fl & 0xFF) * 0x28;
    if (fl & 0x100) return 0;                                       /* hole */
    out->x = c * 2048.0f + F(s, 4);
    out->y = F(v, 6) + F(s, 8) + ((D(m, 0) & 1) ? F(m, 4) : 0.0f);
    out->z = r * 2048.0f + F(s, 0xC);
    out->c = cfg_vcolor ? (D(v, 2) | 0xFF000000) : 0xFFFFFFFF;
    return 1;
}

static int cmp_tri(const void *a, const void *b) { return (int)((const DWORD *)a)[0] - (int)((const DWORD *)b)[0]; }

static void build_sector(BYTE *world, BYTE *s, Chunk *ch)
{
    DWORD w = D(s, 0x18), h = D(s, 0x1C), type = D(s, 0x28), c, r, ntri = 0, k, i;
    BYTE *cells = P(s, 0x20);
    Vtx *verts;
    DWORD *tris;          /* per triangle: tex, v0, v1, v2 */
    DWORD *idx;
    float minx = 1e30f, maxx = -1e30f, minz = 1e30f, maxz = -1e30f;
    if (!w || !h || !cells || !P(s, 0x24) || w > 4096 || h > 4096) return;
    verts = (Vtx *)malloc(w * h * 6 * sizeof(Vtx));
    tris = (DWORD *)malloc(w * h * 2 * 4 * sizeof(DWORD));
    if (!verts || !tris) { free(verts); free(tris); return; }
    for (r = 0; r < h; r++) for (c = 0; c < w; c++) {
        const WORD *raw = (const WORD *)(cells + (w * r + c) * 0x16);
        WORD flags = raw[0];
        int grid[4][2] = { { c, r }, { c + 1, r }, { c + 1, r + 1 }, { c, r + 1 } }, slots[4][2], half;
        int base = type == 1 ? (int)((flags & 1) + 4) : (int)(3 - (flags & 1));
        for (k = 0; k < 4; k++) {
            int src = type == 1 ? (k == 0 ? 1 : k == 1 ? 0 : k == 2 ? 3 : 2) : (int)k;
            slots[k][0] = grid[src][0]; slots[k][1] = grid[src][1];
        }
        for (half = 0; half < 2; half++) {
            WORD tex = raw[1 + half];
            Vtx tv[3];
            int ok = 1;
            if (tex >= 0xFFFE) continue;
            for (k = 0; k < 3; k++) {
                int sl = half == 0 ? (base + (int)k) & 3 : (base - 2 + (int)k) & 3;
                ok &= vertex_of(world, s, slots[sl][0], slots[sl][1], &tv[k]);
                tv[k].u = raw[3 + sl] / 65535.0f;
                tv[k].v = raw[7 + sl] / 65535.0f;
            }
            if (!ok) continue;
            for (k = 0; k < 3; k++) {
                verts[ntri * 3 + k] = tv[k];
                if (tv[k].x < minx) minx = tv[k].x; if (tv[k].x > maxx) maxx = tv[k].x;
                if (tv[k].z < minz) minz = tv[k].z; if (tv[k].z > maxz) maxz = tv[k].z;
            }
            tris[ntri * 4] = tex; tris[ntri * 4 + 1] = ntri * 3; tris[ntri * 4 + 2] = ntri * 3 + 1; tris[ntri * 4 + 3] = ntri * 3 + 2;
            ntri++;
        }
    }
    if (ntri) {
        qsort(tris, ntri, 4 * sizeof(DWORD), cmp_tri);           /* group by texture */
        idx = (DWORD *)malloc(ntri * 3 * sizeof(DWORD));
        ch->ranges = (Range *)calloc(ntri, sizeof(Range));
        if (idx && ch->ranges) {
            for (i = 0; i < ntri; i++) {
                idx[i * 3] = tris[i * 4 + 1]; idx[i * 3 + 1] = tris[i * 4 + 2]; idx[i * 3 + 2] = tris[i * 4 + 3];
                if (!ch->nranges || ch->ranges[ch->nranges - 1].tex != tris[i * 4]) {
                    ch->ranges[ch->nranges].tex = tris[i * 4]; ch->ranges[ch->nranges].first = i * 3; ch->nranges++;
                }
                ch->ranges[ch->nranges - 1].tris++;
            }
            ch->vb = d9_vb_create(verts, ntri * 3 * sizeof(Vtx));
            ch->ib = d9_ib_create(idx, ntri * 3);
            ch->nverts = ntri * 3;
            ch->cx = (minx + maxx) * 0.5f; ch->cz = (minz + maxz) * 0.5f;
            ch->radius = 0.5f * sqrtf((maxx - minx) * (maxx - minx) + (maxz - minz) * (maxz - minz));
        }
        free(idx);
    }
    free(verts); free(tris);
}

void r3d_level_loaded(void *world_p)
{
    BYTE *world = (BYTE *)world_p, *sectors, **arr;
    DWORD count, i, ntexobj, tris = 0, draws = 0, texok = 0;
    if (!cfg_terrain || !be_enabled() || !d9_ready()) return;
    free_level();
    __try {
        count = D(world, 0x14); sectors = P(world, 0xC);
        arr = (BYTE **)P(world, 0x2838); ntexobj = D(world, 0x2838 + 8);
        if (!sectors || count > 100000 || ntexobj > 4096) { hook_log("[r3d] world looks invalid\n"); return; }
        g_tex = (void **)calloc(ntexobj ? ntexobj : 1, sizeof(void *));
        g_texalpha = (BYTE *)calloc(ntexobj ? ntexobj : 1, 1);
        for (i = 0; i < ntexobj; i++) if (arr && arr[i] && (D(arr[i], 0x14) & 0xF0000)) g_texalpha[i] = 1;
        g_ntex = ntexobj;
        for (i = 0; i < ntexobj; i++)
            if (arr && arr[i] && (g_tex[i] = be_texture_from_surface(P(arr[i], 0x44)))) texok++;
        g_chunks = (Chunk *)calloc(count ? count : 1, sizeof(Chunk));
        for (i = 0; i < count; i++) build_sector(world, sectors + i * 0xA8, &g_chunks[i]);
        g_nchunks = count;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hook_log("[r3d] exception while building the terrain\n");
        free_level();
        return;
    }
    for (i = 0; i < (DWORD)g_nchunks; i++) { int k; for (k = 0; k < g_chunks[i].nranges; k++) { tris += g_chunks[i].ranges[k].tris; draws++; } }
    hook_log("[r3d] terrain: %d sectors, %u triangles, %u draws/frame (all in range), %u of %u textures copied\n",
             g_nchunks, tris, draws, texok, ntexobj);
    hook_log_flush();
    g_logged_frame = 0;
}

/* ---- per frame ----------------------------------------------------------------------------------------- */
void r3d_on_begin_scene(void)
{
    float view[16], proj[16], *cam = (float *)G_CAMERA, campos[3], zn = 32.0f, zf = cfg_far;
    BYTE *disp;
    int i, drawn = 0;
    if (!g_nchunks) return;
    __try {
        disp = *(BYTE **)G_DISPLAYP;
        if (!disp) return;
        /* view: p_view = V * p_world + t, V row-major at cam+0x0C, t at cam+0; D3D uses row vectors (p * M) */
        memset(view, 0, sizeof(view));
        for (i = 0; i < 3; i++) {
            view[0 * 4 + i] = cam[3 + i * 3 + 0];
            view[1 * 4 + i] = cam[3 + i * 3 + 1];
            view[2 * 4 + i] = cam[3 + i * 3 + 2];
            view[3 * 4 + i] = cam[i];
        }
        view[15] = 1.0f;
        /* camera position in the world: -V^T t */
        for (i = 0; i < 3; i++) campos[i] = -(cam[3 + 0 * 3 + i] * cam[0] + cam[3 + 1 * 3 + i] * cam[1] + cam[3 + 2 * 3 + i] * cam[2]);
        /* projection, right-handed (the game looks down -z): x_ndc = (kx*Sx/cx) * x / -z, y_ndc = (ky*Sy/cy) * y / -z */
        memset(proj, 0, sizeof(proj));
        proj[0]  = F(disp, 0x69C) * F(disp, 0x6B0) / F(disp, 0x68C);
        proj[5]  = F(disp, 0x6A0) * F(disp, 0x6B4) / F(disp, 0x690);
        proj[10] = zf / (zn - zf);
        proj[11] = -1.0f;
        proj[14] = zn * zf / (zn - zf);
        if (!g_logged_frame++) {
            hook_log("[r3d] camera at %.0f %.0f %.0f, projection x %.4f y %.4f, near %.0f far %.0f\n",
                     campos[0], campos[1], campos[2], proj[0], proj[5], zn, zf);
            hook_log_flush();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!d9_world_begin(view, proj)) return;
    {   /* pass 0: opaque; pass 1: textures the game alpha-blends (water...), z-write stays on */
        int pass;
        for (pass = 0; pass < (cfg_alpha ? 2 : 1); pass++) {
            if (pass == 1) d9_world_alpha(1);
            for (i = 0; i < g_nchunks; i++) {
                Chunk *ch = &g_chunks[i];
                int k;
                if (!ch->vb) continue;
                if (cfg_radius > 0) {
                    float dx = ch->cx - campos[0], dz = ch->cz - campos[2], lim = cfg_radius + ch->radius;
                    if (dx * dx + dz * dz > lim * lim) continue;
                }
                for (k = 0; k < ch->nranges; k++) {
                    Range *rg = &ch->ranges[k];
                    int a = cfg_alpha && rg->tex < g_ntex && g_texalpha[rg->tex];
                    if (a != pass) continue;
                    d9_world_draw(ch->vb, sizeof(Vtx), ch->nverts, ch->ib, rg->first, rg->tris, rg->tex < g_ntex ? g_tex[rg->tex] : NULL);
                    drawn++;
                }
            }
        }
        if (cfg_alpha) d9_world_alpha(0);
    }
    d9_world_end();
    if (g_logged_frame == 1) { hook_log("[r3d] first frame: %d terrain draws\n", drawn); hook_log_flush(); }
}

/* ---- install ------------------------------------------------------------------------------------------- */
int r3d_install(void)
{
    static const BYTE b_world[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x58, 0x01, 0x00, 0x00 };   /* push ebp; mov ebp,esp; sub esp,158 */
    int n = 0;
    cfg_terrain       = hook_ini_int("remix", "terrain", 1);
    cfg_alpha         = hook_ini_int("remix", "terrain_alpha", 1);
    cfg_block_world   = hook_ini_int("remix", "block_game_terrain", 1);
    cfg_block_objects = hook_ini_int("remix", "block_game_objects", 1);
    cfg_block_fx      = hook_ini_int("remix", "block_game_effects", 1);
    cfg_vcolor        = hook_ini_int("remix", "terrain_vertex_color", 0);
    cfg_radius        = (float)hook_ini_int("remix", "terrain_radius", 0);
    cfg_far           = (float)hook_ini_int("remix", "far_plane", 2000000);
    if (!be_enabled() || !cfg_terrain) return 0;
    if ((O_world = (thisfn)detour_install(0x004410E0, b_world, sizeof(b_world), H_world))) n++;
    if (!freecam_drawobj_hooked()) {   /* object draws (sky included); freecam.c's hook does this when it is on */
        static const BYTE b_draw[] = { 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00 };   /* sub esp,0C8 */
        if ((O_obj = (objfn)detour_install(0x00446E70, b_draw, sizeof(b_draw), H_obj))) n++;
    }
    {   /* 3D overlays 0x44A360 (thiscall, no args) and 0x4480C0 (thiscall, 1 arg), queued-fan flush 0x43C990 */
        static const BYTE b_fx1[] = { 0x51, 0xA1, 0x28, 0x7A, 0x48, 0x00 };                     /* push ecx; mov eax,[487A28] */
        static const BYTE b_fx2[] = { 0x81, 0xEC, 0x8C, 0x02, 0x00, 0x00 };                     /* sub esp,28C */
        static const BYTE b_flush[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0xBC, 0x01, 0x00, 0x00 };  /* push esi; mov esi,ecx; mov eax,[esi+1BC] */
        if ((O_fx1 = (thisfn)detour_install(0x0044A360, b_fx1, sizeof(b_fx1), H_fx1))) n++;
        if ((O_fx2 = (thisfn1)detour_install(0x004480C0, b_fx2, sizeof(b_fx2), H_fx2))) n++;
        if ((O_flush = (thisfn)detour_install(0x0043C990, b_flush, sizeof(b_flush), H_flush))) n++;
    }
    hook_log("[r3d] M3 world: %d hooks, block terrain %d objects %d effects %d, radius %.0f, far %.0f, vertex colour %d\n",
             n, cfg_block_world, cfg_block_objects, cfg_block_fx, cfg_radius, cfg_far, cfg_vcolor);
    return n;
}
