#pragma once

// Lightweight counters describing LocalVolume's materialization work.
//
// These are incremented on the hot paths that the staged-materialization
// refactor (docs/ai/2026-10-08-localvolume-staged-materialization-plan.md)
// moves around. Tests and benchmarks reset and read them to observe
// "enumerate once", "count without realizing", and similar properties without
// depending on timing. They are always compiled in (plain atomics, negligible
// cost) so a stock build can be measured.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Executor
{

struct LocalVolumeStats
{
    // Number of fs::directory_iterator scans that actually ran.
    std::atomic<uint64_t> directoryIterations { 0 };
    // Entries visited by those scans (after the hidden-name filter).
    std::atomic<uint64_t> entriesSeen { 0 };
    // Item objects constructed by the item factory.
    std::atomic<uint64_t> itemsConstructed { 0 };
    // Per-entry factory probes (the format gauntlet in
    // ItemFactory::createRepForFile, run from FileItem::ensureMaterialized).
    std::atomic<uint64_t> factoryProbes { 0 };
    // Count-stage directory scans (readdir tally; no items constructed).
    std::atomic<uint64_t> countScans { 0 };
    // ".bin" files probed to derive a Mac name (NameResolve only).
    std::atomic<uint64_t> nameProbes { 0 };
};

inline LocalVolumeStats& localVolumeStats()
{
    static LocalVolumeStats stats;
    return stats;
}

// The budget for the realization cache's LRU queue, in Item::cacheWeight()
// units (a realized item is 1; a listed directory is its descriptor count).
// Exposed so tests can force eviction without building half a million items.
inline size_t& localVolumeCacheBudget()
{
    static size_t budget = 500000;
    return budget;
}

inline void resetLocalVolumeStats()
{
    auto& s = localVolumeStats();
    s.directoryIterations = 0;
    s.entriesSeen = 0;
    s.itemsConstructed = 0;
    s.factoryProbes = 0;
    s.countScans = 0;
    s.nameProbes = 0;
}

} // namespace Executor
