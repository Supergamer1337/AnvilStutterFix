// The game loads this DLL as version.dll from its folder, so every export is forwarded to the real one in System32.
#include "common.h"

#define VERSION_EXPORTS(X)                                                                  \
    X(GetFileVersionInfoA) X(GetFileVersionInfoByHandle) X(GetFileVersionInfoExA)           \
    X(GetFileVersionInfoExW) X(GetFileVersionInfoSizeA) X(GetFileVersionInfoSizeExA)        \
    X(GetFileVersionInfoSizeExW) X(GetFileVersionInfoSizeW) X(GetFileVersionInfoW)          \
    X(VerFindFileA) X(VerFindFileW) X(VerInstallFileA) X(VerInstallFileW)                   \
    X(VerLanguageNameA) X(VerLanguageNameW) X(VerQueryValueA) X(VerQueryValueW)

extern "C" {
#define DECLARE_SLOT(name) void* g_real_##name = nullptr;
VERSION_EXPORTS(DECLARE_SLOT)
#undef DECLARE_SLOT
}

// We simply jump to the original function in version
#define DEFINE_STUB(name)                         \
    __asm__(".section .text\n"                    \
            ".globl Proxy_" #name "\n"            \
            "Proxy_" #name ":\n"                  \
            "    jmpq *g_real_" #name "(%rip)\n");
VERSION_EXPORTS(DEFINE_STUB)
#undef DEFINE_STUB

bool Proxy_Init()
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return false;
    lstrcatW(path, L"\\version.dll");

    HMODULE real = LoadLibraryW(path);
    if (!real) return false;

#define RESOLVE(name) if (!(g_real_##name = reinterpret_cast<void*>(GetProcAddress(real, #name)))) return false;
    VERSION_EXPORTS(RESOLVE)
#undef RESOLVE
    return true;
}
