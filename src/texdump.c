/*
 * texdump.c - dump a level's terrain textures after load (R.8-B).
 *
 * The world's texture table (world+0x2838: {tex **array, capacity, count}) is what the
 * terrain cells' texA/texB index (0x4410E0: tex = array[id]). Each entry is a texture
 * object created by 0x434260; its converted pixels live in a system-memory DirectDraw
 * surface at +0x44 (IDirectDrawSurface4, also the upload source in 0x43E390).
 * We Lock that surface read-only, convert to 32-bit BGRA and write an uncompressed DDS
 * (A8R8G8B8, readable by RTX Remix and Blender), plus manifest.txt with the object fields
 * (flags +0x14 hold the blend/alpha mode bits, see F-029).
 */
#define CINTERFACE
#define COBJMACROS
#include <stdio.h>
#include <string.h>
#include <ddraw.h>
#include "hook.h"

#define D(p, off)  (*(DWORD *)((BYTE *)(p) + (off)))
#define F(p, off)  (*(float *)((BYTE *)(p) + (off)))
#define P(p, off)  (*(BYTE **)((BYTE *)(p) + (off)))

static void wr(HANDLE f, const void *p, DWORD n) { DWORD w; WriteFile(f, p, n, &w, NULL); }

static int write_dds(const char *path, int w, int h, const DWORD *bgra)
{
    DWORD hdr[32];
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0x20534444;            /* "DDS " */
    hdr[1] = 124;                   /* header size */
    hdr[2] = 0x100F;                /* CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT */
    hdr[3] = h; hdr[4] = w; hdr[5] = w * 4;
    /* [6] depth, [7] mip count, [8..18] reserved; DDS_PIXELFORMAT starts at [19] */
    hdr[19] = 32; hdr[20] = 0x41;   /* pixel format: size, RGB | ALPHAPIXELS */
    hdr[22] = 32;                   /* [21] FourCC = 0 */
    hdr[23] = 0x00FF0000; hdr[24] = 0x0000FF00; hdr[25] = 0x000000FF; hdr[26] = 0xFF000000;
    hdr[27] = 0x1000;               /* DDSCAPS_TEXTURE */
    wr(f, hdr, sizeof(hdr));
    wr(f, bgra, w * h * 4);
    CloseHandle(f);
    return 1;
}

