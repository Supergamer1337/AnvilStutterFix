#include "d3d12_hooks.h"
#include "debug.h"

#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "MinHook.h"

namespace {

using PFN_D3D12CreateDevice    = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFN_CreatePlacedResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                                             D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_AddRef  = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using PFN_Release = ULONG(STDMETHODCALLTYPE*)(IUnknown*);

constexpr int kSlotAddRef = 1, kSlotRelease = 2, kSlotCreatePlacedResource = 29;

PFN_D3D12CreateDevice    o_D3D12CreateDevice    = nullptr;
PFN_CreatePlacedResource o_CreatePlacedResource = nullptr;
std::atomic<bool>        g_deviceHooked{false};
std::atomic<int64_t>     g_lastGameCreateQpc{0};   // last expensive create by the game itself

bool GameIsCreating()
{
    if (!g_gentle) return false;
    int64_t last = g_lastGameCreateQpc.load(std::memory_order_relaxed);
    return last && QpcToUs(Qpc() - last) < g_cfg.quietMs * 1000.0;
}

PoolKey MakeKey(const CreateParams& p)
{
    PoolKey k{};
    k.device = p.device; k.heap = p.heap; k.offset = p.offset;
    k.dimension = (UINT)p.desc.Dimension; k.alignment = p.desc.Alignment; k.width = p.desc.Width; k.height = p.desc.Height;
    k.depth = p.desc.DepthOrArraySize; k.mips = p.desc.MipLevels; k.format = (UINT)p.desc.Format;
    k.sampleCount = p.desc.SampleDesc.Count; k.sampleQuality = p.desc.SampleDesc.Quality;
    k.layout = (UINT)p.desc.Layout; k.flags = (UINT)p.desc.Flags; k.state = (UINT)p.state; k.hasClear = p.hasClear;
    // The optimized clear value is left out. It is only a hint for fast clears, clearing to another value is still correct.
    if (p.hasClear) k.clearFormat = (UINT)p.clear.Format;
    return k;
}

// D3D12Core shares its Release implementation with other object types, so the resource vtable slot is patched
// instead of hooking the function.
struct VtblPatch
{
    void**      vtbl;
    PFN_AddRef  addRef;
    PFN_Release release;   // the original
};
constexpr int    kMaxVtbls = 16;
VtblPatch        g_vtbls[kMaxVtbls];
std::atomic<int> g_vtblCount{0};
std::mutex       g_vtblMutex;

const VtblPatch* FindVtbl(void** vtbl)
{
    int n = g_vtblCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_vtbls[i].vtbl == vtbl) return &g_vtbls[i];
    return nullptr;
}
const VtblPatch* VtblOf(IUnknown* obj)
{
    const VtblPatch* vp = FindVtbl(*reinterpret_cast<void***>(obj));
    return vp ? vp : &g_vtbls[0];
}

ULONG STDMETHODCALLTYPE hk_Release(IUnknown* self);

bool PatchResourceVtable(IUnknown* obj)
{
    void** vtbl = *reinterpret_cast<void***>(obj);
    if (FindVtbl(vtbl)) return true;

    std::lock_guard<std::mutex> lk(g_vtblMutex);
    if (FindVtbl(vtbl)) return true;
    int   n = g_vtblCount.load(std::memory_order_relaxed);
    DWORD old;
    if (n >= kMaxVtbls || !VirtualProtect(&vtbl[kSlotRelease], sizeof(void*), PAGE_READWRITE, &old))
    {
        static bool logged = false;   // guarded by g_vtblMutex
        if (!logged) Log("Could not patch ID3D12Resource vtable %p; resources using it are passed through", vtbl);
        logged = true;
        return false;
    }
    g_vtbls[n] = { vtbl, reinterpret_cast<PFN_AddRef>(vtbl[kSlotAddRef]), reinterpret_cast<PFN_Release>(vtbl[kSlotRelease]) };
    g_vtblCount.store(n + 1, std::memory_order_release);   // publish before the slot can call hk_Release
    vtbl[kSlotRelease] = reinterpret_cast<void*>(&hk_Release);
    VirtualProtect(&vtbl[kSlotRelease], sizeof(void*), old, &old);
    Log("Patched ID3D12Resource vtable %p (Release %p -> hook)", vtbl, g_vtbls[n].release);
    return true;
}

