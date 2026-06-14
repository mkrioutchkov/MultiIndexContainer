// =============================================================================
//  Playground for mic::multi_index_container
//
//  Open Playground.sln in Visual Studio, make sure the config is "Debug | x64"
//  (the toolbar dropdowns), and press F5 / the green Run button.
//
//  A guided tour of one Book library viewed through several indices, then the
//  newer toys: open-ended range scans, try_modify / modify_key, relational
//  equi_join / group_by, std::format introspection, lifecycle observers and a
//  fixed-capacity static table. Scroll to "YOUR TURN" and start hacking — the
//  library is header-only in ..\include\mic.
// =============================================================================

#include <mic/multi_index.hpp>

#include <print>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <ranges>
#include <vector>
#include <memory>

// -----------------------------------------------------------------------------
//  Domain: a tiny library of books (made formattable so "{:full}" can print it).
// -----------------------------------------------------------------------------
struct Book {
    int         id;
    std::string title;
    std::string author;
    int         year;
};
template <> struct std::formatter<Book> : std::formatter<std::string> {
    auto format(const Book& b, std::format_context& ctx) const {
        return std::formatter<std::string>::format(
            std::format("#{} {} ({}, {})", b.id, b.title, b.author, b.year), ctx);
    }
};

//   by_id          ordered, unique         primary key
//   by_author      ordered, duplicates ok  many books per author
//   by_title       hashed, unique          O(1) exact-title lookup
//   by_year        ranked, duplicates ok   order statistics (nth / rank)
//   by_author_year ordered composite       (author, year) prefix + range queries
using Library = mic::multi_index<Book,
    mic::indexed_by<
        mic::ordered_unique     <"by_id",           mic::key<&Book::id>>,
        mic::ordered_non_unique <"by_author",       mic::key<&Book::author>>,
        mic::hashed_unique      <"by_title",        mic::key<&Book::title>>,
        mic::ranked_non_unique  <"by_year",         mic::key<&Book::year>>,
        mic::ordered_non_unique <"by_author_year",  mic::key<&Book::author, &Book::year>>
    >>;

static void show(const Book& b) {
    std::println("    #{:<2} {:<32} {}  {}", b.id, b.title, b.year, b.author);
}
static void heading(std::string_view s) { std::println("\n=== {} ===", s); }

