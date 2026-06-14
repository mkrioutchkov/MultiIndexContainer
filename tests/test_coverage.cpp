// =============================================================================
//  tests/test_coverage.cpp
//
//  A broad regression net exercising every index kind, every operation, and the
//  awkward edges — so a storage-layer rewrite can't silently change behaviour.
//  Standalone (own main + harness); built as a second test binary.
// =============================================================================

#include "mic/multi_index.hpp"

#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <memory_resource>
#include <print>
#include <random>
#include <ranges>
#include <stdexcept>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {

int g_checks = 0, g_fail = 0;
void check(bool c, const char* what, int line) {
    ++g_checks;
    if (!c) { ++g_fail; std::println("  FAIL [{}]: {}", line, what); }
}
#define CHECK(x) check((x), #x, __LINE__)

struct Rec {
    int          id;
    std::string  name;
    std::string  email;
    int          score;
    std::string  category() const { return name.empty() ? "?" : std::string(1, name[0]); }
};

// ===========================================================================
void test_sequenced() {
    std::println("[cov] sequenced index");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&Rec::id>>, mic::sequenced<"seq">>>;
    T t;
    auto seq = t.get<"seq">();
    seq.push_back(Rec{1, "a", "a@x", 0});
    seq.push_back(Rec{2, "b", "b@x", 0});
    seq.push_front(Rec{3, "c", "c@x", 0});
    std::vector<int> order;
    for (const Rec& r : seq) order.push_back(r.id);
    CHECK((order == std::vector<int>{3, 1, 2}));
    CHECK(seq.front().id == 3);
    CHECK(seq.back().id == 2);

    auto [pos, ok] = seq.push_back(Rec{1, "dup", "d@x", 0});   // dup id rejected
    CHECK(!ok);
    CHECK(t.size() == 3);

    seq.relocate(seq.begin(), std::prev(seq.end()));           // move id 2 to front
    order.clear(); for (const Rec& r : seq) order.push_back(r.id);
    CHECK((order == std::vector<int>{2, 3, 1}));

    seq.reverse();
    order.clear(); for (const Rec& r : seq) order.push_back(r.id);
    CHECK((order == std::vector<int>{1, 3, 2}));

    seq.pop_front(); seq.pop_back();
    CHECK(t.size() == 1);
    CHECK(seq.front().id == 3);
}

// ===========================================================================
void test_random_access() {
    std::println("[cov] random_access index");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&Rec::id>>, mic::random_access<"ra">>>;
    T t;
    auto ra = t.get<"ra">();
    for (int i = 0; i < 5; ++i) ra.push_back(Rec{i, "n", "e" + std::to_string(i), i * 10});
    CHECK(ra.capacity() >= 5);
    CHECK(ra[0].id == 0);
    CHECK(ra.at(4).id == 4);
    CHECK(ra.front().id == 0);
    CHECK(ra.back().id == 4);
    CHECK(ra.nth_at(2)->id == 2);
    CHECK(ra.index_of(ra.nth_at(3)) == 3);

    t.get<"by_id">().erase_key(2);
    std::vector<int> ids;
    for (const Rec& r : ra) ids.push_back(r.id);
    CHECK((ids == std::vector<int>{0, 1, 3, 4}));
    CHECK(ra[2].id == 3);
}

// ===========================================================================
void test_ordered_lookups() {
    std::println("[cov] ordered lookups (unique + non-unique, heterogeneous)");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique    <"by_id",   mic::key<&Rec::id>>,
        mic::ordered_non_unique<"by_name", mic::key<&Rec::name>>>>;
    T t;
    t.insert(Rec{1, "alice", "a", 0});
    t.insert(Rec{2, "bob",   "b", 0});
    t.insert(Rec{3, "alice", "c", 0});
    t.insert(Rec{4, "carol", "d", 0});

    auto id = t.get<"by_id">();
    CHECK(id.find(3)->email == "c");
    CHECK(id.contains(2));
    CHECK(!id.contains(99));
    CHECK(id.count(4) == 1);
    CHECK(id.lower_bound(2)->id == 2);
    CHECK(id.upper_bound(2)->id == 3);

    auto nm = t.get<"by_name">();
    CHECK(nm.count("alice") == 2);
    CHECK(nm.find("bob")->id == 2);                          // const char*
    CHECK(nm.find(std::string_view("carol"))->id == 4);     // string_view
    auto [lo, hi] = nm.equal_range("alice");
    CHECK(std::distance(lo, hi) == 2);

    std::vector<int> ids;
    for (const Rec& r : nm) ids.push_back(r.id);
    CHECK((ids == std::vector<int>{1, 3, 2, 4}));            // alice,alice,bob,carol
}

