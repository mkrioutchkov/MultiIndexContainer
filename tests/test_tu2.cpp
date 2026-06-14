// Second translation unit: included alongside test_main.cpp to catch ODR / inline
// violations in the header-only library (non-inline free functions, etc.).
#include "mic/multi_index.hpp"
#include <string>

namespace {
struct Rec { int id; std::string name; };
}

// Exercised from nowhere; exists only to instantiate the container in a 2nd TU.
std::size_t mic_tu2_probe() {
    mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&Rec::id>>,
        mic::hashed_unique <"by_name", mic::key<&Rec::name>>
    >> t;
    t.insert(Rec{1, "x"});
    t.emplace(2, "y");
    return t.size();
}