struct ResInfo
{
    double       createMs;
    CreateParams params;
};

SRWLOCK                            g_trackLock = SRWLOCK_INIT;
std::unordered_map<void*, ResInfo> g_tracked;

bool LookupTracked(void* obj, ResInfo* out)
{
    AcquireSRWLockShared(&g_trackLock);
    auto it = g_tracked.find(obj);
    bool found = it != g_tracked.end();
    if (found) *out = it->second;
    ReleaseSRWLockShared(&g_trackLock);
    return found;
}
void Track(void* obj, const ResInfo& info)
{
    AcquireSRWLockExclusive(&g_trackLock);
    g_tracked[obj] = info;
    ReleaseSRWLockExclusive(&g_trackLock);
}
void Untrack(void* obj)
{
    AcquireSRWLockExclusive(&g_trackLock);
    g_tracked.erase(obj);
    ReleaseSRWLockExclusive(&g_trackLock);
}
size_t TrackedCount()
{
    AcquireSRWLockShared(&g_trackLock);
    size_t n = g_tracked.size();
    ReleaseSRWLockShared(&g_trackLock);
    return n;
}

struct Deferred
{
    IUnknown* obj;
    double    queuedMs;
};
std::mutex           g_queueMutex;
std::deque<Deferred> g_queue;

void DeferFree(IUnknown* obj, double queuedMs)
{
    std::lock_guard<std::mutex> lk(g_queueMutex);
    g_queue.push_back({ obj, queuedMs });
}

struct KeyStats
{
    double lastReleaseMs = 0;   // 0 while the game holds the resource
    double costEwmaUs    = 0;
    int    recreates     = 0;
};
struct Ready
{
    IUnknown* obj;
    IID       riid;
    double    createdMs;
};

std::mutex                                                   g_poolMutex;
std::unordered_map<PoolKey, KeyStats, PoolKeyHash>           g_keyStats;
std::unordered_map<PoolKey, std::vector<Ready>, PoolKeyHash> g_ready;
std::unordered_map<PoolKey, int, PoolKeyHash>                g_pendingCount;
std::deque<CreateParams>                                     g_pending;   // each entry holds a reference on its heap
size_t                                                       g_readyCount = 0;

bool IsHot(const KeyStats& ks)
{
    return ks.recreates > 0 && ks.costEwmaUs >= g_cfg.poolMinCostUs;
}

// The heap is alive here because the caller is creating, serving or releasing a resource on it.
void EnsureSpareLocked(const PoolKey& key, const CreateParams& p, const KeyStats& ks)
{
    if (!g_poolEnabled || !IsHot(ks)) return;
    size_t have = 0;
    auto r = g_ready.find(key);
    if (r != g_ready.end()) have += r->second.size();
    auto pc = g_pendingCount.find(key);
    if (pc != g_pendingCount.end()) have += pc->second;
    if (have >= (size_t)g_cfg.poolSparesPerKey) return;
    if (g_readyCount + g_pending.size() >= (size_t)g_cfg.poolMaxSize) return;

    p.heap->AddRef();   // keeps the heap alive until the spare is built
    g_pending.push_back(p);
    g_pendingCount[key]++;
}

void SchedulePreCreate(const CreateParams& p)
{
    double  now = NowMs();
    PoolKey key = MakeKey(p);
    std::lock_guard<std::mutex> lk(g_poolMutex);
    KeyStats& ks = g_keyStats[key];
    ks.lastReleaseMs = now;
    Debug_OnRelease(key, now);
    EnsureSpareLocked(key, p, ks);
}

void OnRealCreate(const CreateParams& p, double us)
{
    PoolKey key = MakeKey(p);
    std::lock_guard<std::mutex> lk(g_poolMutex);
    KeyStats& ks = g_keyStats[key];
    ks.costEwmaUs = ks.costEwmaUs > 0 ? 0.7 * ks.costEwmaUs + 0.3 * us : us;
    EnsureSpareLocked(key, p, ks);
}