// ===========================================================================
void test_hashed_lookups() {
    std::println("[cov] hashed lookups (transparent + buckets)");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::hashed_unique     <"by_email", mic::key<&Rec::email>>,
        mic::hashed_non_unique <"by_name",  mic::key<&Rec::name>>>>;
    T t;
    t.insert(Rec{1, "x", "a@x", 0});
    t.insert(Rec{2, "x", "b@x", 0});
    t.insert(Rec{3, "y", "c@x", 0});

    auto em = t.get<"by_email">();
    CHECK(em.find("a@x")->id == 1);
    CHECK(em.find(std::string("b@x"))->id == 2);
    CHECK(em.find(std::string_view("c@x"))->id == 3);
    CHECK(em.contains("a@x"));
    CHECK(!em.contains("zzz"));
    CHECK(em.bucket_count() >= 1);
    CHECK(em.load_factor() > 0.0f);

    auto nm = t.get<"by_name">();
    CHECK(nm.count("x") == 2);
    auto [lo, hi] = nm.equal_range("x");
    CHECK(std::distance(lo, hi) == 2);

    auto [it, ok] = t.insert(Rec{9, "z", "a@x", 0});        // dup email
    CHECK(!ok);
    CHECK(t.size() == 3);
}

// ===========================================================================
void test_composite() {
    std::println("[cov] composite keys (full + prefix; hashed full)");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique    <"by_id", mic::key<&Rec::id>>,
        mic::ordered_non_unique<"by_ns", mic::key<&Rec::name, &Rec::score>>,
        mic::hashed_non_unique <"hns",   mic::key<&Rec::name, &Rec::score>>>>;
    T t;
    t.insert(Rec{1, "a", "", 10});
    t.insert(Rec{2, "a", "", 20});
    t.insert(Rec{3, "b", "", 10});

    auto ns = t.get<"by_ns">();
    CHECK(ns.count(std::tuple{std::string("a"), 20}) == 1);
    auto [lo, hi] = ns.equal_range(std::tuple{std::string("a")});   // prefix
    CHECK(std::distance(lo, hi) == 2);
    std::vector<int> ids;
    for (const Rec& r : ns) ids.push_back(r.id);
    CHECK((ids == std::vector<int>{1, 2, 3}));
    CHECK(t.get<"hns">().count(std::tuple{std::string("a"), 10}) == 1);
}

// ===========================================================================
int twice(const Rec& r) { return r.score * 2; }

void test_extractors() {
    std::println("[cov] extractors: member / const_mem_fun / global_fun / identity");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique    <"by_id",  mic::member<Rec, int, &Rec::id>>,
        mic::ordered_non_unique<"by_cat", mic::const_mem_fun<Rec, std::string, &Rec::category>>,
        mic::ordered_non_unique<"by_2x",  mic::global_fun<Rec, int, &twice>>>>;
    T t;
    t.insert(Rec{1, "apple",   "", 5});
    t.insert(Rec{2, "avocado", "", 7});
    t.insert(Rec{3, "banana",  "", 5});
    CHECK(t.get<"by_id">().find(2)->name == "avocado");
    CHECK(t.get<"by_cat">().count("a") == 2);
    CHECK(t.get<"by_2x">().count(10) == 2);

    using S = mic::multi_index_container<int, mic::indexed_by<mic::ordered_unique<"v", mic::identity<int>>>>;
    S s; s.insert(3); s.insert(1); s.insert(2);
    std::vector<int> v(s.begin(), s.end());
    CHECK((v == std::vector<int>{1, 2, 3}));
}

