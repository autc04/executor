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
    // Per-entry factory probes (the format gauntlet in createItemForDirEntry).
    std::atomic<uint64_t> factoryProbes { 0 };
};

inline LocalVolumeStats& localVolumeStats()
{
    static LocalVolumeStats stats;
    return stats;
}

inline void resetLocalVolumeStats()
{
    auto& s = localVolumeStats();
    s.directoryIterations = 0;
    s.entriesSeen = 0;
    s.itemsConstructed = 0;
    s.factoryProbes = 0;
}

} // namespace Executor
