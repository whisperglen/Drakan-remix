/*
 * meshdump.c - M4 step 1: verify the object mesh format at runtime by writing every model the game draws, once,
 * as an OBJ + MTL + DDS textures (open in Blender like the terrain). [dump] meshes=1.
 *
 * Format (from 0x446E70, see FINDINGS F-044):
 *   obj+0x74 mesh: +0x0C vertex count, +0x10 face count, +0x14 positions (float[3], model space), +0x18 faces,
 *                  +0x74 flags, +0x80 next coarser LOD mesh, +0x84 next finer LOD mesh, +0x88 LOD distance
 *   face (0x2C bytes): +0 u16 v[4], +8 float u[4], +0x18 float v[4], +0x28 u16 texture index (>= 0xFFFE none),
 *                      +0x2A u16 flags (4 = quad, 2 = double-sided)
 *   obj+0x7C: first field -> texture set, +4 -> array of texture objects (flags +0x14, surface +0x44)
 *   obj+0x78 animation (characters): faces of LOD 0 = model DB (anim+0x80) +0x84 -> entry 0: first +8, count +0xC;
 *                  deformed positions (this frame) at anim+0xA4.
 * Static objects are written at their finest LOD (following +0x84). Animated ones are written in their rest pose
 * (model DB +0x14) with every face of the DB, one OBJ group per entry of the DB +0x84 face-range table. Positions are model space (no object transform).
 */
#include <stdio.h>
#include <string.h>
#include "hook.h"

#define D(p, o) (*(DWORD *)((BYTE *)(p) + (o)))
#define F(p, o) (*(float *)((BYTE *)(p) + (o)))
#define P(p, o) (*(BYTE **)((BYTE *)(p) + (o)))
#define W(p, o) (*(WORD *)((BYTE *)(p) + (o)))

#define MAXSEEN 512
static void *g_seen[MAXSEEN];
static int   g_nseen, g_on = -1, g_count;
static void *g_texseen[2048];
static int   g_ntexseen;
static char  g_dir[MAX_PATH];
static BYTE *g_t84;                 /* animated: face-range table of the model DB being written */
static DWORD g_n98;

void meshdump_level_loaded(const char *level_file)
{
    char name[MAX_PATH];
    const char *base;
    int i;
    if (g_on < 0) g_on = hook_ini_int("dump", "meshes", 0);
    if (!g_on) return;
    g_nseen = 0; g_ntexseen = 0; g_count = 0;
    GetModuleFileNameA(NULL, g_dir, sizeof(g_dir));
    if (strrchr(g_dir, '\\')) strrchr(g_dir, '\\')[1] = 0;
    strncat(g_dir, "drakan_dump", sizeof(g_dir) - strlen(g_dir) - 1);
    CreateDirectoryA(g_dir, NULL);
    base = level_file ? (strrchr(level_file, '\\') ? strrchr(level_file, '\\') + 1 : level_file) : "unknown";
    _snprintf(name, sizeof(name), "%s", base); name[sizeof(name) - 1] = 0;
    for (i = 0; name[i]; i++) if (name[i] == '.' || name[i] == ' ' || name[i] == ':') name[i] = '_';
    strncat(g_dir, "\\meshes_", sizeof(g_dir) - strlen(g_dir) - 1);
    strncat(g_dir, name, sizeof(g_dir) - strlen(g_dir) - 1);
    CreateDirectoryA(g_dir, NULL);
    hook_log("[mesh] dumping drawn models to %s\n", g_dir);
}

static BYTE *texture_of(BYTE *obj, WORD idx)
{
    BYTE *set, *arr;
    if (idx >= 0xFFFE) return NULL;
    set = P(obj, 0x7C);
    if (!set) return NULL;
    arr = P(set, 4);
    return arr ? ((BYTE **)arr)[idx] : NULL;
}