// ===========================================================================
void test_deref_chains() {
    std::println("[cov] dereference: shared_ptr / unique_ptr / reference_wrapper");
    {
        using T = mic::multi_index_container<std::shared_ptr<Rec>,
            mic::indexed_by<mic::ordered_unique<"by_id", mic::member<Rec, int, &Rec::id>>>>;
        T t;
        t.insert(std::make_shared<Rec>(Rec{1, "a", "", 0}));
        t.insert(std::make_shared<Rec>(Rec{2, "b", "", 0}));
        CHECK((*t.get<"by_id">().find(2))->name == "b");
    }
    {
        using T = mic::multi_index_container<std::unique_ptr<Rec>,
            mic::indexed_by<mic::ordered_unique<"by_id", mic::member<Rec, int, &Rec::id>>>>;
        T t;
        t.insert(std::make_unique<Rec>(Rec{5, "u", "", 0}));
        CHECK((*t.get<"by_id">().find(5))->name == "u");
    }
    {
        Rec a{1, "ra", "", 0}, b{2, "rb", "", 0};
        using T = mic::multi_index_container<std::reference_wrapper<Rec>,
            mic::indexed_by<mic::ordered_unique<"by_id", mic::member<Rec, int, &Rec::id>>>>;
        T t;
        t.insert(std::ref(a)); t.insert(std::ref(b));
        CHECK(t.get<"by_id">().find(2)->get().name == "rb");
    }
}

// ===========================================================================
void test_multi_uniqueness() {
    std::println("[cov] uniqueness across all unique indices");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id",    mic::key<&Rec::id>>,
        mic::hashed_unique <"by_email", mic::key<&Rec::email>>>>;
    T t;
    t.insert(Rec{1, "a", "x@x", 0});
    auto [it, ok] = t.insert(Rec{2, "b", "x@x", 0});         // new id, dup email
    CHECK(!ok);
    CHECK(it->id == 1);
    CHECK(t.size() == 1);
    auto r = t.try_insert(Rec{3, "c", "x@x", 0});
    CHECK(!r.has_value());
    CHECK(r.error().index_tag == "by_email");
}

// ===========================================================================
void test_modify_replace() {
    std::println("[cov] modify (non-key / key / conflict) and replace");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique    <"by_id",    mic::key<&Rec::id>>,
        mic::ordered_non_unique<"by_score", mic::key<&Rec::score>>,
        mic::hashed_unique     <"by_email", mic::key<&Rec::email>>>>;
    T t;
    t.insert(Rec{1, "a", "a@x", 100});
    t.insert(Rec{2, "b", "b@x", 200});
    auto id = t.get<"by_id">();

    auto it = id.find(1);
    CHECK(id.modify(it, [](Rec& r) { r.name = "A"; }));       // non-key
    CHECK(it->name == "A");

    CHECK(id.modify(it, [](Rec& r) { r.score = 999; }));      // key change -> by_score
    CHECK(it->id == 1);                                       // by_id iterator still valid
    CHECK(t.get<"by_score">().lower_bound(999)->id == 1);

    bool ok = id.modify(it, [](Rec& r) { r.email = "b@x"; }); // conflict -> rollback
    CHECK(!ok);
    CHECK(id.find(1)->email == "a@x");
    CHECK(t.size() == 2);

    Rec n = *id.find(2); n.name = "B2";
    CHECK(id.replace(id.find(2), n));
    CHECK(id.find(2)->name == "B2");
}

