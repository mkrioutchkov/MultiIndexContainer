// =============================================================================
//  Playground for mic::multi_index_container
//
//  Open Playground.sln in Visual Studio, make sure the config is "Debug | x64"
//  (the toolbar dropdowns), and press F5 / the green Run button.
//
//  This is a guided tour: one container of Books, viewed through five different
//  indices, exercising lookups, ranges, modify/replace, projection, node
//  handles, std::expected and composite keys. Scroll to "YOUR TURN" at the
//  bottom and start hacking — the library is header-only in ..\include\mic.
// =============================================================================

#include <mic/multi_index.hpp>

#include <print>
#include <string>
#include <string_view>
#include <tuple>
#include <ranges>
#include <memory>

// -----------------------------------------------------------------------------
//  Domain: a tiny library of books.
// -----------------------------------------------------------------------------
struct Book {
    int         id;
    std::string title;
    std::string author;
    int         year;
};

// Five views over the same Book objects (one physical copy per book, threaded
// into every index). Note each index just names a different key:
//
//   by_id          ordered, unique         primary key
//   by_author      ordered, duplicates ok  many books per author
//   by_title       hashed, unique          O(1) exact-title lookup
//   by_year        ranked, duplicates ok   order statistics (nth / rank)
//   by_author_year ordered composite       (author, year) prefix queries
using Library = mic::multi_index_container<Book,
    mic::indexed_by<
        mic::ordered_unique     <"by_id",          mic::key<&Book::id>>,
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
    lib.emplace(4, "Effective C++",                    "Meyers",     2005);  // emplace = in-place
    lib.emplace(5, "C++ Concurrency in Action",        "Williams",   2019);

    std::println("Loaded {} books.  {}", lib.size(), std::format("{}", lib));

    // ---- iterate through different indices --------------------------------
    heading("by id (ordered, unique)");
    for (const Book& b : lib.get<"by_id">()) show(b);

    heading("by author (ordered, duplicates kept)");
    for (const Book& b : lib.get<"by_author">()) show(b);

    // ---- lookups ----------------------------------------------------------
    heading("lookups");
    std::println("  find by title (hashed):  \"A Tour of C++\" -> {}",
                 lib.get<"by_title">().find("A Tour of C++")->author);
    std::println("  count by author:         Meyers wrote {}", lib.get<"by_author">().count("Meyers"));
    std::println("  contains id 5?           {}", lib.get<"by_id">().contains(5));

    std::println("  equal_range by author:   Stroustrup ->");
    auto [lo, hi] = lib.get<"by_author">().equal_range("Stroustrup");
    for (auto it = lo; it != hi; ++it) show(*it);

    // ---- ranked index: order statistics -----------------------------------
    heading("ranked by year (nth / rank)");
    auto byYear = lib.get<"by_year">();
    std::println("  oldest (nth 0):          {} ({})", byYear.nth(0)->title, byYear.nth(0)->year);
    std::println("  newest (nth size-1):     {} ({})",
                 byYear.nth(lib.size() - 1)->title, byYear.nth(lib.size() - 1)->year);
    std::println("  rank of 'A Tour of C++': {}", byYear.rank(byYear.find(2022)));

    // ---- ranges adaptors over an index ------------------------------------
    heading("std::ranges over indices");
    std::print("  three newest:           ");
    for (const Book& b : lib.get<"by_year">() | std::views::reverse | std::views::take(3))
        std::print(" {}", b.year);
    std::println();

    std::print("  Stroustrup titles:      ");
    for (const Book& b : lib.get<"by_id">()
            | std::views::filter([](const Book& b) { return b.author == "Stroustrup"; }))
        std::print(" \"{}\"", b.title);
    std::println();

    // ---- composite key: (author, year) prefix query -----------------------
    heading("composite key prefix query");
    std::println("  all Meyers books, oldest first (prefix on author):");
    auto [m0, m1] = lib.get<"by_author_year">().equal_range(std::tuple{std::string("Meyers")});
    for (auto it = m0; it != m1; ++it) show(*it);

    // ---- modify (repositions across every index) --------------------------
    heading("modify");
    {
        auto it = lib.get<"by_id">().find(4);
        std::println("  before: {} ({})", it->title, it->year);
        lib.get<"by_id">().modify(it, [](Book& b) { b.year = 2001; });  // re-sorts by_year
        std::println("  after:  {} ({})  -> oldest is now {}",
                     it->title, it->year, lib.get<"by_year">().nth(0)->title);
    }

    // ---- projection: jump between indices in O(1) -------------------------
    heading("projection");
    {
        auto t = lib.get<"by_title">().find("A Tour of C++");   // hashed iterator
        auto a = lib.project<"by_author">(t);                   // same book, author index
        std::println("  found via title, viewed in author index: {} by {}", a->title, a->author);
    }

    // ---- std::expected fallible insert ------------------------------------
    heading("try_insert (std::expected)");
    {
        auto r = lib.try_insert(Book{1, "Imposter", "Nobody", 2000});  // id 1 already exists
        if (!r)
            std::println("  rejected by index '{}' (duplicate key)", r.error().index_tag);
        else
            std::println("  inserted");
    }

    // ---- node handle: re-key without a copy -------------------------------
    heading("node handle (extract / re-key / re-insert)");
    {
        auto nh = lib.extract(lib.get<"by_id">().find(5));      // detach from all indices
        std::println("  extracted '{}'; size now {}", nh.value().title, lib.size());
        nh.value().id = 50;                                     // safe: detached, re-key freely
        auto res = lib.insert(std::move(nh));                   // re-thread under new id
        std::println("  re-inserted under id 50: ok={}, size {}", res.inserted, lib.size());
    }

    // ---- erase ------------------------------------------------------------
    heading("erase");
    std::println("  erase_key by_id(2) removed {} book(s); {} remain",
                 lib.get<"by_id">().erase_key(2), lib.size());

    // =======================================================================
    //  YOUR TURN — mess about below. Ideas:
    //
    //    * Add a 6th index and query it.
    //    * lib.get<"by_id">().replace(it, newBook);
    //    * Build the container from a vector with std::from_range.
    //    * Iterate by_author_year and watch books sort by (author, then year).
    //    * Swap two Library instances, or copy one and compare.
    // =======================================================================

    // ---- bonus: key-mutation safety with shared_ptr elements --------------
    //  Storing shared_ptr<Book> lets `it->id = x` compile and corrupt the index.
    //  const_element_container hands out a *const* pointee instead, so keys can
    //  only change via modify(). Uncomment the marked line to see it refuse.
    heading("bonus: const_element_container (pointer-element safety)");
    {
        using SafeLib = mic::const_element_container<std::shared_ptr<Book>,
            mic::indexed_by<mic::ordered_unique<"by_id", mic::key<&Book::id>>>>;
        SafeLib s;
        s.insert(std::make_shared<Book>(1, "Safe", "You", 2024));
        const Book& b = *s.get<"by_id">().find(1);   // iterator yields const Book&
        std::println("  read-only view: {} ({})", b.title, b.year);
        // b.id = 99;   // <-- uncomment: compile error C3490 (key is immutable)
        s.get<"by_id">().modify(s.get<"by_id">().find(1), [](auto& p) { p->title = "Still Safe"; });
        std::println("  changed via modify(): {}", s.get<"by_id">().find(1)->title);
    }

    std::println("\nDone.");
    return 0;
}
