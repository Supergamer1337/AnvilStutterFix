#include "debug.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#if ASF_DEBUG

Config            g_cfg;
// Used for toggling features on/off using shortcuts
std::atomic<bool> g_deferEnabled{true};
std::atomic<bool> g_poolEnabled{true};
std::atomic<bool> g_gentle{true};
std::atomic<bool> g_traceEvents{false};

namespace {

struct Setting
{
    bool          newGroup;
    const char*   key;
    int Config::* field;
    const char*   help;
};
const Setting kSettings[] = {
    { true, "DeferReleases", &Config::deferReleases,
      "Free released placed resources on a background thread. Ctrl+Shift+F12 toggles this in game." },
    { false, "PoolEnabled", &Config::poolEnabled,
      "Have copies of resources the game keeps recreating ready in advance. Ctrl+Shift+F11 toggles this in game." },
    { false, "Gentle", &Config::gentle,
      "Pause background work while the game creates expensive resources. Ctrl+Shift+F10 toggles this in game." },
    { false, "TraceEvents", &Config::traceEvents,
      "Write every resource event to AnvilStutterFix_events.csv. This slows the game threads down.\n"
      "Ctrl+Shift+F9 toggles this in game." },

    { true, "TickMs", &Config::tickMs, "How often the background threads run in milliseconds." },
    { false, "MinAgeMs", &Config::minAgeMs, "How long a released resource is kept before it is freed in milliseconds." },
    { false, "MaxReleasesPerTick", &Config::maxReleasesPerTick,
      "Resources freed per tick. Each free makes the GPU update its page tables, so they are spread out." },
    { false, "FreeBudgetUs", &Config::freeBudgetUs, "Stop freeing for the current tick after this many microseconds." },
    { false, "QuietMs", &Config::quietMs,
      "Background work waits until the game has not created an expensive resource for this many milliseconds." },
    { false, "MaxQueue", &Config::maxQueue, "With more resources than this waiting to be freed, the limits above are ignored." },

    { true, "PoolMinCostUs", &Config::poolMinCostUs,
      "Only resources that take the game at least this many microseconds to create get spares.\n"
      "This is also what counts as expensive for Gentle and QuietMs." },
    { false, "PoolSparesPerKey", &Config::poolSparesPerKey, "Spares kept ready per resource." },
    { false, "PoolTtlMs", &Config::poolTtlMs, "Unused spares are freed after this many milliseconds." },
    { false, "PoolPreCreatesPerTick", &Config::poolPreCreatesPerTick, "Spares built per tick." },
    { false, "PoolPreCreateBudgetUs", &Config::poolPreCreateBudgetUs, "Stop building spares for the current tick after this many microseconds." },
    { false, "PoolMaxSize", &Config::poolMaxSize, "Maximum number of spares, including those still being built." },

    { true, "StatsIntervalMs", &Config::statsIntervalMs, "How often a stats line is written to AnvilStutterFix.log in milliseconds." },
};

std::wstring Widen(const char* ascii) { return std::wstring(ascii, ascii + strlen(ascii)); }

bool WriteDefaultIni(const std::wstring& path)
{
    const Config def;
    std::string  text = "; AnvilStutterFix settings for the debug build. Delete this file to get the defaults back.\r\n"
                        "; The release build ignores it and always uses the defaults for now.\r\n"
                        "[AnvilStutterFix]\r\n";
    for (const Setting& s : kSettings)
    {
        if (s.newGroup) text += "\r\n";
        text += "; ";
        for (const char* c = s.help; *c; c++)
            if (*c == '\n') text += "\r\n; ";
            else text += *c;
        text += std::string("\r\n") + s.key + "=" + std::to_string(def.*s.field) + "\r\n";
    }

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool  ok = WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
    CloseHandle(f);
    return ok;
}

void LoadConfig()
{
    std::wstring ini = FileNextToDll(L"AnvilStutterFix.ini");
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (WriteDefaultIni(ini)) Log("Created AnvilStutterFix.ini with the default settings");
        else Log("Could not create AnvilStutterFix.ini; using the default settings");
    }
    for (const Setting& s : kSettings)
        g_cfg.*s.field = (int)GetPrivateProfileIntW(L"AnvilStutterFix", Widen(s.key).c_str(), g_cfg.*s.field, ini.c_str());
    if (g_cfg.tickMs < 1) g_cfg.tickMs = 1;
    if (g_cfg.maxReleasesPerTick < 1) g_cfg.maxReleasesPerTick = 1;
    if (g_cfg.poolPreCreatesPerTick < 1) g_cfg.poolPreCreatesPerTick = 1;