// We try to mimic a new object so the underlying game systems remain the same.
Ready TakeFromPool(const CreateParams& p, MissReason* why)
{
    PoolKey key = MakeKey(p);
    std::lock_guard<std::mutex> lk(g_poolMutex);

    auto      ksIt = g_keyStats.find(key);
    KeyStats* ks   = ksIt != g_keyStats.end() ? &ksIt->second : nullptr;
#if ASF_DEBUG
    bool released = ks && ks->lastReleaseMs > 0, eligible = ks && IsHot(*ks);
#endif
    if (ks && ks->lastReleaseMs > 0)
    {
        ks->recreates++;
        ks->lastReleaseMs = 0;
    }
    if (!g_poolEnabled) { *why = MissDisabled; return {}; }

    auto it = g_ready.find(key);
    if (it != g_ready.end() && !it->second.empty())
    {
        Ready r = it->second.back();
        it->second.pop_back();
        if (it->second.empty()) g_ready.erase(it);
        g_readyCount--;
        if (ks) EnsureSpareLocked(key, p, *ks);
        return r;
    }
#if ASF_DEBUG
    auto pc = g_pendingCount.find(key);
    *why = Debug_ClassifyMiss(key, released, eligible, pc != g_pendingCount.end() && pc->second > 0);
#endif
    return {};
}

ULONG STDMETHODCALLTYPE hk_Release(IUnknown* self)
{
    const VtblPatch* vp = VtblOf(self);

    ResInfo info;
    if (!LookupTracked(self, &info)) return vp->release(self);

    // A count of 2 after our own AddRef means the caller is dropping the last reference.
    ULONG n = vp->addRef(self);
    if (n != 2)
    {
        vp->release(self);
        return vp->release(self);
    }

    if (!g_deferEnabled)
    {
        // Schedule first, while the resource still keeps the heap alive for SchedulePreCreate.
        SchedulePreCreate(info.params);
        Untrack(self);
        vp->release(self);
        int64_t t0 = Debug_Now();
        vp->release(self);
        double us = Debug_UsSince(t0);
        Debug_CountSyncRelease(us);
        Debug_Trace('S', self, us, info.params, info.createMs, 0);
        return 0;
    }

    // Our reference now belongs to the free queue.
    vp->release(self);
    size_t depth;
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        g_queue.push_back({ self, NowMs() });
        depth = g_queue.size();
    }
    Debug_CountDeferred(depth);
    Debug_Trace('D', self, 0, info.params, info.createMs, depth);
    SchedulePreCreate(info.params);
    return 0;
}

void LogStats(double intervalMs)
{
    size_t depth, ready, pending;
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        depth = g_queue.size();
    }
    {
        std::lock_guard<std::mutex> lk(g_poolMutex);
        ready   = g_readyCount;
        pending = g_pending.size();
    }
    Debug_LogStats(intervalMs, depth, ready, pending, TrackedCount());
}

void DrainFrees(double now)
{
    int  budget = g_cfg.maxReleasesPerTick;
    bool backlog;
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        backlog = g_queue.size() > (size_t)g_cfg.maxQueue;
    }
    if (backlog && budget < 32) budget = 32;

    int64_t tickStart = Qpc();
    for (int i = 0; i < budget; i++)
    {
        if (!backlog && i > 0 && QpcToUs(Qpc() - tickStart) >= g_cfg.freeBudgetUs) return;
        Deferred d;
        size_t   depth;
        {
            std::lock_guard<std::mutex> lk(g_queueMutex);
            if (g_queue.empty()) return;
            if (!backlog && now - g_queue.front().queuedMs < g_cfg.minAgeMs) return;
            d = g_queue.front();
            g_queue.pop_front();
            depth = g_queue.size();
        }
        ResInfo info{};
        bool    traced = g_traceEvents && LookupTracked(d.obj, &info);
        Untrack(d.obj);   // before the object can be destroyed and its address reused
        int64_t t0 = Debug_Now();
        VtblOf(d.obj)->release(d.obj);
        double us = Debug_UsSince(t0);
        Debug_CountFree(us);
        if (traced) Debug_Trace('F', d.obj, us, info.params, info.createMs, depth);
    }
}