int main() {
    Library lib;

    // ---- insert / emplace -------------------------------------------------
    lib.insert(Book{1, "The C++ Programming Language", "Stroustrup", 2013});
    lib.insert(Book{2, "Effective Modern C++",         "Meyers",     2014});
    lib.insert(Book{3, "A Tour of C++",                "Stroustrup", 2022});
    lib.emplace(4, "Effective C++",                    "Meyers",     2005);
    lib.emplace(5, "C++ Concurrency in Action",        "Williams",   2019);
    std::println("Loaded {} books.  {}", lib.size(), std::format("{}", lib));

    // ---- iterate through different indices --------------------------------
    heading("by author (ordered, duplicates kept)");
    for (const Book& b : lib.get<"by_author">()) show(b);

    // ---- lookups ----------------------------------------------------------
    heading("lookups");
    std::println("  find by title (hashed):  \"A Tour of C++\" -> {}",
                 lib.get<"by_title">().find("A Tour of C++")->author);
    std::println("  count by author:         Meyers wrote {}", lib.get<"by_author">().count("Meyers"));

    // ---- ranked index: order statistics -----------------------------------
    heading("ranked by year (nth / rank)");
    auto byYear = lib.get<"by_year">();
    std::println("  oldest (nth 0):          {} ({})", byYear.nth(0)->title, byYear.nth(0)->year);
    std::println("  rank of 'A Tour of C++': {}", byYear.rank(byYear.find(2022)));

    // ---- ranges adaptors over an index ------------------------------------
    heading("std::ranges over indices");
    std::print("  three newest years:     ");
    for (const Book& b : lib.get<"by_year">() | std::views::reverse | std::views::take(3))
        std::print(" {}", b.year);
    std::println();

    // ---- composite key: prefix + open-ended range -------------------------
    heading("composite key: prefix & range()");
    auto [m0, m1] = lib.get<"by_author_year">().equal_range(mic::prefix(std::string("Meyers")));
    std::println("  all Meyers books (prefix on author):");
    for (auto it = m0; it != m1; ++it) show(*it);
    std::println("  Stroustrup books from 2015 onward (range with key_ge):");
    auto recent = lib.get<"by_author_year">().range(
        mic::key_ge(std::tuple{std::string("Stroustrup"), 2015}),
        mic::key_le(mic::prefix(std::string("Stroustrup"))));
    for (const Book& b : recent) show(b);

    // ---- modify / try_modify / modify_key ---------------------------------
    heading("modify / try_modify / modify_key");
    {
        auto it = lib.get<"by_id">().find(4);
        lib.get<"by_id">().modify(it, [](Book& b) { b.year = 2001; });   // re-sorts by_year
        std::println("  modify year -> oldest is now {}", lib.get<"by_year">().nth(0)->title);

        // try_modify names the index that blocks a clashing key change
        auto r = lib.get<"by_id">().try_modify(it, [](Book& b) { b.title = "A Tour of C++"; });
        std::println("  try_modify to a taken title -> {}",
                     r ? "ok" : std::format("rejected by '{}'", r.error().index_tag));

        // modify_key edits just the key, through the index that owns it
        lib.get<"by_year">().modify_key(lib.project<"by_year">(it), [](int& y) { y += 1; });
        std::println("  modify_key bumped the year to {}", it->year);
    }

    // ---- projection -------------------------------------------------------
    heading("projection");
    {
        auto t = lib.get<"by_title">().find("A Tour of C++");
        auto a = lib.project<"by_author">(t);
        std::println("  found via title, viewed in author index: {} by {}", a->title, a->author);
    }

    // ---- relational queries: equi_join + group_by -------------------------
#if MIC_HAS_GENERATOR
    heading("relational queries (equi_join / group_by)");
    struct Author { std::string name; std::string country; };
    using Authors = mic::multi_index<Author, mic::indexed_by<
        mic::ordered_non_unique<"by_name", mic::key<&Author::name>>>>;
    Authors authors;
    authors.insert({"Stroustrup", "DK"});
    authors.insert({"Meyers",     "US"});
    // INNER JOIN books to author bios on author name:
    for (auto&& [book, author] : mic::queries::equi_join(lib.get<"by_author">(), authors.get<"by_name">()))
        std::println("  {:<28} <- {} ({})", book.title, author.name, author.country);
    // GROUP BY author:
    std::println("  books per author:");
    for (auto&& [name, group] : mic::queries::group_by(lib.get<"by_author">()))
        std::println("    {:<12} {}", name, std::ranges::distance(group));
#endif

    // ---- std::format introspection ----------------------------------------
    heading("std::format introspection");
    std::println("{:stats}", lib);
    std::println("{:audit}", lib);
    std::println("  by_title load_factor = {:.2f}", lib.stats().index<"by_title">().load_factor);
    std::println("  books in year order:\n{:index=by_year}", lib);

    // ---- lifecycle observers ----------------------------------------------
    heading("observers (mic::observed + RAII token)");
    {
        using Watched = mic::observed<Book, mic::indexed_by<
            mic::ordered_unique<"by_id", mic::key<&Book::id>>>>;
        Watched w;
        auto token = w.subscribe({
            .on_insert = [](const Book& b) { std::println("    + added '{}'", b.title); },
            .on_erase  = [](const Book& b) { std::println("    - removed '{}'", b.title); },
        });
        w.insert(Book{1, "Observed", "You", 2024});
        w.get<"by_id">().erase(w.get<"by_id">().find(1));
    } // token dropped -> observation stops

    // ---- fixed-capacity inline table (no heap) ----------------------------
    heading("static_multi_index (inline arena, no heap)");
    {
        using Fixed = mic::static_multi_index<Book, /*N=*/4,
            mic::indexed_by<mic::ordered_unique<"by_id", mic::key<&Book::id>>>>;
        Fixed fixed;
        int placed = 0, full = 0;
        for (int i = 0; i < 100; ++i) {
            auto r = fixed.try_emplace(i, std::format("b{}", i), "anon", 2000);
            if (r) ++placed; else ++full;
        }
        std::println("  capacity>={}, placed {} with zero heap, {} capacity_exceeded",
                     Fixed::capacity, placed, full);
    }

    // =======================================================================
    //  YOUR TURN — mess about below. Ideas:
    //    * Add a 6th index and query it.
    //    * lib.get<"by_id">().replace(it, newBook);  or try_replace
    //    * Build the container from a vector with std::from_range.
    //    * equi_join two of your own containers; group_by something.
    //    * Print "{:full}" of lib (Book is formattable above).
    // =======================================================================

    std::println("\nDone.");
    return 0;
}
