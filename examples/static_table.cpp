// =============================================================================
//  examples/static_table.cpp
//
//  Zero-heap, fixed-capacity storage — for embedded / real-time code that can't
//  (or won't) call the global allocator.
//
//    * mic::static_multi_index<T, N, IndexedBy>
//        Every node and per-index structure lives in an arena embedded in the
//        object. NO heap allocation. Holds at least N elements; try_insert /
//        try_emplace return insert_error::capacity_exceeded once the arena fills
//        (plain insert/emplace would throw, like std:: containers on OOM).
//
//    * mic::pmr::multi_index<T, IndexedBy> over a stack std::array
//        The same indices, but pooled in a buffer you own. With a null upstream
//        the resource never falls back to the heap.
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\static_table.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <memory_resource>
#include <print>
#include <string>

struct Reading {
    int         sensor_id;
    std::string location;
    float       celsius;
};

using Indices = mic::indexed_by<
    mic::ordered_unique    <"by_id",  mic::key<&Reading::sensor_id>>,
    mic::hashed_non_unique <"by_loc", mic::key<&Reading::location>>
>;

int main() {
    // ---- 1. static_multi_index: inline arena, no heap ----------------------
    using FixedTable = mic::static_multi_index<Reading, /*N=*/8, Indices>;
    std::println("FixedTable holds at least {} readings, entirely inline.", FixedTable::capacity);

    FixedTable t;
    int stored = 0, rejected = 0;
    for (int i = 0; i < 10'000; ++i) {
        auto r = t.try_emplace(i, std::format("rack-{}", i % 4), 20.0f + static_cast<float>(i % 5));
        if (r) ++stored;
        else if (r.error().why == mic::insert_error<Reading>::reason::capacity_exceeded) ++rejected;
    }
    std::println("stored {} readings with ZERO heap allocation; {} rejected as capacity_exceeded.",
                 stored, rejected);
    assert(stored >= 8);        // at least N fit
    assert(rejected > 0);       // overflow was reported, never crashed

    // lookups work exactly like any other container
    assert(t.get<"by_id">().find(0) != t.get<"by_id">().end());
    std::println("rack-0 currently holds {} readings.", t.get<"by_loc">().count("rack-0"));

    // ---- 2. pmr over a stack buffer: pool the whole container --------------
    // A null upstream means a full buffer fails instead of silently heap-allocating.
    alignas(std::max_align_t) std::array<std::byte, 16 * 1024> buffer;
    std::pmr::monotonic_buffer_resource arena{
        buffer.data(), buffer.size(), std::pmr::null_memory_resource()};

    mic::pmr::multi_index<Reading, Indices> pooled{&arena};
    pooled.insert({10, "roof",  15.2f});
    pooled.insert({11, "lab-A", 22.9f});
    pooled.insert({12, "lab-A", 25.0f});
    assert(pooled.size() == 3);
    assert(pooled.get<"by_loc">().count("lab-A") == 2);

    std::println("\npmr container of {} readings, served entirely from a {}-byte stack buffer:",
                 pooled.size(), buffer.size());
    for (const Reading& r : pooled.get<"by_id">())
        std::println("  #{:<2} {:<6} {:.1f} C", r.sensor_id, r.location, r.celsius);

    std::println("\nAll static-table assertions passed.");
    return 0;
}
