// =============================================================================
//  examples/symbol_table.cpp
//
//  A compiler-style symbol table with lexical scoping. Each Symbol has a name, a
//  scope depth, and a sequence number (declaration order). Three indices:
//
//    * ordered_non_unique<"by_name">  -> all declarations of a name (shadowing)
//    * ordered_unique    <"by_seq">   -> stable declaration order / unique handle
//    * ordered_non_unique<"by_scope"> -> bulk-remove a whole scope on exit
//
//  Name resolution returns the declaration in the INNERMOST (deepest) open scope,
//  i.e. the shadowing one. Leaving a scope removes all of its symbols at once.
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\symbol_table.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <cassert>
#include <optional>
#include <print>
#include <string>

struct Symbol {
    std::string  name;
    int          scope;   // lexical depth: 0 = global, higher = more nested
    int          seq;     // monotonically increasing declaration order
    std::string  type;    // payload
};

using SymbolTable = mic::multi_index_container<Symbol,
    mic::indexed_by<
        mic::ordered_non_unique<"by_name",  mic::key<&Symbol::name>>,
        mic::ordered_unique    <"by_seq",   mic::key<&Symbol::seq>>,
        mic::ordered_non_unique<"by_scope", mic::key<&Symbol::scope>>
    >>;

class Scopes {
    SymbolTable tab_;
    int         depth_ = 0;
    int         seq_   = 0;
public:
    void enter() { ++depth_; }
    void leave() {                          // drop every symbol declared at this depth
        if (depth_ == 0) return;
        tab_.get<"by_scope">().erase_key(depth_);
        --depth_;
    }
    void declare(std::string name, std::string type) {
        tab_.get<"by_seq">().insert(Symbol{std::move(name), depth_, seq_++, std::move(type)});
    }
    // Resolve a name to its innermost (deepest-scope) live declaration.
    std::optional<Symbol> resolve(const std::string& name) const {
        auto byName = const_cast<SymbolTable&>(tab_).get<"by_name">();
        auto [lo, hi] = byName.equal_range(name);
        std::optional<Symbol> best;
        for (auto it = lo; it != hi; ++it)
            if (!best || it->scope > best->scope) best = *it;
        return best;
    }
    std::size_t size() const { return tab_.size(); }
};

int main() {
    Scopes s;

    // global scope
    s.declare("x", "int");
    s.declare("printf", "fn");
    assert(s.resolve("x")->type == "int");

    // enter a function body, shadow x
    s.enter();
    s.declare("x", "double");          // shadows global int x
    s.declare("y", "char");
    std::println("inside function: x resolves to {}", s.resolve("x")->type);
    assert(s.resolve("x")->type == "double");   // innermost wins
    assert(s.resolve("y")->type == "char");
    assert(s.resolve("printf")->type == "fn");  // still visible from outer scope

    // nested block, shadow again
    s.enter();
    s.declare("x", "std::string");
    std::println("inside block:    x resolves to {}", s.resolve("x")->type);
    assert(s.resolve("x")->type == "std::string");
    std::println("symbols live: {}", s.size());

    // leave block -> its x disappears, function's x re-emerges
    s.leave();
    std::println("after block:     x resolves to {}", s.resolve("x")->type);
    assert(s.resolve("x")->type == "double");
    assert(!s.resolve("y_block_local").has_value());

    // leave function -> global x re-emerges, y gone
    s.leave();
    std::println("back in global:  x resolves to {}", s.resolve("x")->type);
    assert(s.resolve("x")->type == "int");
    assert(!s.resolve("y").has_value());
    assert(s.size() == 2);             // only the two globals remain

    std::println("\nAll symbol-table assertions passed.");
    return 0;
}