static void write_mesh(BYTE *obj, BYTE *mesh, BYTE *anim, int levels)
{
    char path[MAX_PATH], tpath[MAX_PATH];
    FILE *f, *m;
    BYTE *pos, *faces;
    DWORD nv = D(mesh, 0x0C), first = 0, nf = D(mesh, 0x10), i, k, quads = 0, dbl = 0, bad = 0;
    WORD texused[256]; int ntexused = 0;
    pos = P(mesh, 0x14);
    faces = P(mesh, 0x18);
    if (anim) {   /* rest pose + every face of the model DB (all LODs / parts), grouped by the +0x84 table */
        BYTE *db = P(anim, 0x80);
        if (db) {
            BYTE *t84 = P(db, 0x84) ? *(BYTE **)P(db, 0x84) : NULL;
            DWORD k2, n98 = D(db, 0x98);
            hook_log("[mesh]   animated: obj+74 %p db %p, db verts %u faces %u, +98 %u entries; +84 table:", mesh, db, D(db, 0x0C), D(db, 0x10), n98);
            for (k2 = 0; t84 && k2 < n98 && k2 < 16; k2++) hook_log(" [%u: first %u count %u]", k2, D(t84 + k2 * 0x20, 8), D(t84 + k2 * 0x20, 0xC));
            hook_log("\n");
            mesh = db;                                   /* positions, faces and counts from the DB */
            /* best LOD only: faces from 0 up to where LOD 1 starts (LOD 1+ number their vertices from their own base).
             * The +0x84 count covers the body; the faces after it (up to the next LOD) are extra parts such as the hands. */
            pos = P(db, 0x14); faces = P(db, 0x18); nv = D(db, 0x0C); first = 0;
            nf = (t84 && n98 > 1) ? D(t84 + 0x20, 8) : D(db, 0x10);
            g_t84 = t84; g_n98 = n98;
        }
    }
    if (!pos || !faces || nv == 0 || nv > 200000 || nf > 200000) return;
    g_count++;
    _snprintf(path, sizeof(path), "%s\\mesh_%03d.obj", g_dir, g_count); path[sizeof(path) - 1] = 0;
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Drakan object %p mesh %p%s, %u vertices, faces %u..%u, model space\nmtllib mesh_%03d.mtl\n",
            obj, mesh, anim ? " (animated: rest pose, best LOD with its extra parts)" : "", nv, first, first + nf - 1, g_count);
    for (i = 0; i < nv; i++) fprintf(f, "v %.3f %.3f %.3f\n", F(pos, i * 12), F(pos, i * 12 + 4), F(pos, i * 12 + 8));
    for (i = 0; i < nf; i++) {               /* one vt per face corner, 4 per face */
        BYTE *fc = faces + (first + i) * 0x2C;
        for (k = 0; k < 4; k++) fprintf(f, "vt %.5f %.5f\n", F(fc, 8 + k * 4), 1.0f - F(fc, 0x18 + k * 4));
    }
    {
        int cur = -2;
        for (i = 0; i < nf; i++) {
            BYTE *fc = faces + (first + i) * 0x2C;
            WORD tex = W(fc, 0x28), fl = W(fc, 0x2A);
            int n = (fl & 4) ? 4 : 3, ok = 1, t;
            for (k = 0; k < (DWORD)n; k++) if (W(fc, k * 2) >= nv) ok = 0;
            if (!ok) { bad++; continue; }
            if (fl & 4) quads++;
            if (fl & 2) dbl++;
            if (anim && g_t84) {                 /* start of a face range of the +0x84 table: new OBJ group */
                DWORD k3;
                for (k3 = 0; k3 < 1; k3++) {
                    if (first + i == D(g_t84, 8)) { fprintf(f, "g lod0_body\n"); cur = -2; }
                    if (first + i == D(g_t84, 8) + D(g_t84, 0xC)) { fprintf(f, "g lod0_extra_parts\n"); cur = -2; }
                }
            }
            if ((int)tex != cur) {
                cur = tex;
                fprintf(f, "usemtl %s%u\n", tex >= 0xFFFE ? "none" : "tex_", tex >= 0xFFFE ? 0 : tex);
                for (t = 0; t < ntexused && texused[t] != tex; t++) ;
                if (t == ntexused && ntexused < 256) texused[ntexused++] = tex;
            }
            fprintf(f, "f");
            for (k = 0; k < (DWORD)n; k++) fprintf(f, " %u/%u", W(fc, k * 2) + 1, i * 4 + k + 1);
            fprintf(f, "\n");
        }
    }
    fclose(f);
    _snprintf(path, sizeof(path), "%s\\mesh_%03d.mtl", g_dir, g_count); path[sizeof(path) - 1] = 0;
    m = fopen(path, "w");
    if (m) {
        int t;
        fprintf(m, "newmtl none0\nKd 0.8 0.8 0.8\n\n");
        for (t = 0; t < ntexused; t++) {
            BYTE *tx = texture_of(obj, texused[t]);
            int j, done = 0;
            if (texused[t] >= 0xFFFE) continue;
            fprintf(m, "newmtl tex_%u\nKd 1 1 1\n", texused[t]);
            if (tx) {
                _snprintf(tpath, sizeof(tpath), "tex_%p.dds", tx); tpath[sizeof(tpath) - 1] = 0;
                for (j = 0; j < g_ntexseen; j++) if (g_texseen[j] == tx) done = 1;
                if (!done) {
                    char full[MAX_PATH];
                    _snprintf(full, sizeof(full), "%s\\%s", g_dir, tpath); full[sizeof(full) - 1] = 0;
                    if (texdump_texture(tx, full) && g_ntexseen < 2048) g_texseen[g_ntexseen++] = tx;
                }
                fprintf(m, "map_Kd %s\n# texture object %p flags %08X\n", tpath, tx, D(tx, 0x14));
            }
            fprintf(m, "\n");
        }
        fclose(m);
    }
    hook_log("[mesh] #%03d obj %p mesh %p (%d LOD levels%s) %u verts, faces %u..%u (%u quads, %u double-sided, %u bad), %d textures,"
             " obj +4 %08X +8C %08X mesh +74 %08X, at %.0f %.0f %.0f\n",
             g_count, obj, mesh, levels, anim ? ", animated" : "", nv, first, first + nf - 1, quads, dbl, bad, ntexused,
             D(obj, 4), D(obj, 0x8C), D(mesh, 0x74), F(obj, 8), F(obj, 0xC), F(obj, 0x10));
}

void meshdump_object(BYTE *obj)
{
    BYTE *mesh, *anim, *best;
    int i, levels = 1;
    if (g_on <= 0 || !g_dir[0] || g_nseen >= MAXSEEN) return;
    __try {
        mesh = P(obj, 0x74);
        anim = P(obj, 0x78);
        if (!mesh) return;
        best = mesh;
        if (!anim) {                      /* finest LOD: follow +0x84 */
            while (P(best, 0x84) && levels < 16) { best = P(best, 0x84); levels++; }
            { BYTE *c = mesh; while (P(c, 0x80) && levels < 32) { c = P(c, 0x80); levels++; } }
        }
        for (i = 0; i < g_nseen; i++) if (g_seen[i] == (anim ? (void *)P(anim, 0x80) : (void *)best)) return;
        g_seen[g_nseen++] = anim ? (void *)P(anim, 0x80) : (void *)best;
        write_mesh(obj, best, anim, levels);
        hook_log_flush();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hook_log("[mesh] exception reading object %p\n", obj);
    }
}
