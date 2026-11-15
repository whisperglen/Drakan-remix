/* main.c - DllMain, config (drakan_hook.ini) and the log file (drakan_hook.log),
 * both in the game folder (folder of the running exe). */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "hook.h"

static HMODULE g_self;
static char    g_dir[MAX_PATH];        /* game folder, with trailing backslash */
static char    g_ini[MAX_PATH];
static FILE   *g_log;
static CRITICAL_SECTION g_log_cs;

HMODULE hook_self_module(void) { return g_self; }

int hook_ini_int(const char *section, const char *key, int def)
{
    return (int)GetPrivateProfileIntA(section, key, def, g_ini);
}

/* ini value naming a DLL: empty = <system32>\sysname; absolute; or relative to the game folder */
void hook_ini_path(const char *section, const char *key, const char *sysname, char *out, DWORD size)
{
    char v[MAX_PATH];
    GetPrivateProfileStringA(section, key, "", v, sizeof(v), g_ini);
    if (v[0] == 0) {
        GetSystemDirectoryA(out, size);
        strncat(out, "\\", size - strlen(out) - 1);
        strncat(out, sysname, size - strlen(out) - 1);
    } else if (v[1] == ':' || v[0] == '\\') {
        strncpy(out, v, size); out[size - 1] = 0;
    } else {
        _snprintf(out, size, "%s%s", g_dir, v); out[size - 1] = 0;
    }
}

void hook_chain_path(char *out, DWORD size) { hook_ini_path("chain", "ddraw", "ddraw.dll", out, size); }

void hook_log(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    EnterCriticalSection(&g_log_cs);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    LeaveCriticalSection(&g_log_cs);
}

void hook_log_flush(void) { if (g_log) fflush(g_log); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], *slash;
        SYSTEMTIME st;
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_log_cs);

        GetModuleFileNameA(NULL, g_dir, sizeof(g_dir));
        slash = strrchr(g_dir, '\\');
        if (slash) slash[1] = 0;
        _snprintf(g_ini, sizeof(g_ini), "%sdrakan_hook.ini", g_dir);
        _snprintf(path, sizeof(path), "%sdrakan_hook.log", g_dir);
        {   /* keep the previous run's log (useful when a run fails) */
            char prev[MAX_PATH];
            _snprintf(prev, sizeof(prev), "%sdrakan_hook.prev.log", g_dir);
            MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
        }
        g_log = fopen(path, "w");

        GetLocalTime(&st);
        hook_log("drakan_hook (ddraw proxy) %04d-%02d-%02d %02d:%02d:%02d, game folder %s\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, g_dir);
        if (hook_ini_int("hooks", "enable", 1))
            hook_log("[main] %d hooks installed\n", drakan_install_hooks());
        else
            hook_log("[main] hooks disabled in ini (proxy only)\n");
        hook_log_flush();
    } else if (reason == DLL_PROCESS_DETACH) {
        census_report(reserved ? "process exit" : "unload");
        hook_log_flush();
        if (reserved) return TRUE;      /* process exit: the CRT closes the file itself */
        hook_log("[main] unload\n");
        if (g_log) fclose(g_log);
        g_log = NULL;
    }
    return TRUE;
}