static DWORD expand(DWORD v, DWORD mask)      /* masked channel -> 0..255 */
{
    int shift = 0, bits = 0;
    if (!mask) return 255;
    while (!((mask >> shift) & 1)) shift++;
    while ((mask >> (shift + bits)) & 1) bits++;
    v = (v & mask) >> shift;
    if (bits >= 8) return v >> (bits - 8);
    return (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
}

/* Returns a GlobalAlloc'd BGRA copy of the surface, or NULL (reason in *why). */
static DWORD *read_surface(IDirectDrawSurface4 *s, int *pw, int *ph, DDPIXELFORMAT *pf, const char **why)
{
    DDSURFACEDESC2 sd;
    PALETTEENTRY pal[256];
    int x, y, w, h, bpp, have_pal = 0, ck = -1;
    DWORD *out;
    HRESULT hr;

    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    if (IDirectDrawSurface4_GetSurfaceDesc(s, &sd) != DD_OK) { *why = "GetSurfaceDesc failed"; return NULL; }
    w = sd.dwWidth; h = sd.dwHeight; *pf = sd.ddpfPixelFormat; bpp = pf->dwRGBBitCount;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { *why = "bad size"; return NULL; }
    if (pf->dwFlags & (DDPF_PALETTEINDEXED8 | DDPF_PALETTEINDEXED4)) {
        IDirectDrawPalette *p = NULL;
        DDCOLORKEY key;
        if (IDirectDrawSurface4_GetPalette(s, &p) == DD_OK && p) {
            have_pal = IDirectDrawPalette_GetEntries(p, 0, 0, 256, pal) == DD_OK;
            IDirectDrawPalette_Release(p);
        }
        if (!have_pal) { *why = "palettized without palette"; return NULL; }
        if (IDirectDrawSurface4_GetColorKey(s, DDCKEY_SRCBLT, &key) == DD_OK) ck = (int)key.dwColorSpaceLowValue;
        bpp = (pf->dwFlags & DDPF_PALETTEINDEXED8) ? 8 : 4;
    } else if (!(pf->dwFlags & DDPF_RGB) || (bpp != 16 && bpp != 24 && bpp != 32)) {
        *why = "unsupported pixel format"; return NULL;
    }
    out = (DWORD *)GlobalAlloc(GMEM_FIXED, w * h * 4);
    if (!out) { *why = "out of memory"; return NULL; }

    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    hr = IDirectDrawSurface4_Lock(s, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_NOSYSLOCK, NULL);
    if (hr != DD_OK) { GlobalFree(out); *why = "Lock failed"; return NULL; }
    __try {
        for (y = 0; y < h; y++) {
            const BYTE *row = (const BYTE *)sd.lpSurface + y * sd.lPitch;
            for (x = 0; x < w; x++) {
                DWORD c;
                if (bpp <= 8) {
                    int i = bpp == 8 ? row[x] : ((row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15);  /* 4-bit: high nibble first (unverified) */
                    c = (i == ck ? 0 : 0xFF000000) | (pal[i].peRed << 16) | (pal[i].peGreen << 8) | pal[i].peBlue;
                } else {
                    DWORD v = bpp == 16 ? *(const WORD *)(row + 2 * x)
                            : bpp == 24 ? (row[3 * x] | (row[3 * x + 1] << 8) | (row[3 * x + 2] << 16))
                            : *(const DWORD *)(row + 4 * x);
                    DWORD a = (pf->dwFlags & DDPF_ALPHAPIXELS) ? expand(v, pf->dwRGBAlphaBitMask) : 255;
                    c = (a << 24) | (expand(v, pf->dwRBitMask) << 16) | (expand(v, pf->dwGBitMask) << 8) | expand(v, pf->dwBBitMask);
                }
                out[y * w + x] = c;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        IDirectDrawSurface4_Unlock(s, NULL);
        GlobalFree(out); *why = "exception reading pixels"; return NULL;
    }
    IDirectDrawSurface4_Unlock(s, NULL);
    *pw = w; *ph = h;
    return out;
}

/* dir: folder to create and fill (e.g. ...\drakan_dump\level_001_X_tex) */
void texdump_world(BYTE *world, const char *dir)
{
    BYTE **arr;
    DWORD count, i, ok = 0;
    char path[MAX_PATH];
    FILE *man;

    __try {
        arr = (BYTE **)P(world, 0x2838);
        count = D(world, 0x2838 + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) { hook_log("[tex] cannot read the texture table\n"); return; }
    if (!arr || count == 0 || count > 4096) { hook_log("[tex] texture table empty or invalid (count %u)\n", count); return; }

    CreateDirectoryA(dir, NULL);
    _snprintf(path, sizeof(path), "%s\\manifest.txt", dir); path[sizeof(path) - 1] = 0;
    man = fopen(path, "w");
    if (man) fprintf(man, "# index id flags(+14) w(+18) h(+1C) +28 +2C next(+38) +3C detail(+4C) detail_uv(+50) anim_cb(+58) | surface: w h bpp pf_flags masks R G B A | file\n");

    for (i = 0; i < count; i++) {
        BYTE *t = arr[i];
        IDirectDrawSurface4 *s;
        DWORD *px;
        int w = 0, h = 0;
        DDPIXELFORMAT pf;
        const char *why = "";
        if (!t) { if (man) fprintf(man, "%u null\n", i); continue; }
        __try {
            if (man) fprintf(man, "%u %08X %08X %u %u %08X %08X %p %p %p %.4f %p", i, D(t, 4), D(t, 0x14),
                             D(t, 0x18), D(t, 0x1C), D(t, 0x28), D(t, 0x2C), P(t, 0x38), P(t, 0x3C),
                             P(t, 0x4C), F(t, 0x50), P(t, 0x58));
            s = (IDirectDrawSurface4 *)P(t, 0x44);
        } __except (EXCEPTION_EXECUTE_HANDLER) { if (man) fprintf(man, " | exception\n"); continue; }
        if (!s) { if (man) fprintf(man, " | no surface\n"); continue; }
        memset(&pf, 0, sizeof(pf));
        px = read_surface(s, &w, &h, &pf, &why);
        if (!px) { if (man) fprintf(man, " | %s\n", why); continue; }
        _snprintf(path, sizeof(path), "%s\\tex_%03u.dds", dir, i); path[sizeof(path) - 1] = 0;
        if (write_dds(path, w, h, px)) ok++;
        if (man) fprintf(man, " | %d %d %u %08X %08X %08X %08X %08X | tex_%03u.dds\n", w, h, pf.dwRGBBitCount,
                         pf.dwFlags, pf.dwRBitMask, pf.dwGBitMask, pf.dwBBitMask, pf.dwRGBAlphaBitMask, i);
        GlobalFree(px);
    }
    if (man) fclose(man);
    hook_log("[tex] %u of %u textures written to %s\n", ok, count, dir);
    hook_log_flush();
}

/* one texture object -> DDS (used by meshdump.c); 1 = written */
int texdump_texture(BYTE *t, const char *path)
{
    IDirectDrawSurface4 *s;
    DWORD *px;
    int w = 0, h = 0, ok;
    DDPIXELFORMAT pf;
    const char *why = "";
    __try { s = (IDirectDrawSurface4 *)P(t, 0x44); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!s) return 0;
    memset(&pf, 0, sizeof(pf));
    px = read_surface(s, &w, &h, &pf, &why);
    if (!px) return 0;
    ok = write_dds(path, w, h, px);
    GlobalFree(px);
    return ok;
}
