/*
 * freecam.c - detachable fly camera (noclip view) for inspecting levels.
 *
 * Hooks the camera builder 0x40A160 (thiscall: ecx = camera, arg = xform {pos[3], R[3][3]},
 * ret 4; F-015). The main loop 0x407D40 calls it every frame with the global camera
 * 0x4879B0 and the view object's transform, just before the frame render 0x43BEB0.
 * While the free camera is on we pass our own position instead; the rotation stays the
 * game's, so mouse look still works through the game. Only the view moves: the player
 * stays where it is (but still reacts to the game's own movement keys).
 *
 * Keys (Windows virtual-key codes, overridable in [freecam] of drakan_hook.ini; NumLock on):
 *   F9 toggle | Num8/Num5 forward/back (along the view, incl. pitch) | Num4/Num6 strafe
 *   Num9/Num3 up/down (world Y) | Num1/Num7 speed x2 / x0.5 | Num. back to the game camera
 *   F11 log the camera position (works with the free camera off too)
 * [freecam] move_sky=1 also draws the sky dome around the free camera (see H_drawobj).
 */
#include <math.h>
#include "hook.h"
#include "be.h"

#define A_CAMBUILD 0x0040A160
#define G_CAMERA   0x004879B0
#define CELL       2048.0f      /* terrain cell size in world units (F-021) */

typedef void (__fastcall *cambuild_fn)(void *cam, void *edx, const float *xform);
typedef int  (__fastcall *drawobj_fn)(BYTE *obj, void *edx, float *camera);
static cambuild_fn O_cambuild;
static drawobj_fn  O_drawobj;
static float g_game_pos[3];              /* where the game wanted the camera this frame */
static int   g_sky_logged, cfg_sky;

static int   fc_on;
static float fc_pos[3], fc_speed;
static LARGE_INTEGER fc_freq, fc_last;
static int   k_toggle, k_fwd, k_back, k_left, k_right, k_up, k_down, k_faster, k_slower, k_reset, k_logpos;
static SHORT prev_state[256];

