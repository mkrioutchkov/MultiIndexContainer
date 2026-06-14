// =============================================================================
//  examples/const_elements.cpp
//
//  Storing elements by smart pointer is supported, but it has a sharp edge: a
//  `const std::shared_ptr<Book>&` still hands you a *mutable* `Book&`, so code
//  like `it->id = 7` compiles and silently corrupts the indices (the element is
//  now in the wrong tree/bucket).  That is the same caveat Boost.MultiIndex has.
//
//  mic::const_element_container closes that hole: for a pointer-like element
//  type, its iterators expose the pointee as `const T&`.  Reads work, ranges
//  work, but you cannot mutate a key (or anything) through an iterator — the
//  only way to change an element is modify()/replace(), which re-threads every
//  index correctly.  It is opt-in; plain multi_index_container keeps the
//  Boost-style raw (mutable-pointee) access.
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\const_elements.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <cassert>
#include <memory>
#include <print>
#include <ranges>
#include <string>

struct Book {
    int         id;
    std::string title;
    int         year;
};

// Note const_element_container instead of multi_index_container.
using Catalog = mic::const_element_container<std::shared_ptr<Book>,
    mic::indexed_by<
        mic::ordered_unique   <"by_id",   mic::key<&Book::id>>,
        mic::ranked_non_unique<"by_year", mic::key<&Book::year>>
    >>;

int main() {
    Catalog cat;
    cat.insert(std::make_shared<Book>(1, "A Tour of C++",                2022));
    cat.insert(std::make_shared<Book>(2, "The C++ Programming Language", 2013));
    cat.insert(std::make_shared<Book>(3, "Effective Modern C++",         2014));

    // Iterators yield `const Book&` — dot syntax, read-only.
    std::println("By id:");
    for (const Book& b : cat.get<"by_id">())
        std::println("  #{}  {} ({})", b.id, b.title, b.year);

    // Ranges adaptors still compose (the reference is a real lvalue ref).
    std::println("\nTwo newest:");
    for (const Book& b : cat.get<"by_year">() | std::views::reverse | std::views::take(2))
        std::println("  {} ({})", b.title, b.year);

    // This would NOT compile — keys are immutable through the iterator:
    //     auto it = cat.get<"by_id">().find(1);
    //     it->id = 99;                 // error C3490: accessed through a const object
    //     for (auto& b : cat.get<"by_id">()) b.year = 0;   // also rejected

    // The sanctioned way to change an element: modify(). Its mutator receives the
    // stored shared_ptr<Book>& (mutable), and modify() repositions the element in
    // every index afterwards, so invariants are preserved.
    auto it = cat.get<"by_id">().find(2);
    bool ok = cat.get<"by_id">().modify(it, [](std::shared_ptr<Book>& p) {
        p->year = 1985;                 // changes the by_year key safely
    });
    assert(ok);

    std::println("\nBy year after modify (still correctly ordered):");
    for (const Book& b : cat.get<"by_year">())
        std::println("  {} ({})", b.title, b.year);

    std::println("\nKeys are safe: mutation only flows through modify()/replace().");
    return 0;
}