    std::string line;
    for (const Setting& s : kSettings) line += std::string(" ") + s.key + "=" + std::to_string(g_cfg.*s.field);
    Log("Settings:%s", line.c_str());
}

struct Toggle
{
    int                vk;
    std::atomic<bool>* flag;
    const char*        name;
    bool               wasDown;
};
Toggle g_toggles[] = {
    { VK_F12, &g_deferEnabled, "deferred release", false },
    { VK_F11, &g_poolEnabled, "spare pool", false },
    { VK_F10, &g_gentle, "gentle background mode", false },
    { VK_F9, &g_traceEvents, "event trace (AnvilStutterFix_events.csv)", false },
};

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

const char* const kMissNames[MissCount] = { "new", "state-differs", "clear-differs", "not-eligible", "still-pending", "no-copy", "pool-off" };

// Releases keyed without initial state and clear format, to tell those mismatches apart from new resources.
struct LooseRelease
{
    double lastReleaseMs;
    UINT   state, clearFormat;
    bool   hasClear;
};
PoolKey LooseKey(PoolKey k)
{
    k.state = 0; k.clearFormat = 0; k.hasClear = false;
    return k;
}
std::unordered_map<PoolKey, LooseRelease, PoolKeyHash> g_looseReleased;

struct Stats
{
    std::atomic<uint64_t> creates{0}, createUs{0}, createMaxUs{0};
    std::atomic<uint64_t> poolHits{0}, hitUs{0};
    std::atomic<uint64_t> deferred{0};
    std::atomic<uint64_t> syncReleases{0}, syncUs{0}, syncMaxUs{0};
    std::atomic<uint64_t> drained{0}, drainUs{0}, drainMaxUs{0};
    std::atomic<uint64_t> precreated{0}, precreateUs{0}, precreateFailed{0}, expired{0};
    std::atomic<uint64_t> maxQueue{0};
    std::atomic<uint64_t> missCount[MissCount]{}, missUs[MissCount]{};
    std::atomic<uint64_t> pausedTicks{0};   // ticks the background threads sat out
};
Stats g_stats;

} // namespace

void Debug_Init()
{
    LoadConfig();
    g_deferEnabled = g_cfg.deferReleases != 0;
    g_poolEnabled  = g_cfg.poolEnabled != 0;
    g_gentle       = g_cfg.gentle != 0;
    g_traceEvents  = g_cfg.traceEvents != 0;

    Log_OpenEvents(L"AnvilStutterFix_events.csv");
    LogEvent("type,time_ms,tid,resource,duration_us,dimension,width,height,depth_or_array,mips,format,flags,heap,heap_offset,lifetime_ms,queue_depth");
    for (const Toggle& t : g_toggles) Log("Hotkey Ctrl+Shift+F%d toggles %s", t.vk - VK_F1 + 1, t.name);
}

void Debug_PollHotkeys()
{
    for (Toggle& t : g_toggles)
    {
        bool down = KeyDown(t.vk) && KeyDown(VK_CONTROL) && KeyDown(VK_SHIFT);
        if (down && !t.wasDown)
        {
            bool on = !t.flag->load();
            t.flag->store(on);
            Log("Hotkey: %s %s", t.name, on ? "ON" : "OFF");
            MessageBeep(on ? MB_ICONASTERISK : MB_ICONEXCLAMATION);
        }
        t.wasDown = down;
    }
}

