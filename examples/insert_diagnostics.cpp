// =============================================================================
//  examples/insert_diagnostics.cpp
//
//  "Which index rejected my insert?" — a diagnostic Boost.MultiIndex does not
//  offer.
//
//  When a container has several unique indices, a failed insert in Boost gives
//  you back only a pair<iterator,bool>: the bool says "no", and the iterator
//  points at *some* conflicting element — but you are never told *which* of the
//  unique constraints actually fired. With three unique indices, you have to
//  re-derive it by hand.
//
//  mic's std::expected-returning try_insert() answers the question directly. On
//  failure it returns an insert_error carrying:
//      * index_pos  — the 0-based position of the offending index
//      * index_tag  — its string-literal tag, e.g. "by_email"
//      * blocking   — a pointer to the existing element you collided with
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\insert_diagnostics.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <cassert>
#include <cstdint>
#include <print>
#include <string>

struct User {
    std::uint64_t id;
    std::string   username;
    std::string   email;
    std::string   display;
};

// Three unique constraints over the same element: id, username and email must
// each be unique. An insert succeeds only if it satisfies ALL of them.
using Users = mic::multi_index_container<User,
    mic::indexed_by<
        mic::ordered_unique<"by_id",       mic::key<&User::id>>,
        mic::hashed_unique <"by_username", mic::key<&User::username>>,
        mic::hashed_unique <"by_email",    mic::key<&User::email>>
    >>;

// Try to register a user and narrate exactly what happened. Returns true on
// success. On failure we report which named index objected and the element it
// clashed with — all from the single insert_error.
static bool register_user(Users& users, User candidate) {
    auto result = users.try_insert(std::move(candidate));
    if (result) {
        const User& u = **result;   // *expected -> iterator, *iterator -> User
        std::println("  [ok]       registered '{}' (id={})", u.username, u.id);
        return true;
    }

    const auto& e = result.error();   // mic::insert_error<User>
    std::println("  [REJECTED]  index '{}' (#{}) refused it; "
                 "collides with existing {{id={}, username='{}', email='{}'}}",
                 e.index_tag, e.index_pos,
                 e.blocking->id, e.blocking->username, e.blocking->email);
    return false;
}

int main() {
    Users users;

    std::println("Registering users (id / username / email must each be unique):");

    assert(register_user(users, {1, "ada",   "ada@x.io",   "Ada Lovelace"}));
    assert(register_user(users, {2, "linus", "linus@x.io", "Linus T."}));

    // --- id collision: same id as ada, everything else fresh ----------------
    {
        bool ok = register_user(users, {1, "grace", "grace@x.io", "Grace H."});
        assert(!ok);
        auto r = users.try_insert(User{1, "grace", "grace@x.io", "Grace H."});
        assert(!r);
        assert(r.error().index_tag == "by_id");          // <-- the headline
        assert(r.error().index_pos == 0);
        assert(r.error().blocking->username == "ada");   // the element we hit
    }

    // --- username collision: fresh id and email, but username "ada" taken ---
    {
        auto r = users.try_insert(User{3, "ada", "ada2@x.io", "Ada II"});
        assert(!r);
        assert(r.error().index_tag == "by_username");
        assert(r.error().index_pos == 1);
        assert(r.error().blocking->id == 1);
        register_user(users, {3, "ada", "ada2@x.io", "Ada II"});
    }

    // --- email collision: fresh id and username, but linus's email reused ---
    {
        auto r = users.try_insert(User{4, "carol", "linus@x.io", "Carol"});
        assert(!r);
        assert(r.error().index_tag == "by_email");
        assert(r.error().index_pos == 2);
        assert(r.error().blocking->username == "linus");
        register_user(users, {4, "carol", "linus@x.io", "Carol"});
    }

    // --- a fully fresh user goes in ----------------------------------------
    assert(register_user(users, {5, "carol", "carol@x.io", "Carol R."}));

    // Exactly the three that satisfied every unique index survive.
    assert(users.size() == 3);
    std::println("\nFinal roster ({} users):", users.size());
    for (const User& u : users.get<"by_id">())
        std::println("  id={}  {:<8} {}", u.id, u.username, u.email);

    // For contrast: the classic Boost-compatible insert() still works and hands
    // back the conflicting element — it just can't tell you which index fired.
    {
        auto [it, ok] = users.insert(User{1, "zoe", "zoe@x.io", "Zoe"});
        assert(!ok);
        assert(it->username == "ada");   // we know *what* we hit, not *which index*
    }

    std::println("\nAll insert-diagnostics assertions passed.");
    return 0;
}
