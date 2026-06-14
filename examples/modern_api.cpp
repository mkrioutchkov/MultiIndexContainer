// =============================================================================
//  examples/modern_api.cpp
//
//  A short tour of the C++23 conveniences mic adds on top of the Boost feature
//  set, all on one container:
//
//    * std::from_range construction            (build straight from any range)
//    * transparent string_view lookup          (find without materialising a key)
//    * ranked order statistics: nth() / rank()  (O(log n) "top-K" / "what place")
//    * std::expected try_insert                 (typed failure, no exceptions)
//    * std::format / std::println support       ("{}" prints a summary)
//    * std::pmr arena allocation                (one resource feeds the whole box)
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\modern_api.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <array>
#include <cassert>
#include <memory_resource>
#include <print>
#include <string>
#include <string_view>
#include <vector>

struct Track {
    std::string title;
    std::string artist;
    long        plays;
};

// One index spec, reused for the default-allocator and the pmr container below.
using TrackIndices = mic::indexed_by<
    mic::hashed_unique     <"by_title", mic::key<&Track::title>>,
    mic::ranked_non_unique <"by_plays", mic::key<&Track::plays>>,
    mic::sequenced         <"by_added">
>;
using Tracks    = mic::multi_index_container<Track, TrackIndices>;
using PmrTracks = mic::pmr::multi_index_container<Track, TrackIndices>;

int main() {
    // ---- 1. std::from_range: build directly from a vector ------------------
    std::vector<Track> seed{
        {"Clair de Lune",  "Debussy",     1200},
        {"Gymnopedie No.1","Satie",         900},
        {"Air on G String","Bach",         1500},
        {"Nocturne Op.9",  "Chopin",       1100},
        {"The Swan",       "Saint-Saens",   400},
    };
    Tracks lib(std::from_range, seed);
    assert(lib.size() == 5);

    // ---- 5. std::format / std::println -------------------------------------
    std::println("Loaded {}", lib);                       // "multi_index(size=5)"
    assert(std::format("{}", lib) == "multi_index(size=5)");

    // ---- 2. transparent lookup: find by string_view, no std::string built --
    // string keys hash through string_view, so this allocates nothing for the
    // probe key (the hash agrees with std::hash<std::string> by the standard).
    {
        std::string_view probe = "Air on G String";
        auto byTitle = lib.get<"by_title">();
        auto it = byTitle.find(probe);                    // no temporary std::string
        assert(it != byTitle.end());
        assert(it->artist == "Bach");
        std::println("find(\"{}\") -> {}", probe, it->artist);
    }

    // ---- 3. ranked order statistics: nth() and rank(), both O(log n) -------
    // by_plays is a size-augmented tree, sorted ascending by play count.
    {
        auto byPlays = lib.get<"by_plays">();

        // Least- and most-played without scanning: nth(0) and nth(size-1).
        assert(byPlays.nth(0)->title == "The Swan");            // 400, fewest
        auto top = byPlays.nth(lib.size() - 1);
        assert(top->title == "Air on G String");               // 1500, most
        std::println("Most played: {} ({} plays)", top->title, top->plays);

        // "What place is Clair de Lune in?" rank() answers in O(log n).
        auto cdl = byPlays.find(1200);
        assert(cdl != byPlays.end());
        std::size_t place = byPlays.rank(cdl);                 // 0-based from the bottom
        assert(place == 3);                                    // 400,900,1100,[1200],1500
        std::println("Clair de Lune is #{} from the top",
                     lib.size() - place);                      // -> #2 most played

        // Top-3 chart: walk the last three in descending order.
        std::println("Top 3:");
        for (std::size_t i = 0; i < 3; ++i) {
            auto t = byPlays.nth(lib.size() - 1 - i);
            std::println("  {}. {:<16} {} plays", i + 1, t->title, t->plays);
        }
    }

    // ---- 4. std::expected try_insert: typed failure, no throw --------------
    {
        auto r = lib.try_insert(Track{"Clair de Lune", "Imposter", 1});
        assert(!r);                                            // title already present
        assert(r.error().index_tag == "by_title");
        std::println("try_insert duplicate -> rejected by '{}'", r.error().index_tag);
        assert(lib.size() == 5);                              // container untouched
    }

    // ---- 6. std::pmr: one memory_resource backs the whole container --------
    // Every allocation (element nodes + the hashed bucket array) is served from
    // this stack buffer; nothing reaches the global heap.
    {
        std::array<std::byte, 16 * 1024> buffer;
        std::pmr::monotonic_buffer_resource arena{buffer.data(), buffer.size()};
        PmrTracks fast{&arena};
        for (const Track& t : seed) fast.insert(t);
        assert(fast.size() == 5);
        assert(fast.get<"by_title">().find("Nocturne Op.9")->artist == "Chopin");
        std::println("pmr container built in a {}-byte arena: {}", buffer.size(), fast);
    }

    std::println("\nAll modern-API assertions passed.");
    return 0;
}