void Debug_LogStats(double intervalMs, size_t queueDepth, size_t ready, size_t pending, size_t tracked)
{
    auto take = [](std::atomic<uint64_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
    uint64_t c = take(g_stats.creates), cUs = take(g_stats.createUs), cMax = take(g_stats.createMaxUs);
    uint64_t h = take(g_stats.poolHits), hUs = take(g_stats.hitUs);
    uint64_t d = take(g_stats.deferred);
    uint64_t r = take(g_stats.drained), rUs = take(g_stats.drainUs), rMax = take(g_stats.drainMaxUs);
    uint64_t pc = take(g_stats.precreated), pUs = take(g_stats.precreateUs), pf = take(g_stats.precreateFailed), ex = take(g_stats.expired);
    uint64_t q = take(g_stats.maxQueue);
    uint64_t total = c + h;
    Log("[%.0fs] CreatePlacedResource %llu: pool hits %llu (%.0f%%, %.2f ms), real creates %llu (%.2f ms, max %.2f) "
        "| pre-created %llu (%.2f ms bg, %llu failed), expired %llu, ready %zu, pending %zu "
        "| deferred %llu, freed %llu (%.2f ms bg, max %.2f), queue %zu (peak %llu) | tracked %zu",
        intervalMs / 1000.0, total, h, total ? 100.0 * h / total : 0.0, hUs / 1000.0, c, cUs / 1000.0, cMax / 1000.0,
        pc, pUs / 1000.0, pf, ex, ready, pending, d, r, rUs / 1000.0, rMax / 1000.0, queueDepth, q, tracked);

    char line[512];
    int  n = 0;
    for (int i = 0; i < MissCount; i++)
    {
        uint64_t mc = take(g_stats.missCount[i]), mu = take(g_stats.missUs[i]);
        if (mc) n += snprintf(line + n, sizeof(line) - n, "%s %llu (%.2f ms)  ", kMissNames[i], mc, mu / 1000.0);
    }
    n += snprintf(line + n, sizeof(line) - n, "| background ticks paused for game creates: %llu", take(g_stats.pausedTicks));
    Log("       real-create reasons: %s", line);

    uint64_t s = take(g_stats.syncReleases), sUs = take(g_stats.syncUs), sMax = take(g_stats.syncMaxUs);
    Log("       defer=%s pool=%s gentle=%s trace=%s | sync releases %llu (%.2f ms, max %.2f)",
        g_deferEnabled ? "on" : "off", g_poolEnabled ? "on" : "off", g_gentle ? "on" : "off", g_traceEvents ? "on" : "off",
        s, sUs / 1000.0, sMax / 1000.0);
}

void Debug_Trace(char type, void* obj, double durUs, const CreateParams& p, double createdMs, size_t depth)
{
    if (!g_traceEvents) return;
    double now = NowMs();
    LogEvent("%c,%.3f,%lu,%p,%.1f,%d,%llu,%u,%u,%u,%d,0x%x,%p,%llu,%.1f,%zu", type, now, GetCurrentThreadId(), obj, durUs,
             (int)p.desc.Dimension, (unsigned long long)p.desc.Width, p.desc.Height, p.desc.DepthOrArraySize, p.desc.MipLevels,
             (int)p.desc.Format, (unsigned)p.desc.Flags, p.heap, (unsigned long long)p.offset, createdMs ? now - createdMs : 0.0, depth);
}

void Debug_OnRelease(const PoolKey& key, double now)
{
    g_looseReleased[LooseKey(key)] = { now, key.state, key.clearFormat, key.hasClear };
}

MissReason Debug_ClassifyMiss(const PoolKey& key, bool released, bool eligible, bool pending)
{
    if (released || eligible) return pending ? MissPending : eligible ? MissNoCopy : MissNotEligible;
    auto lr = g_looseReleased.find(LooseKey(key));
    if (lr == g_looseReleased.end() || NowMs() - lr->second.lastReleaseMs > 10000) return MissNew;
    if (lr->second.state != key.state) return MissStateDiffers;
    if (lr->second.hasClear != key.hasClear || lr->second.clearFormat != key.clearFormat) return MissClearDiffers;
    return MissNew;
}

void Debug_CountRealCreate(MissReason why, double us)
{
    g_stats.creates++;
    g_stats.createUs += (uint64_t)us;
    AtomicMax(g_stats.createMaxUs, (uint64_t)us);
    g_stats.missCount[why]++;
    g_stats.missUs[why] += (uint64_t)us;
}

void Debug_CountHit(double us)
{
    g_stats.poolHits++;
    g_stats.hitUs += (uint64_t)us;
}

void Debug_CountDeferred(size_t queueDepth)
{
    g_stats.deferred++;
    AtomicMax(g_stats.maxQueue, queueDepth);
}

void Debug_CountSyncRelease(double us)
{
    g_stats.syncReleases++;
    g_stats.syncUs += (uint64_t)us;
    AtomicMax(g_stats.syncMaxUs, (uint64_t)us);
}

void Debug_CountFree(double us)
{
    g_stats.drained++;
    g_stats.drainUs += (uint64_t)us;
    AtomicMax(g_stats.drainMaxUs, (uint64_t)us);
}

void Debug_CountSpareBuilt(double us)
{
    g_stats.precreated++;
    g_stats.precreateUs += (uint64_t)us;
}

void Debug_CountSpareFailed() { g_stats.precreateFailed++; }
void Debug_CountExpired(size_t n) { g_stats.expired += n; }
void Debug_CountPausedTick() { g_stats.pausedTicks++; }

#endif