// ===========================================================================
void test_copy_move_swap() {
    std::println("[cov] copy / move / swap / assignment");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&Rec::id>>, mic::sequenced<"seq">>>;
    T a;
    for (int i = 0; i < 4; ++i) a.get<"seq">().push_back(Rec{i, "n", "e" + std::to_string(i), 0});
    a.get<"seq">().relocate(a.get<"seq">().begin(), std::prev(a.get<"seq">().end())); // reorder seq

    T b = a;                                  // copy preserves seq order
    CHECK(b.size() == 4);
    std::vector<int> ao, bo;
    for (const Rec& r : a.get<"seq">()) ao.push_back(r.id);
    for (const Rec& r : b.get<"seq">()) bo.push_back(r.id);
    CHECK(ao == bo);
    CHECK(b.get<"by_id">().contains(3));

    T c = std::move(a);                       // move is O(1), source empty
    CHECK(c.size() == 4);
    CHECK(a.size() == 0);

    T d; d.insert(Rec{100, "z", "z", 0});
    d.swap(c);
    CHECK(d.size() == 4);
    CHECK(c.size() == 1 && c.get<"by_id">().contains(100));

    d = d;                                    // self-assign safe
    CHECK(d.size() == 4);
    c = b;                                    // copy-assign
    CHECK(c.size() == 4);
}

// ===========================================================================
void test_ctors() {
    std::println("[cov] range / initializer_list / from_range constructors");
    using S = mic::multi_index_container<int, mic::indexed_by<mic::ordered_unique<"v", mic::identity<int>>>>;
    std::vector<int> src{5, 3, 1, 4, 2};
    S a(src.begin(), src.end());
    CHECK(a.size() == 5);
    CHECK((std::vector<int>(a.begin(), a.end()) == std::vector<int>{1, 2, 3, 4, 5}));

    S b{9, 7, 8};
    CHECK(b.size() == 3);
    CHECK(b.get<"v">().contains(8));

    S c(std::from_range, std::vector<int>{20, 10, 30});
    CHECK(c.size() == 3);
    CHECK(*c.begin() == 10);
}

// ===========================================================================
void test_node_handle_and_merge() {
    std::println("[cov] node handle (extract / re-key / reinsert / outlive) and merge");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&Rec::id>>>>;
    T t;
    t.insert(Rec{1, "a", "a", 0});
    t.insert(Rec{2, "b", "b", 0});

    auto nh = t.extract(t.get<"by_id">().find(1));
    CHECK(t.size() == 1);
    CHECK(nh.value().name == "a");
    nh.value().id = 100;
    auto ret = t.insert(std::move(nh));
    CHECK(ret.inserted);
    CHECK(t.get<"by_id">().contains(100));

    // failed reinsert returns the handle (node retained, not leaked)
    auto nh2 = t.extract(t.get<"by_id">().find(2));
    t.insert(Rec{2, "clash", "c", 0});       // take id 2
    nh2.value().id = 2;
    auto ret2 = t.insert(std::move(nh2));
    CHECK(!ret2.inserted);
    CHECK(static_cast<bool>(ret2.node));     // handle still owns it

    // handle outliving its container must not crash (it owns its allocator)
    {
        T tmp; tmp.insert(Rec{7, "g", "g", 0});
        auto h = tmp.extract(tmp.get<"by_id">().find(7));
        // tmp goes out of scope here, h survives and frees the node itself
        CHECK(h.value().id == 7);
    }

    // merge: collisions stay in the source
    T other;
    other.insert(Rec{100, "dup", "d", 0});   // clashes with t's 100
    other.insert(Rec{200, "new", "n", 0});
    t.merge(other);
    CHECK(t.get<"by_id">().contains(200));
    CHECK(other.get<"by_id">().contains(100));
}

// ===========================================================================
void test_projection() {
    std::println("[cov] projection across index kinds");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id",   mic::key<&Rec::id>>,
        mic::hashed_unique <"by_email",mic::key<&Rec::email>>,
        mic::sequenced     <"seq">>>;
    T t;
    t.insert(Rec{1, "a", "a@x", 0});
    t.insert(Rec{2, "b", "b@x", 0});

    auto h = t.get<"by_email">().find("b@x");           // hashed iterator
    CHECK(t.project<"by_id">(h)->id == 2);
    CHECK(t.project<"seq">(h)->email == "b@x");
    auto o = t.get<"by_id">().find(1);
    CHECK(t.project<"by_email">(o)->email == "a@x");
}

