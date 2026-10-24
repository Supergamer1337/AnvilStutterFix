#pragma once

#include "d3d12_hooks.h"

enum MissReason { MissNew, MissStateDiffers, MissClearDiffers, MissNotEligible, MissPending, MissNoCopy, MissDisabled, MissCount };

#if ASF_DEBUG
extern std::atomic<bool> g_deferEnabled, g_poolEnabled, g_gentle, g_traceEvents;

void Debug_Init();
void Debug_PollHotkeys();
void Debug_LogStats(double intervalMs, size_t queueDepth, size_t ready, size_t pending, size_t tracked);

inline int64_t Debug_Now() { return Qpc(); }
inline double  Debug_UsSince(int64_t t0) { return QpcToUs(Qpc() - t0); }

// createdMs is 0 for creation events, which then get a lifetime of 0.
void Debug_Trace(char type, void* obj, double durUs, const CreateParams& p, double createdMs, size_t depth);

// OnRelease and ClassifyMiss run under the pool lock.
void       Debug_OnRelease(const PoolKey& key, double now);
MissReason Debug_ClassifyMiss(const PoolKey& key, bool released, bool eligible, bool pending);

void Debug_CountRealCreate(MissReason why, double us);
void Debug_CountHit(double us);
void Debug_CountDeferred(size_t queueDepth);
void Debug_CountSyncRelease(double us);
void Debug_CountFree(double us);
void Debug_CountSpareBuilt(double us);
void Debug_CountSpareFailed();
void Debug_CountExpired(size_t n);
void Debug_CountPausedTick();
#else
constexpr bool g_deferEnabled = true, g_poolEnabled = true, g_gentle = true, g_traceEvents = false;

inline void    Debug_Init() {}
inline void    Debug_PollHotkeys() {}
inline void    Debug_LogStats(double, size_t, size_t, size_t, size_t) {}
inline int64_t Debug_Now() { return 0; }
inline double  Debug_UsSince(int64_t) { return 0; }
inline void    Debug_Trace(char, void*, double, const CreateParams&, double, size_t) {}
inline void    Debug_OnRelease(const PoolKey&, double) {}
inline void    Debug_CountRealCreate(MissReason, double) {}
inline void    Debug_CountHit(double) {}
inline void    Debug_CountDeferred(size_t) {}
inline void    Debug_CountSyncRelease(double) {}
inline void    Debug_CountFree(double) {}
inline void    Debug_CountSpareBuilt(double) {}
inline void    Debug_CountSpareFailed() {}
inline void    Debug_CountExpired(size_t) {}
inline void    Debug_CountPausedTick() {}
#endif
