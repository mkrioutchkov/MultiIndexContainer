// =============================================================================
//  examples/observed_table.cpp
//
//  Two more proposal features made real:
//
//    * mic::observed<...> — opt into lifecycle observers. subscribe({...}) takes
//      a callback set (designated initialisers) and returns an RAII token;
//      observation stops when the token is destroyed. Zero per-op cost until the
//      first subscriber, and nothing at all on a plain mic::multi_index.
//
//    * std::format introspection — "{}" is a summary; "{:stats}" reports each
//      index's kind/uniqueness (and hashed load factors); "{:audit}" runs an
//      invariant check; "{:full}" / "{:index=tag}" dump elements (needs a
//      formattable value_type). staff.stats().index<"tag">() reads it structured.
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\observed_table.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <format>
#include <print>
#include <string>
#include <vector>

struct Account { int id; std::string owner; long balance; };

// Make Account formattable so {:full} / {:index=tag} can print elements.
template <> struct std::formatter<Account> : std::formatter<std::string> {
    auto format(const Account& a, std::format_context& ctx) const {
        return std::formatter<std::string>::format(
            std::format("#{} {} ${}", a.id, a.owner, a.balance), ctx);
    }
};

using Ledger = mic::observed<Account, mic::indexed_by<
    mic::ordered_unique     <"by_id",      mic::key<&Account::id>>,
    mic::hashed_unique      <"by_owner",   mic::key<&Account::owner>>,
    mic::ranked_non_unique  <"by_balance", mic::key<&Account::balance>>
>>;

int main() {
    Ledger ledger;

    // ---- subscribe: an audit trail that lives as long as the token --------
    std::vector<std::string> audit;
    {
        auto token = ledger.subscribe({
            .on_insert = [&](const Account& a){ audit.push_back(std::format("OPEN  {} (${})", a.owner, a.balance)); },
            .on_erase  = [&](const Account& a){ audit.push_back(std::format("CLOSE {}", a.owner)); },
            .on_modify = [&](const Account& a){ audit.push_back(std::format("UPDATE {} -> ${}", a.owner, a.balance)); },
        });

        ledger.insert({1, "Ada",   1'000});
        ledger.insert({2, "Bjarne", 500});
        ledger.insert({3, "Carol", 2'500});

        // a deposit: modify a non-key field via the ranked balance index
        auto by_owner = ledger.get<"by_owner">();
        by_owner.modify(by_owner.find("Bjarne"), [](Account& a){ a.balance += 750; });

        // close an account
        ledger.get<"by_id">().erase(ledger.get<"by_id">().find(3));
    } // token dropped here -> observation stops

    std::println("Audit trail ({} events):", audit.size());
    for (const auto& line : audit) std::println("  {}", line);

    // further changes are now unobserved (no token)
    ledger.insert({4, "Dan", 300});
    std::println("(after token drop, {} accounts, audit still {} events)\n", ledger.size(), audit.size());

    // ---- std::format introspection ----------------------------------------
    std::println("summary : {}", ledger);                 // multi_index(size=3)
    std::println("\n{:stats}\n", ledger);
    std::println("{:audit}\n", ledger);
    std::println("balances ascending:\n{:index=by_balance}\n", ledger);

    auto s = ledger.stats();
    std::println("by_owner is hashed with load_factor {:.2f}", s.index<"by_owner">().load_factor);

    std::println("\nobserved_table done.");
    return 0;
}