// ===========================================================================
void test_iterator_stability() {
    std::println("[cov] references stay valid across inserts/erases of OTHER elements");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&Rec::id>>>>;
    T t;
    t.insert(Rec{1, "keep", "k", 0});
    const Rec* p = &*t.get<"by_id">().find(1);
    for (int i = 2; i < 200; ++i) t.insert(Rec{i, "n", "e" + std::to_string(i), 0});
    for (int i = 2; i < 100; ++i) t.get<"by_id">().erase_key(i);
    CHECK(p == &*t.get<"by_id">().find(1));   // address unchanged
    CHECK(p->name == "keep");
}

// ===========================================================================
void test_ranked() {
    std::println("[cov] ranked: nth / rank / unique / bounds");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ranked_unique    <"r_id",    mic::key<&Rec::id>>,
        mic::ranked_non_unique<"r_score", mic::key<&Rec::score>>>>;
    T t;
    t.insert(Rec{1, "a", "a", 30});
    t.insert(Rec{2, "b", "b", 10});
    t.insert(Rec{3, "c", "c", 20});
    t.insert(Rec{4, "d", "d", 10});

    auto rs = t.get<"r_score">();
    CHECK(rs.nth(0)->score == 10);
    CHECK(rs.nth(3)->score == 30);
    CHECK(rs.rank(rs.lower_bound(20)) == 2);   // two 10s before the 20
    CHECK(rs.count(10) == 2);

    auto ri = t.get<"r_id">();
    CHECK(ri.nth(0)->id == 1);
    CHECK(ri.rank(ri.find(3)) == 2);
    auto [it, ok] = t.insert(Rec{1, "x", "x", 0});  // ranked_unique rejects dup id
    CHECK(!ok);
}

// ===========================================================================
struct Throwy {
    int id;
    static inline bool armed = false;
    explicit Throwy(int i) : id(i) {}
    Throwy(const Throwy& o) : id(o.id) { if (armed) throw std::runtime_error("boom"); }
    Throwy(Throwy&&) = default;
    Throwy& operator=(const Throwy&) = default;
    Throwy& operator=(Throwy&&) = default;
};
int throwy_id(const Throwy& t) { return t.id; }

void test_exception_safety() {
    std::println("[cov] strong guarantee: a throwing insert leaves the container unchanged");
    using T = mic::multi_index_container<Throwy, mic::indexed_by<
        mic::ordered_unique<"id", mic::global_fun<Throwy, int, &throwy_id>>>>;
    T t;
    t.insert(Throwy{1});
    t.insert(Throwy{2});
    Throwy victim{3};
    Throwy::armed = true;
    bool threw = false;
    try { t.insert(victim); } catch (const std::exception&) { threw = true; }   // copy throws
    Throwy::armed = false;
    CHECK(threw);
    CHECK(t.size() == 2);                       // unchanged
    CHECK(t.get<"id">().contains(1));
    CHECK(t.get<"id">().contains(2));
    CHECK(!t.get<"id">().contains(3));
}

// ===========================================================================
struct counting_resource : std::pmr::memory_resource {
    std::size_t allocs = 0, live = 0;
    std::pmr::memory_resource* up = std::pmr::new_delete_resource();
    void* do_allocate(std::size_t b, std::size_t a) override { ++allocs; ++live; return up->allocate(b, a); }
    void do_deallocate(void* p, std::size_t b, std::size_t a) override { --live; up->deallocate(p, b, a); }
    bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override { return this == &o; }
};

