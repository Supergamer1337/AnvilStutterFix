#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>

// Debug builds (ASF_DEBUG=1) add the ini, hotkeys, detailed stats and an event trace.
#ifndef ASF_DEBUG
#define ASF_DEBUG 0
#endif
#ifndef ASF_VERSION
#define ASF_VERSION "dev"
#endif

// Defaults tuned in Valhalla. Release builds compile them in, debug builds read AnvilStutterFix.ini.
// Each setting is described in kSettings in debug.cpp.
struct Config
{
    int deferReleases = 1;
    int poolEnabled   = 1;
    int gentle        = 1;
    int traceEvents   = 0;

    int tickMs             = 1;
    int minAgeMs           = 100;
    int maxReleasesPerTick = 1;
    int freeBudgetUs       = 800;
    int quietMs            = 8;
    int maxQueue           = 4096;

    int poolMinCostUs         = 40;
    int poolSparesPerKey      = 1;
    int poolTtlMs             = 60000;
    int poolPreCreatesPerTick = 4;
    int poolPreCreateBudgetUs = 600;
    int poolMaxSize           = 4096;

    int statsIntervalMs = 5000;
};

#if ASF_DEBUG
extern Config g_cfg;
#else
constexpr Config g_cfg{};
#endif

std::wstring FileNextToDll(const wchar_t* name);

// Log lines are buffered and written to AnvilStutterFix.log next to the DLL by a background thread.
void Log_Init(HMODULE self);
void Log_OpenEvents(const wchar_t* name);
void Log_Flush();
void Log(const char* fmt, ...);
void LogEvent(const char* fmt, ...);

void    Timing_Init();
int64_t Qpc();
double  QpcToUs(int64_t ticks);
double  NowMs();   // since the DLL was loaded

bool Proxy_Init();
void Hooks_Install();

inline void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