void RunPreCreates()
{
    int64_t tickStart = Qpc();
    for (int i = 0; i < g_cfg.poolPreCreatesPerTick; i++)
    {
        if (i > 0 && QpcToUs(Qpc() - tickStart) >= g_cfg.poolPreCreateBudgetUs) return;
        CreateParams p;
        {
            std::lock_guard<std::mutex> lk(g_poolMutex);
            if (g_pending.empty()) return;
            p = g_pending.front();
            g_pending.pop_front();
            auto pc = g_pendingCount.find(MakeKey(p));
            if (pc != g_pendingCount.end() && --pc->second <= 0) g_pendingCount.erase(pc);
        }
        void*   obj = nullptr;
        int64_t t0  = Debug_Now();
        HRESULT hr  = g_poolEnabled ? o_CreatePlacedResource(p.device, p.heap, p.offset, &p.desc, p.state,
                                                            p.hasClear ? &p.clear : nullptr, p.riid, &obj)
                                    : E_ABORT;
        double us = Debug_UsSince(t0);
        p.heap->Release();   // the new resource holds its own heap reference

        if (FAILED(hr) || !obj)
        {
            if (hr == E_ABORT) continue;
            static bool logged = false;   // only this thread builds spares
            if (!logged)
                Log("Pre-creating a spare failed (hr 0x%08lx, %llux%u format %d). Further failures are not logged.",
                    (unsigned long)hr, (unsigned long long)p.desc.Width, p.desc.Height, (int)p.desc.Format);
            logged = true;
            Debug_CountSpareFailed();
            continue;
        }
        IUnknown* res = static_cast<IUnknown*>(obj);
        if (!PatchResourceVtable(res))
        {
            res->Release();   // its release could not be deferred, so it is not handed out
            continue;
        }
        Track(res, { NowMs(), p });
        Debug_CountSpareBuilt(us);
        Debug_Trace('P', res, us, p, 0, 0);
        std::lock_guard<std::mutex> lk(g_poolMutex);
        g_ready[MakeKey(p)].push_back({ res, p.riid, NowMs() });
        g_readyCount++;
    }
}

void ExpirePool(double now)
{
    std::vector<IUnknown*> expired;
    {
        std::lock_guard<std::mutex> lk(g_poolMutex);
        for (auto it = g_ready.begin(); it != g_ready.end();)
        {
            auto& v = it->second;
            for (size_t i = 0; i < v.size();)
            {
                if (!g_poolEnabled || now - v[i].createdMs > g_cfg.poolTtlMs)
                {
                    expired.push_back(v[i].obj);
                    v[i] = v.back();
                    v.pop_back();
                    g_readyCount--;
                }
                else
                    i++;
            }
            it = v.empty() ? g_ready.erase(it) : std::next(it);
        }
    }
    for (IUnknown* obj : expired) DeferFree(obj, now);
    Debug_CountExpired(expired.size());
}

HANDLE MakeTickTimer()
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return timer ? timer : CreateWaitableTimerW(nullptr, FALSE, nullptr);
}
void WaitTick(HANDLE timer)
{
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)g_cfg.tickMs * 10000;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
}

DWORD WINAPI PoolThread(void*)
{
    HANDLE timer = MakeTickTimer();
    double lastStats = NowMs(), lastExpire = NowMs();
    for (;;)
    {
        WaitTick(timer);
        Debug_PollHotkeys();
        double now = NowMs();
        if (GameIsCreating()) Debug_CountPausedTick();
        else RunPreCreates();
        if (now - lastExpire >= 100) { ExpirePool(now); lastExpire = now; }
        if (ASF_DEBUG && now - lastStats >= g_cfg.statsIntervalMs) { LogStats(now - lastStats); lastStats = now; }
    }
}

DWORD WINAPI FreeThread(void*)
{
    HANDLE timer = MakeTickTimer();
    for (;;)
    {
        WaitTick(timer);
        bool backlog;
        {
            std::lock_guard<std::mutex> lk(g_queueMutex);
            backlog = g_queue.size() > (size_t)g_cfg.maxQueue;
        }
        if (GameIsCreating() && !backlog) { Debug_CountPausedTick(); continue; }
        DrainFrees(NowMs());
    }
}