void test_pmr_pool() {
    std::println("[cov] pmr: pool resource, propagation, no leak");
    counting_resource res;
    using T = mic::pmr::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&Rec::id>>,
        mic::hashed_unique <"by_email", mic::key<&Rec::email>>,
        mic::sequenced     <"seq">>>;
    {
        T t(&res);
        CHECK(t.get_allocator().resource() == &res);
        for (int i = 0; i < 100; ++i) t.insert(Rec{i, "n", "e" + std::to_string(i), 0});
        CHECK(t.size() == 100);
        CHECK(res.allocs > 100);                // node + indices all from res
        T moved = std::move(t);                 // allocator travels with move
        CHECK(moved.get_allocator().resource() == &res);
        CHECK(moved.size() == 100);
    }
    CHECK(res.live == 0);                        // everything returned, no leak
}

// ===========================================================================
void test_format_and_capacity() {
    std::println("[cov] format / clear / empty");
    using T = mic::multi_index_container<Rec, mic::indexed_by<mic::ordered_unique<"by_id", mic::key<&Rec::id>>>>;
    T t;
    CHECK(t.empty());
    t.insert(Rec{1, "a", "a", 0});
    t.insert(Rec{2, "b", "b", 0});
    std::string s = std::format("{}", t);
    CHECK(s.find("size=2") != std::string::npos);
    t.clear();
    CHECK(t.empty() && t.size() == 0);
    CHECK(t.begin() == t.end());
}

// ===========================================================================
void test_inline_key_boundary() {
    std::println("[cov] inline-key boundary (trivially-copyable vs not)");
    struct Big { std::int64_t a, b, c; bool operator==(const Big&) const = default; auto operator<=>(const Big&) const = default; };
    struct Holder { int id; Big big; };
    // 24-byte trivially-copyable key exceeds the 16-byte inline threshold -> pointer storage path
    using T = mic::multi_index_container<Holder, mic::indexed_by<
        mic::ordered_unique<"by_id",  mic::key<&Holder::id>>,     // int -> inline path
        mic::ordered_unique<"by_big", mic::key<&Holder::big>>>>;  // 24 bytes -> pointer path
    T t;
    t.insert(Holder{1, Big{1, 2, 3}});
    t.insert(Holder{2, Big{4, 5, 6}});
    CHECK(t.get<"by_id">().find(2)->big.a == 4);
    CHECK(t.get<"by_big">().find(Big{1, 2, 3})->id == 1);
}

// ===========================================================================
void test_differential_ordered() {
    std::println("[cov] ordered_non_unique differential vs std::multimap oracle");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique    <"by_id",  mic::key<&Rec::id>>,
        mic::ordered_non_unique<"by_sc",  mic::key<&Rec::score>>>>;
    T t;
    std::mt19937 rng(99);
    std::map<int, int> oracle;     // id -> score
    int next_id = 0;
    auto verify = [&] {
        std::vector<int> scores;
        for (auto& [id, sc] : oracle) scores.push_back(sc);
        std::ranges::sort(scores);
        std::vector<int> got;
        for (const Rec& r : t.get<"by_sc">()) got.push_back(r.score);
        CHECK(got == scores);
        CHECK(t.size() == oracle.size());
        if (!scores.empty()) {
            int x = scores[rng() % scores.size()];
            std::size_t expect = static_cast<std::size_t>(std::ranges::upper_bound(scores, x) - std::ranges::lower_bound(scores, x));
            CHECK(t.get<"by_sc">().count(x) == expect);
        }
    };
    for (int round = 0; round < 500; ++round) {
        int op = static_cast<int>(rng() % 3);
        if (op == 0 || oracle.empty()) {
            int id = next_id++, sc = static_cast<int>(rng() % 30);
            t.insert(Rec{id, "n", "e" + std::to_string(id), sc}); oracle[id] = sc;
        } else if (op == 1) {
            auto it = oracle.begin(); std::advance(it, rng() % oracle.size());
            t.get<"by_id">().erase_key(it->first); oracle.erase(it);
        } else {
            auto it = oracle.begin(); std::advance(it, rng() % oracle.size());
            int nsc = static_cast<int>(rng() % 30);
            t.get<"by_id">().modify(t.get<"by_id">().find(it->first), [&](Rec& r) { r.score = nsc; });
            it->second = nsc;
        }
        if (round % 25 == 0) verify();
    }
    verify();
}

