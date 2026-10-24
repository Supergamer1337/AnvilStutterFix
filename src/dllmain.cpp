#include "debug.h"

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(self);
        if (!Proxy_Init()) return FALSE;   // the game cannot run without the real version.dll
        Timing_Init();
        Log_Init(self);
        Debug_Init();
        Hooks_Install();
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        Log_Flush();
    }
    return TRUE;
}
