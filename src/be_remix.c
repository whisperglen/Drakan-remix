/*
 * be_remix.c - M2: the Remix API through the 32-bit bridge (REMIX.md sections 1 and 5).
 *
 * After our D3D9 device exists, we ask the d3d9.dll it came from for "remixapi_InitializeLibrary". Only the
 * Remix bridge client has it; the system d3d9.dll doesn't, and then this file does nothing. M2 only finds out
 * what the bridge offers:
 *   - which remixapi_Interface functions are filled in (the user's experience: not all of them, through the bridge);
 *   - whether remixapi_RegisterCallbacks exists, and when its BeginScene / EndScene / Present callbacks fire
 *     (thread, and order relative to our own calls).
 * Nothing is drawn through the API yet (M3: camera + terrain).
 * Separate file because remix_c.h and d3d9.h both declare the IDirect3D9Ex types.
 */
#include <windows.h>
#include <string.h>
#define REMIX_ALLOW_X86          /* remix_c.h is used for its types only on x86 (see bridge_remix_api.h) */
#include "../remix/remix_c.h"
#include "hook.h"
#include "be.h"

typedef void (__cdecl *rx_cb)(void);
typedef remixapi_ErrorCode (REMIXAPI_CALL *rx_register_fn)(rx_cb begin_scene, rx_cb end_scene, rx_cb present);

static remixapi_Interface g_rx;
static int g_rx_ok;
static DWORD g_render_thread;
static volatile LONG g_cb_count[3];

int rx_ready(void) { return g_rx_ok; }
const void *rx_api(void) { return g_rx_ok ? &g_rx : NULL; }

static void cb_note(int k, const char *name)
{
    LONG n = InterlockedIncrement(&g_cb_count[k]);
    if (n == 1 || n == 1000)
        hook_log("[remix] bridge callback %s #%ld on thread %lu (render thread %lu)\n", name, n,
                 GetCurrentThreadId(), g_render_thread);
    if (n == 1) hook_log_flush();
}
static void __cdecl cb_begin(void)   { cb_note(0, "BeginScene"); }
static void __cdecl cb_end(void)     { cb_note(1, "EndScene"); }
static void __cdecl cb_present(void) { cb_note(2, "Present"); }

void rx_init(void *d3d9_module)
{
    static int tried;
    PFN_remixapi_InitializeLibrary init;
    rx_register_fn reg;
    remixapi_InitializeLibraryInfo info;
    remixapi_ErrorCode e;
    HMODULE m = (HMODULE)d3d9_module;
    if (tried) return;
    tried = 1;
    g_render_thread = GetCurrentThreadId();
    if (!hook_ini_int("remix", "api", 1)) { hook_log("[remix] API disabled in ini\n"); return; }
    init = m ? (PFN_remixapi_InitializeLibrary)GetProcAddress(m, "remixapi_InitializeLibrary") : NULL;
    if (!init) { hook_log("[remix] this d3d9.dll has no remixapi_InitializeLibrary: not the Remix bridge, API off\n"); return; }

    memset(&info, 0, sizeof(info));
    info.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
    info.version = REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    memset(&g_rx, 0, sizeof(g_rx));
    e = init(&info, &g_rx);
    hook_log("[remix] remixapi_InitializeLibrary (header %d.%d.%d) -> error %d%s\n", REMIXAPI_VERSION_MAJOR,
             REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH, (int)e,
             e == REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION ? " (bridge/runtime version differs from our header)" : "");
    if (e != REMIXAPI_ERROR_CODE_SUCCESS) return;
    g_rx_ok = 1;

#define P(f) hook_log("[remix]   %-26s %s\n", #f, g_rx.f ? "yes" : "-")
    hook_log("[remix] interface functions provided by the bridge:\n");
    P(Shutdown); P(CreateMaterial); P(DestroyMaterial); P(CreateMesh); P(DestroyMesh); P(SetupCamera);
    P(DrawInstance); P(CreateLight); P(DestroyLight); P(DrawLightInstance); P(SetConfigVariable);
    P(dxvk_CreateD3D9); P(dxvk_RegisterD3D9Device); P(dxvk_GetExternalSwapchain); P(dxvk_GetVkImage);
    P(dxvk_CopyRenderingOutput); P(dxvk_SetDefaultOutput); P(pick_RequestObjectPicking); P(pick_HighlightObjects);
    P(Startup); P(Present); P(SetCameraMediumMaterial);
#undef P

    reg = (rx_register_fn)GetProcAddress(m, "remixapi_RegisterCallbacks");
    if (reg && !hook_ini_int("remix", "callbacks", 1)) hook_log("[remix] RegisterCallbacks skipped ([remix] callbacks=0)\n");
    else if (reg) {
        e = reg(cb_begin, cb_end, cb_present);
        hook_log("[remix] remixapi_RegisterCallbacks -> error %d\n", (int)e);
    } else hook_log("[remix] no remixapi_RegisterCallbacks export\n");
    hook_log_flush();
}

/* our own frame events, to compare with the bridge callbacks in the log (first two frames only) */
void rx_note_frame_event(const char *what)
{
    static int n;
    if (!g_rx_ok || n >= 12) return;
    n++;
    hook_log("[remix] our %s (callbacks so far: begin %ld, end %ld, present %ld)\n", what,
             g_cb_count[0], g_cb_count[1], g_cb_count[2]);
    hook_log_flush();                  /* a hang inside the bridge must not lose these lines */
}

/* The Remix menu (ImGui, Alt+X by default) takes the input through the bridge's DirectInput hook. The game's mouse loses
 * its DirectInput acquisition, but the window never loses focus, so the game (WndProc 0x412B70: it re-acquires on
 * WM_ACTIVATE / WM_EXITSIZEMOVE) never takes it back. When the menu hotkey closes the menu, post the game window a
 * deactivate + activate pair so its own code re-acquires. [remix] menu_key (virtual-key, default 'X'), menu_alt (1 = with Alt),
 * reacquire_after_menu (1 = on). */
void rx_frame_tick(HWND hwnd)
{
    static int init, on, key, alt, open, was_down, pending;
    int down;
    if (!g_rx_ok || !hwnd) return;
    if (!init) {
        init = 1;
        on = hook_ini_int("remix", "reacquire_after_menu", 1);
        key = hook_ini_int("remix", "menu_key", 'X');
        alt = hook_ini_int("remix", "menu_alt", 1);
    }
    if (!on) return;
    if (pending && --pending == 0) {
        PostMessageA(hwnd, WM_ACTIVATE, WA_INACTIVE, 0);
        PostMessageA(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
        hook_log("[remix] menu closed: re-activated the game window so it takes the mouse back\n");
    }
    if (GetForegroundWindow() != hwnd) { was_down = 0; return; }
    down = (GetAsyncKeyState(key) & 0x8000) && (!alt || (GetAsyncKeyState(VK_MENU) & 0x8000));
    if (down && !was_down) {
        open = !open;
        if (!open) pending = 3;            /* a few frames later, after Remix has released the input */
    }
    was_down = down;
}