HRESULT STDMETHODCALLTYPE hk_CreatePlacedResource(ID3D12Device* self, ID3D12Heap* heap, UINT64 offset,
                                                  const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state,
                                                  const D3D12_CLEAR_VALUE* clear, REFIID riid, void** ppv)
{
    if (!ppv || !desc || !heap)
        return o_CreatePlacedResource(self, heap, offset, desc, state, clear, riid, ppv);

    CreateParams p{ self, heap, offset, *desc, state, clear != nullptr, clear ? *clear : D3D12_CLEAR_VALUE{}, riid };

    int64_t    t0  = Qpc();
    MissReason why = MissNew;
    Ready      r   = TakeFromPool(p, &why);
    if (r.obj)
    {
        HRESULT hr = S_OK;
        if (IsEqualIID(riid, r.riid))
            *ppv = r.obj;
        else
        {
            hr = r.obj->QueryInterface(riid, ppv);
            if (FAILED(hr)) DeferFree(r.obj, NowMs());
            else VtblOf(r.obj)->release(r.obj);   // the game keeps the reference from QueryInterface
        }
        if (SUCCEEDED(hr))
        {
            double us = Debug_UsSince(t0);
            Debug_CountHit(us);
            if (*ppv != r.obj) Untrack(r.obj);
            Track(*ppv, { NowMs(), p });
            Debug_Trace('H', *ppv, us, p, 0, 0);
            return hr;
        }
    }

    HRESULT hr = o_CreatePlacedResource(self, heap, offset, desc, state, clear, riid, ppv);
    int64_t t1 = Qpc();
    double  us = QpcToUs(t1 - t0);
    // The game creates cheap buffers all the time, so only expensive creates count as a burst.
    if (us >= g_cfg.poolMinCostUs) g_lastGameCreateQpc.store(t1, std::memory_order_relaxed);
    Debug_CountRealCreate(why, us);

    if (SUCCEEDED(hr) && *ppv)
    {
        IUnknown* obj = static_cast<IUnknown*>(*ppv);
        if (PatchResourceVtable(obj)) Track(obj, { NowMs(), p });
        OnRealCreate(p, us);
        Debug_Trace('C', obj, us, p, 0, 0);
    }
    return hr;
}

void HookDevice(IUnknown* unk)
{
    if (g_deviceHooked.exchange(true)) return;

    ID3D12Device* dev = nullptr;
    if (FAILED(unk->QueryInterface(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) || !dev)
    {
        Log("QueryInterface(ID3D12Device) failed; not hooking");
        return;
    }
    void** vtbl   = *reinterpret_cast<void***>(dev);
    void*  target = vtbl[kSlotCreatePlacedResource];
    dev->Release();

    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&hk_CreatePlacedResource), reinterpret_cast<void**>(&o_CreatePlacedResource));
    if (st == MH_OK) st = MH_EnableHook(target);
    Log("Hooked ID3D12Device::CreatePlacedResource at %p: %s", target, MH_StatusToString(st));

    if (HANDLE t = CreateThread(nullptr, 0, PoolThread, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, FreeThread, nullptr, 0, nullptr)) CloseHandle(t);
}

HRESULT WINAPI hk_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL level, REFIID riid, void** ppDevice)
{
    HRESULT hr = o_D3D12CreateDevice(adapter, level, riid, ppDevice);
    Log("D3D12CreateDevice(level 0x%x) -> 0x%08lx", (unsigned)level, (unsigned long)hr);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) HookDevice(static_cast<IUnknown*>(*ppDevice));
    return hr;
}

} // namespace

void Hooks_Install()
{
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK) { Log("MH_Initialize failed: %s", MH_StatusToString(st)); return; }

    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryW(L"d3d12.dll");
    void* target = d3d12 ? reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    if (!target) { Log("D3D12CreateDevice not found"); return; }

    st = MH_CreateHook(target, reinterpret_cast<void*>(&hk_D3D12CreateDevice), reinterpret_cast<void**>(&o_D3D12CreateDevice));
    if (st == MH_OK) st = MH_EnableHook(target);
    Log("Hooked D3D12CreateDevice at %p: %s", target, MH_StatusToString(st));
}