// ===========================================================================
void test_const_element_safety() {
    std::println("[cov] const_element_container: const pointee, modify still works");
    using T = mic::const_element_container<std::shared_ptr<Rec>, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::member<Rec, int, &Rec::id>>>>;
    T t;
    t.insert(std::make_shared<Rec>(Rec{1, "a", "a", 0}));
    static_assert(std::is_same_v<decltype(*t.get<"by_id">().begin()), const Rec&>);
    auto it = t.get<"by_id">().find(1);
    CHECK(it->name == "a");
    CHECK(t.get<"by_id">().modify(it, [](std::shared_ptr<Rec>& p) { p->id = 9; }));
    CHECK(t.get<"by_id">().contains(9));
}

void test_hashed_stress() {
    std::println("[cov] hashed differential stress (rehash / grouping / modify vs oracle)");
    struct Item { int id; int grp; };
    using T = mic::multi_index_container<Item, mic::indexed_by<
        mic::hashed_unique     <"by_id",  mic::key<&Item::id>>,
        mic::hashed_non_unique <"by_grp", mic::key<&Item::grp>>>>;
    T t;
    std::mt19937 rng(7);
    std::map<int, int> oracle;   // id -> grp
    int next_id = 0;
    auto verify = [&] {
        CHECK(t.size() == oracle.size());
        bool by_id_ok = true;
        std::set<int> seen, expect;
        for (auto& [id, g] : oracle) {
            auto it = t.get<"by_id">().find(id);
            if (it == t.get<"by_id">().end() || it->grp != g) by_id_ok = false;
            expect.insert(id);
        }
        CHECK(by_id_ok);
        for (const Item& it : t.get<"by_id">()) seen.insert(it.id);
        CHECK(seen == expect);                                  // iteration covers all, no dups/strays
        std::map<int, int> gcount;
        for (auto& [id, g] : oracle) ++gcount[g];
        bool grp_ok = true;
        for (auto& [g, c] : gcount) {
            if (t.get<"by_grp">().count(g) != static_cast<std::size_t>(c)) grp_ok = false;
            auto [lo, hi] = t.get<"by_grp">().equal_range(g);    // contiguous group
            if (static_cast<std::size_t>(std::distance(lo, hi)) != static_cast<std::size_t>(c)) grp_ok = false;
        }
        CHECK(grp_ok);
    };
    for (int round = 0; round < 800; ++round) {
        int op = static_cast<int>(rng() % 3);
        if (op == 0 || oracle.empty()) {
            int id = next_id++, g = static_cast<int>(rng() % 20);
            t.insert(Item{id, g}); oracle[id] = g;
        } else if (op == 1) {
            auto it = oracle.begin(); std::advance(it, rng() % oracle.size());
            t.get<"by_id">().erase_key(it->first); oracle.erase(it);
        } else {
            auto it = oracle.begin(); std::advance(it, rng() % oracle.size());
            int ng = static_cast<int>(rng() % 20);
            t.get<"by_id">().modify(t.get<"by_id">().find(it->first), [&](Item& x) { x.grp = ng; });
            it->second = ng;
        }
        if (round % 40 == 0) verify();
    }
    verify();
}

} // namespace

int main() {
    test_sequenced();
    test_hashed_stress();
    test_random_access();
    test_ordered_lookups();
    test_hashed_lookups();
    test_composite();
    test_extractors();
    test_deref_chains();
    test_multi_uniqueness();
    test_modify_replace();
    test_copy_move_swap();
    test_ctors();
    test_node_handle_and_merge();
    test_projection();
    test_iterator_stability();
    test_ranked();
    test_exception_safety();
    test_pmr_pool();
    test_format_and_capacity();
    test_inline_key_boundary();
    test_differential_ordered();
    test_const_element_safety();

    std::println("\n[coverage] {}/{} checks passed", g_checks - g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
