// =============================================================================
//  Playground for mic::multi_index_container
//
//  Open Playground.sln in Visual Studio, make sure the config is "Debug | x64"
//  (the toolbar dropdowns), and press F5 / the green ▶ Run button.
//
//  The library is header-only and lives in ..\include\mic\multi_index.hpp —
//  this project already adds that folder to its include path, so you can just
//  #include <mic/multi_index.hpp> and start hacking.
// =============================================================================

#include <mic/multi_index.hpp>

#include <print>
#include <string>

// A little domain to play with: a tiny library of books.
struct Book {
    int         id;
    std::string title;
    std::string author;
    int         year;
};

// One container, three different views over the same Book objects:
//   * by_id     — ordered & unique (primary key)
//   * by_author — ordered, duplicates allowed (many books per author)
//   * by_title  — hashed & unique (fast exact-title lookup)
using Library = mic::multi_index_container<Book,
    mic::indexed_by<
        mic::ordered_unique     <"by_id",     mic::key<&Book::id>>,
        mic::ordered_non_unique <"by_author", mic::key<&Book::author>>,
        mic::hashed_unique      <"by_title",  mic::key<&Book::title>>
    >>;

int main() {
    Library lib;
    lib.insert({1, "The C++ Programming Language", "Stroustrup", 2013});
    lib.insert({2, "Effective Modern C++",         "Meyers",     2014});
    lib.insert({3, "A Tour of C++",                "Stroustrup", 2022});

    std::println("Hello from mic::multi_index_container!  {} books in the library.\n", lib.size());

    // Iterate in id order.
    std::println("All books, by id:");
    for (const Book& b : lib.get<"by_id">())
        std::println("  #{}  {} ({}) - {}", b.id, b.title, b.year, b.author);

    // O(1) exact lookup through the hashed title index.
    std::println("\nWho wrote \"A Tour of C++\"?  {}",
                 lib.get<"by_title">().find("A Tour of C++")->author);

    // Range query through the ordered author index.
    std::println("\nStroustrup wrote {} of these:", lib.get<"by_author">().count("Stroustrup"));
    auto [lo, hi] = lib.get<"by_author">().equal_range("Stroustrup");
    for (auto it = lo; it != hi; ++it)
        std::println("  {} ({})", it->title, it->year);

    // ---------------------------------------------------------------------
    //  Your turn — mess about below. A few things to try:
    //
    //    * Add a 4th index, e.g. ranked_non_unique<"by_year", key<&Book::year>>
    //      then call lib.get<"by_year">().nth(0) for the oldest book.
    //
    //    * Modify a book in place (repositions it in every index):
    //        auto it = lib.get<"by_id">().find(2);
    //        lib.get<"by_id">().modify(it, [](Book& b){ b.year = 1999; });
    //
    //    * Jump between indices with project<>():
    //        auto t = lib.get<"by_title">().find("A Tour of C++");
    //        auto a = lib.project<"by_author">(t);   // same book, author index
    //
    //    * Try try_insert(...) and inspect the std::expected error on a clash.
    // ---------------------------------------------------------------------

    return 0;
}
