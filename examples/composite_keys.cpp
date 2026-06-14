// =============================================================================
//  examples/composite_keys.cpp
//
//  Composite keys with FULL-key and PARTIAL (prefix) lookups.
//
//  A composite key is an ordered tuple of sub-keys, compared lexicographically.
//  The same index then answers queries at any prefix length:
//
//      key = (department, level, name)
//
//        find ({dept, level, name})      -> one exact employee      (full key)
//        equal_range ({dept})            -> a whole department       (1-prefix)
//        equal_range ({dept, level})     -> one level in a dept      (2-prefix)
//        lower/upper_bound ({dept, lvl}) -> open-ended salary-band style ranges
//
//  mic's composite comparator only compares min(len(a), len(b)) components, so a
//  short tuple acts as a prefix that brackets every key sharing it — no separate
//  indices per query shape required.
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\composite_keys.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <cassert>
#include <print>
#include <string>
#include <tuple>

struct Emp {
    std::string department;
    int         level;     // seniority band, 1 (junior) .. 5 (principal)
    std::string name;
    int         salary;
};

// Ordered composite index over (department, level, name) — full key is unique,
// but we declare it non_unique so prefix ranges read naturally. A second hashed
// index gives O(1) lookup by name.
using Org = mic::multi_index_container<Emp,
    mic::indexed_by<
        mic::ordered_non_unique<"by_dept_lvl",
            mic::key<&Emp::department, &Emp::level, &Emp::name>>,
        mic::hashed_unique<"by_name", mic::key<&Emp::name>>
    >>;

int main() {
    Org org;
    org.insert({"Engineering", 3, "Ada",   180'000});
    org.insert({"Engineering", 3, "Bjarne",185'000});
    org.insert({"Engineering", 5, "Carol", 240'000});
    org.insert({"Engineering", 2, "Dan",   140'000});
    org.insert({"Sales",       2, "Eve",   120'000});
    org.insert({"Sales",       4, "Frank", 200'000});

    auto idx = org.get<"by_dept_lvl">();

    // ---- iteration is lexicographic by the whole composite key -------------
    // Engineering sorts before Sales; within a dept by level then name.
    std::println("All, sorted by (department, level, name):");
    for (const Emp& e : idx)
        std::println("  {:<12} L{} {:<8} ${}", e.department, e.level, e.name, e.salary);

    // ---- FULL key: exact lookup via the composite tuple --------------------
    {
        auto it = idx.find(std::tuple{std::string{"Engineering"}, 5, std::string{"Carol"}});
        assert(it != idx.end());
        assert(it->salary == 240'000);
        std::println("\nfind(Engineering, L5, Carol) -> ${}", it->salary);
    }

    // ---- 1-PREFIX: an entire department ------------------------------------
    // equal_range on a 1-element tuple brackets every key starting with it.
    {
        auto [lo, hi] = idx.equal_range(std::tuple{std::string{"Engineering"}});
        int n = 0, payroll = 0;
        for (auto it = lo; it != hi; ++it) { ++n; payroll += it->salary; }
        assert(n == 4);
        assert(idx.count(std::tuple{std::string{"Engineering"}}) == 4);
        assert(idx.count(std::tuple{std::string{"Sales"}}) == 2);
        std::println("\nEngineering: {} people, ${} total payroll", n, payroll);
    }

    // ---- 2-PREFIX: one level within a department ---------------------------
    {
        auto [lo, hi] = idx.equal_range(std::tuple{std::string{"Engineering"}, 3});
        std::println("Engineering L3:");
        int n = 0;
        for (auto it = lo; it != hi; ++it) { std::println("  {}", it->name); ++n; }
        assert(n == 2);   // Ada and Bjarne
    }

    // ---- PARTIAL open-ended range: a band of levels in a department --------
    // [L2, L4] in Engineering: lower_bound at the (dept, 2) prefix, upper_bound
    // at (dept, 4) — upper_bound on a prefix lands *after* the whole L4 group,
    // so the band is inclusive on both ends. (Carol at L5 is excluded.)
    {
        auto lo = idx.lower_bound(std::tuple{std::string{"Engineering"}, 2});
        auto hi = idx.upper_bound(std::tuple{std::string{"Engineering"}, 4});
        std::println("Engineering levels 2..4:");
        int n = 0;
        for (auto it = lo; it != hi; ++it) {
            std::println("  L{} {}", it->level, it->name);
            assert(it->level >= 2 && it->level <= 4);
            ++n;
        }
        assert(n == 3);   // Dan(L2), Ada(L3), Bjarne(L3)
    }

    // ---- the secondary hashed index still does plain by-name lookup --------
    {
        auto byName = org.get<"by_name">();
        auto it = byName.find("Frank");
        assert(it != byName.end());
        assert(it->department == "Sales" && it->level == 4);
        std::println("\nby_name.find(Frank) -> {} L{}", it->department, it->level);
    }

    std::println("\nAll composite-key assertions passed.");
    return 0;
}