static int focused(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
static int down(int vk) { return vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0; }
static int pressed(int vk)                 /* edge: up -> down since the last check */
{
    SHORT now;
    int r;
    if (vk <= 0 || vk >= 256) return 0;
    now = (SHORT)(GetAsyncKeyState(vk) & 0x8000);
    r = now && !prev_state[vk];
    prev_state[vk] = now;
    return r;
}

static void log_position(const float *pos, const float *xf)
{
    /* forward = -(column 2 of R), R[i][j] = xf[3 + 3i + j] */
    float fx = -xf[5], fy = -xf[8], fz = -xf[11];
    hook_log("[freecam] %s camera at %.1f %.1f %.1f (cell x %d, z %d)  looking %.3f %.3f %.3f  speed %.0f/s\n",
             fc_on ? "FREE" : "game", pos[0], pos[1], pos[2],
             (int)floor(pos[0] / CELL), (int)floor(pos[2] / CELL), fx, fy, fz, fc_speed);
    hook_log_flush();
}

static void __fastcall H_cambuild(void *cam, void *edx, const float *xf)
{
    float x[12];
    LARGE_INTEGER now;
    float dt;
    int i, have_focus;

    if (cam != (void *)G_CAMERA) { O_cambuild(cam, edx, xf); return; }

    QueryPerformanceCounter(&now);
    dt = fc_last.QuadPart ? (float)(now.QuadPart - fc_last.QuadPart) / (float)fc_freq.QuadPart : 0.0f;
    if (dt > 0.1f) dt = 0.1f;               /* load screens, breakpoints: no huge jumps */
    fc_last = now;
    have_focus = focused();

    if (have_focus) {
        if (pressed(k_toggle)) {
            fc_on = !fc_on;     /* always start from where the game camera is */
            g_sky_logged = 0;
            if (fc_on) { fc_pos[0] = xf[0]; fc_pos[1] = xf[1]; fc_pos[2] = xf[2]; }
            hook_log("[freecam] %s\n", fc_on ? "on" : "off");
            log_position(fc_on ? fc_pos : xf, xf);
        }
        if (pressed(k_reset)) { fc_pos[0] = xf[0]; fc_pos[1] = xf[1]; fc_pos[2] = xf[2]; }
        if (pressed(k_faster)) fc_speed *= 2.0f;
        if (pressed(k_slower)) fc_speed *= 0.5f;
        if (pressed(k_logpos)) log_position(fc_on ? fc_pos : xf, xf);
    }

    g_game_pos[0] = xf[0]; g_game_pos[1] = xf[1]; g_game_pos[2] = xf[2];
    if (!fc_on) { O_cambuild(cam, edx, xf); return; }

    if (have_focus) {
        /* view axes in world space: right = R col 0, forward = -R col 2 */
        float rx = xf[3], ry = xf[6], rz = xf[9];
        float fx = -xf[5], fy = -xf[8], fz = -xf[11];
        float f = 0, s = 0, u = 0, step = fc_speed * dt;
        if (down(k_fwd))   f += 1;
        if (down(k_back))  f -= 1;
        if (down(k_right)) s += 1;
        if (down(k_left))  s -= 1;
        if (down(k_up))    u += 1;
        if (down(k_down))  u -= 1;
        fc_pos[0] += step * (f * fx + s * rx);
        fc_pos[1] += step * (f * fy + s * ry + u);
        fc_pos[2] += step * (f * fz + s * rz);
    }
    for (i = 0; i < 12; i++) x[i] = xf[i];
    x[0] = fc_pos[0]; x[1] = fc_pos[1]; x[2] = fc_pos[2];
    O_cambuild(cam, edx, x);
}

/* Draw object 0x446E70 (thiscall obj, camera; ret 4; F-016). The sky dome is placed by the game
 * at the game camera's x,z (F-019/F-026), so with the free camera it stays behind and, once we
 * leave it, nothing covers the background (hall of mirrors). Any meshed object whose x,z is
 * exactly the game camera's is drawn shifted by (free - game) for this call only; its stored
 * position is restored right after, so game state is untouched. */
static int draw_obj_moved(BYTE *obj, void *edx, float *camera);
static int __fastcall H_drawobj(BYTE *obj, void *edx, float *camera)
{
    int r;
    LONG blk;
    meshdump_object(obj);                           /* M4 step 1: [dump] meshes */
    blk = r3d_enter(1);                             /* M3: the game's objects are replaced (remix3d.c) */
    r = draw_obj_moved(obj, edx, camera);
    r3d_leave(blk);
    return r;
}

static int draw_obj_moved(BYTE *obj, void *edx, float *camera)
{
    float save[3], *pos;
    int r;
    if (!fc_on || !cfg_sky || camera != (float *)G_CAMERA) return O_drawobj(obj, edx, camera);
    __try {
        pos = (float *)(obj + 0x08);
        if (*(void **)(obj + 0x74) == NULL ||
            fabs(pos[0] - g_game_pos[0]) > 1.0f || fabs(pos[2] - g_game_pos[2]) > 1.0f)
            return O_drawobj(obj, edx, camera);
        if (!g_sky_logged) {
            BYTE *mesh = *(BYTE **)(obj + 0x74);
            hook_log("[freecam] following-object %p (mesh %p, %u verts, y offset %.1f) drawn at the free camera\n",
                     obj, mesh, *(DWORD *)(mesh + 0x0C), pos[1] - g_game_pos[1]);
            hook_log_flush();
            g_sky_logged = 1;
        }
        save[0] = pos[0]; save[1] = pos[1]; save[2] = pos[2];
        pos[0] += fc_pos[0] - g_game_pos[0];
        pos[1] += fc_pos[1] - g_game_pos[1];
        pos[2] += fc_pos[2] - g_game_pos[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return O_drawobj(obj, edx, camera);
    }
    r = O_drawobj(obj, edx, camera);
    pos[0] = save[0]; pos[1] = save[1]; pos[2] = save[2];
    return r;
}

int freecam_drawobj_hooked(void) { return O_drawobj != NULL; }

int freecam_install(void)
{
    static const BYTE b_cam[] = { 0x8B, 0x44, 0x24, 0x04, 0x53, 0x55 };  /* mov eax,[esp+4]; push ebx; push ebp */
    if (!hook_ini_int("freecam", "enable", 1)) { hook_log("[freecam] disabled in ini\n"); return 0; }
    QueryPerformanceFrequency(&fc_freq);
    fc_speed  = (float)hook_ini_int("freecam", "speed", 4096);   /* world units per second */
    k_toggle  = hook_ini_int("freecam", "key_toggle", VK_F9);
    k_fwd     = hook_ini_int("freecam", "key_forward", VK_NUMPAD8);
    k_back    = hook_ini_int("freecam", "key_back", VK_NUMPAD5);
    k_left    = hook_ini_int("freecam", "key_left", VK_NUMPAD4);
    k_right   = hook_ini_int("freecam", "key_right", VK_NUMPAD6);
    k_up      = hook_ini_int("freecam", "key_up", VK_NUMPAD9);
    k_down    = hook_ini_int("freecam", "key_down", VK_NUMPAD3);
    k_faster  = hook_ini_int("freecam", "key_faster", VK_NUMPAD1);
    k_slower  = hook_ini_int("freecam", "key_slower", VK_NUMPAD7);
    k_reset   = hook_ini_int("freecam", "key_reset", VK_DECIMAL);
    k_logpos  = hook_ini_int("freecam", "key_logpos", VK_F11);
    O_cambuild = (cambuild_fn)detour_install(A_CAMBUILD, b_cam, sizeof(b_cam), H_cambuild);
    cfg_sky = hook_ini_int("freecam", "move_sky", 1);
    {   /* always hooked while freecam is on: move_sky uses it, and so does M3's object blocking (remix3d.c) */
        static const BYTE b_draw[] = { 0x81, 0xEC, 0xC8, 0x00, 0x00, 0x00 };   /* sub esp,0C8 */
        O_drawobj = (drawobj_fn)detour_install(0x00446E70, b_draw, sizeof(b_draw), H_drawobj);
        if (!O_drawobj) cfg_sky = 0;
    }
    hook_log("[freecam] sky follows free camera: %s\n", cfg_sky ? "yes" : "no");
    hook_log("[freecam] camera builder hook %s (toggle key 0x%02X, speed %.0f/s)\n",
             O_cambuild ? "installed" : "NOT installed (bytes differ)", k_toggle, fc_speed);
    return (O_cambuild ? 1 : 0) + (O_drawobj ? 1 : 0);
}
