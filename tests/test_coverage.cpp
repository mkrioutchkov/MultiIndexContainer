// =============================================================================
//  tests/test_coverage.cpp
//
//  A broad regression net exercising every index kind, every operation, and the
//  awkward edges — so a storage-layer rewrite can't silently change behaviour.
//  Standalone (own main + harness); built as a second test binary.
// =============================================================================

#include "mic/multi_index.hpp"

#include <algorithm>
#include <array>
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

// Compile-time guard for the EXAMPLES.md §13 constexpr-table snippet (a mic
// container can't be constexpr; this is the documented working equivalent). The
// static_asserts run the lookup during translation, so a green build proves it.
namespace md_section13 {
struct KW { std::string_view word; int id; };
constexpr std::array KEYWORDS = std::to_array<KW>({ {"else", 2}, {"if", 1}, {"while", 3} });
constexpr const KW* find_kw(std::string_view w) {
    auto it = std::ranges::lower_bound(KEYWORDS, w, {}, &KW::word);
    return (it != KEYWORDS.end() && it->word == w) ? &*it : nullptr;
}
static_assert(find_kw("while") && find_kw("while")->id == 3);
static_assert(find_kw("switch") == nullptr);
}

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
    CHECK(r.error().index_pos == 1);                        // by_email is index #1
    CHECK(r.error().blocking != nullptr);
    CHECK(r.error().blocking->id == 1);                     // collided with the live element
    CHECK(t.size() == 1);                                   // rejected insert leaves container intact
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
    CHECK(t.get<"by_score">().count(999) == 1);               // inline-key cache refreshed: new key found
    CHECK(t.get<"by_score">().count(100) == 0);               // ...and the stale old key is gone

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

    // proxy-level extract by key and by iterator (re-key while detached), then
    // restore id 100 so the rest of the test sees unchanged state.
    auto nh3 = t.get<"by_id">().extract(100);            // by key
    CHECK(t.size() == 1 && nh3.value().id == 100);
    nh3.value().id = 5;
    CHECK(t.insert(std::move(nh3)).inserted);
    CHECK(t.get<"by_id">().contains(5));
    CHECK(t.get<"by_id">().extract(999).empty());        // missing key -> empty handle
    auto nh4 = t.get<"by_id">().extract(t.get<"by_id">().find(5));   // by iterator
    nh4.value().id = 100;
    CHECK(t.insert(std::move(nh4)).inserted);
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

    // order-statistics round trip: rank(nth(i)) == i across the whole index, and
    // the end()/size boundaries agree.
    for (std::size_t i = 0; i < t.size(); ++i) CHECK(rs.rank(rs.nth(i)) == i);
    CHECK(rs.nth(t.size()) == rs.end());
    CHECK(rs.rank(rs.end()) == t.size());
    CHECK(rs.lower_bound(10) == rs.equal_range(10).first);
    CHECK(rs.upper_bound(10) == rs.equal_range(10).second);
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

// ===========================================================================
//  insert_error names WHICH unique index rejected an insert, across all index
//  kinds, and carries the blocking element. (mic-over-Boost diagnostic.)
void test_insert_diagnostics() {
    std::println("[cov] insert_error: which index rejected + blocking element");
    struct User { std::uint64_t id; std::string username; std::string email; };
    using T = mic::multi_index_container<User, mic::indexed_by<
        mic::ordered_unique<"by_id",       mic::key<&User::id>>,
        mic::hashed_unique <"by_username", mic::key<&User::username>>,
        mic::hashed_unique <"by_email",    mic::key<&User::email>>>>;
    T t;
    t.insert(User{1, "ada",   "ada@x.io"});
    t.insert(User{2, "linus", "linus@x.io"});

    // id collision -> ordered_unique index #0
    {
        auto r = t.try_insert(User{1, "fresh", "fresh@x.io"});
        CHECK(!r);
        CHECK(r.error().index_tag == "by_id");
        CHECK(r.error().index_pos == 0);
        CHECK(r.error().blocking->username == "ada");
    }
    // username collision -> hashed_unique index #1
    {
        auto r = t.try_insert(User{9, "ada", "new@x.io"});
        CHECK(!r);
        CHECK(r.error().index_tag == "by_username");
        CHECK(r.error().index_pos == 1);
        CHECK(r.error().blocking->id == 1);
    }
    // email collision -> hashed_unique index #2
    {
        auto r = t.try_insert(User{9, "newname", "linus@x.io"});
        CHECK(!r);
        CHECK(r.error().index_tag == "by_email");
        CHECK(r.error().index_pos == 2);
        CHECK(r.error().blocking->id == 2);
    }
    CHECK(t.size() == 2);                                   // none of the rejects landed

    // a fully fresh element succeeds and the expected holds an iterator to it
    auto ok = t.try_insert(User{3, "carol", "carol@x.io"});
    CHECK(ok.has_value());
    CHECK((**ok).id == 3);
    CHECK(t.size() == 3);
}

// ===========================================================================
//  Deeper composite-key coverage: a 3-component key, exact + every prefix
//  length + open-ended bracketed ranges.
void test_composite_prefix() {
    std::println("[cov] composite full key + 1/2-prefix + open-ended ranges");
    struct Emp { std::string dept; int level; std::string name; };
    using T = mic::multi_index_container<Emp, mic::indexed_by<
        mic::ordered_non_unique<"k", mic::key<&Emp::dept, &Emp::level, &Emp::name>>>>;
    T t;
    t.insert(Emp{"Eng",   3, "Ada"});
    t.insert(Emp{"Eng",   3, "Bjarne"});
    t.insert(Emp{"Eng",   5, "Carol"});
    t.insert(Emp{"Eng",   2, "Dan"});
    t.insert(Emp{"Sales", 2, "Eve"});
    t.insert(Emp{"Sales", 4, "Frank"});
    auto k = t.get<"k">();

    // full key -> exactly one
    CHECK(k.count(std::tuple{std::string("Eng"), 5, std::string("Carol")}) == 1);
    auto f = k.find(std::tuple{std::string("Eng"), 5, std::string("Carol")});
    CHECK(f != k.end() && f->name == "Carol");

    // 1-prefix -> whole department
    CHECK(k.count(std::tuple{std::string("Eng")}) == 4);
    CHECK(k.count(std::tuple{std::string("Sales")}) == 2);
    CHECK(k.count(std::tuple{std::string("Nope")}) == 0);

    // 2-prefix -> one level within a department
    {
        auto [lo, hi] = k.equal_range(std::tuple{std::string("Eng"), 3});
        CHECK(std::distance(lo, hi) == 2);                 // Ada, Bjarne
        std::vector<std::string> names;
        for (auto it = lo; it != hi; ++it) names.push_back(it->name);
        CHECK((names == std::vector<std::string>{"Ada", "Bjarne"}));
    }

    // open-ended bracket: Eng levels [2,4] -> Dan(2), Ada(3), Bjarne(3)
    {
        auto lo = k.lower_bound(std::tuple{std::string("Eng"), 2});
        auto hi = k.upper_bound(std::tuple{std::string("Eng"), 4});
        int n = 0;
        for (auto it = lo; it != hi; ++it) { CHECK(it->level >= 2 && it->level <= 4); ++n; }
        CHECK(n == 3);
    }

    // global order is lexicographic across the whole composite
    std::vector<std::string> order;
    for (const Emp& e : k) order.push_back(e.name);
    CHECK((order == std::vector<std::string>{"Dan", "Ada", "Bjarne", "Carol", "Eve", "Frank"}));
}

// ===========================================================================
//  std::from_range construction and std::format summary.
void test_from_range_and_format() {
    std::println("[cov] std::from_range ctor + std::format summary");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&Rec::id>>,
        mic::hashed_unique <"by_em", mic::key<&Rec::email>>>>;
    std::vector<Rec> seed{{1, "a", "a@x", 0}, {2, "b", "b@x", 0}, {3, "c", "c@x", 0}};
    T t(std::from_range, seed);
    CHECK(t.size() == 3);
    CHECK(t.get<"by_id">().contains(2));
    CHECK(t.get<"by_em">().find("c@x")->id == 3);
    CHECK(std::format("{}", t) == "multi_index(size=3)");
}

// ===========================================================================
//  Transparent (heterogeneous) hashed lookup: string_view / const char* probes
//  hit a std::string-keyed index without constructing a std::string.
void test_transparent_lookup() {
    std::println("[cov] transparent hashed lookup (string_view / const char*)");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::hashed_unique    <"by_email", mic::key<&Rec::email>>,
        mic::hashed_non_unique<"by_name",  mic::key<&Rec::name>>>>;
    T t;
    t.insert(Rec{1, "ada",  "ada@x.io",  0});
    t.insert(Rec{2, "ada",  "ada2@x.io", 0});
    t.insert(Rec{3, "bob",  "bob@x.io",  0});

    auto em = t.get<"by_email">();
    std::string_view sv = "ada@x.io";
    CHECK(em.find(sv)->id == 1);                 // string_view probe
    CHECK(em.find("bob@x.io")->id == 3);         // const char* probe
    CHECK(em.contains(std::string_view{"ada2@x.io"}));
    CHECK(!em.contains("missing@x.io"));
    CHECK(em.count("ada@x.io") == 1);

    auto nm = t.get<"by_name">();
    CHECK(nm.count(std::string_view{"ada"}) == 2);
    auto [lo, hi] = nm.equal_range("ada");
    CHECK(std::distance(lo, hi) == 2);
    CHECK(nm.count("nobody") == 0);
}

// ===========================================================================
//  A moved-from container (incl. its hashed indices) is a valid, queryable empty
//  container — querying it must not divide-by-zero, and it can be refilled.
void test_moved_from_valid() {
    std::println("[cov] moved-from container is a valid empty queryable container");
    using T = mic::multi_index_container<Rec, mic::indexed_by<
        mic::hashed_unique     <"by_email", mic::key<&Rec::email>>,
        mic::hashed_non_unique <"by_name",  mic::key<&Rec::name>>,
        mic::ordered_unique    <"by_id",    mic::key<&Rec::id>>>>;
    // ---- move construction ----
    {
        T a;
        a.insert(Rec{1, "ada", "ada@x", 0});
        a.insert(Rec{2, "bob", "bob@x", 0});
        T b = std::move(a);
        CHECK(b.size() == 2);
        // querying the moved-from source must not crash (was: %0 divide-by-zero)
        CHECK(a.size() == 0);
        CHECK(a.get<"by_email">().find("ada@x") == a.get<"by_email">().end());
        CHECK(a.get<"by_email">().count("ada@x") == 0);
        CHECK(a.get<"by_name">().count("ada") == 0);
        CHECK(a.begin() == a.end());
        // ...and it can be refilled and used normally
        a.insert(Rec{7, "carol", "carol@x", 0});
        a.insert(Rec{8, "dave",  "dave@x",  0});
        CHECK(a.size() == 2);
        CHECK(a.get<"by_email">().find("carol@x")->id == 7);
        CHECK(a.get<"by_id">().contains(8));
    }
    // ---- move assignment ----
    {
        T a; a.insert(Rec{1, "x", "x@x", 0});
        T b; b.insert(Rec{2, "y", "y@x", 0}); b.insert(Rec{3, "z", "z@x", 0});
        a = std::move(b);
        CHECK(a.size() == 2);
        CHECK(b.get<"by_email">().count("y@x") == 0);   // moved-from source: no crash
        b.insert(Rec{9, "w", "w@x", 0});                // still usable
        CHECK(b.get<"by_email">().find("w@x")->id == 9);
        CHECK(b.size() == 1);
    }
}

// ===========================================================================
//  Features brought over from the EXAMPLES.md proposal.
// ===========================================================================
struct Person { int id; std::string dept; std::string name; int salary; };
struct Boss   { std::string dept; std::string name; };

void test_aliases() {
    std::println("[cov] mic::multi_index / mic::pmr::multi_index aliases");
    using A = mic::multi_index<Person, mic::indexed_by<mic::ordered_unique<"by_id", mic::key<&Person::id>>>>;
    static_assert(std::is_same_v<A, mic::multi_index_container<Person, mic::indexed_by<mic::ordered_unique<"by_id", mic::key<&Person::id>>>>>);
    A a; a.insert({1, "Eng", "Ada", 100}); CHECK(a.size() == 1);
    using PA = mic::pmr::multi_index<Person, mic::indexed_by<mic::ordered_unique<"by_id", mic::key<&Person::id>>>>;
    std::pmr::monotonic_buffer_resource pool;
    PA p{&pool}; p.insert({1, "Eng", "Ada", 100}); CHECK(p.size() == 1);
}

void test_keyof_and_prefix() {
    std::println("[cov] proxy.key_of, mic::prefix");
    using T = mic::multi_index<Person, mic::indexed_by<
        mic::ordered_non_unique<"by_dn", mic::key<&Person::dept, &Person::name>>>>;
    T t;
    t.insert({1, "Eng", "Ada", 0}); t.insert({2, "Eng", "Bjarne", 0}); t.insert({3, "Ops", "Cleo", 0});
    auto dn = t.get<"by_dn">();
    auto k = dn.key_of(*dn.begin());                 // tuple<string,string>
    CHECK(std::get<0>(k) == "Eng" && std::get<1>(k) == "Ada");
    auto [lo, hi] = dn.equal_range(mic::prefix(std::string("Eng")));
    CHECK(std::distance(lo, hi) == 2);
}

void test_range_bounds() {
    std::println("[cov] ordered range() with key_ge/gt/le/lt/unbounded");
    using T = mic::multi_index<Person, mic::indexed_by<
        mic::ordered_non_unique<"by_ds", mic::key<&Person::dept, &Person::salary>>>>;
    T t;   // Person = { id, dept, name, salary }
    t.insert({1, "Eng", "Dan", 140}); t.insert({2, "Eng", "Ada", 160});
    t.insert({3, "Eng", "Bo", 180});  t.insert({4, "Eng", "Cy", 240});
    t.insert({5, "Sales", "Ev", 120});
    auto ds = t.get<"by_ds">();
    CHECK(std::ranges::distance(ds.range(mic::key_ge(std::tuple{std::string("Eng"), 150}),
                                         mic::key_le(mic::prefix(std::string("Eng"))))) == 3);   // 160,180,240
    CHECK(std::ranges::distance(ds.range(mic::unbounded, mic::key_lt(std::tuple{std::string("Eng"), 180}))) == 2);
    CHECK(std::ranges::distance(ds.range(mic::key_gt(std::tuple{std::string("Eng"), 160}), mic::unbounded)) == 3);
    CHECK(std::ranges::distance(ds.range(mic::unbounded, mic::unbounded)) == 5);
}

void test_try_mutation() {
    std::println("[cov] try_modify / try_replace / modify_key / on_collision::erase / try_emplace");
    using T = mic::multi_index<Person, mic::indexed_by<
        mic::ordered_unique    <"by_id",    mic::key<&Person::id>>,
        mic::hashed_unique     <"by_name",  mic::key<&Person::name>>,
        mic::ranked_non_unique <"by_salary",mic::key<&Person::salary>>>>;
    T t; t.insert({1, "Eng", "Ada", 100}); t.insert({2, "Eng", "Bo", 200});
    auto id = t.get<"by_id">();

    auto r = id.try_modify(id.find(1), [](Person& p){ p.salary = 150; });
    CHECK(r && (**r).salary == 150);
    auto r2 = id.try_modify(id.find(1), [](Person& p){ p.name = "Bo"; });
    CHECK(!r2 && r2.error().index_tag == "by_name" && r2.error().blocking->id == 2);
    CHECK(id.find(1)->name == "Ada");                 // kept

    t.get<"by_salary">().modify_key(t.get<"by_salary">().find(150), [](int& s){ s += 5; });
    CHECK(id.find(1)->salary == 155);

    bool ok = id.modify(id.find(1), [](Person& p){ p.name = "Bo"; }, mic::on_collision::erase);
    CHECK(!ok && !id.contains(1) && t.size() == 1);    // dropped

    auto rr = id.try_replace(id.find(2), Person{2, "Ops", "Bz", 9});
    CHECK(rr && id.find(2)->dept == "Ops");
    CHECK(!t.try_emplace(2, "x", "y", 0));             // dup id
    CHECK(t.try_emplace(7, "z", "Zed", 0).has_value());
}

void test_format_specs() {
    std::println("[cov] std::format specs: {{}}, stats, audit, full, index=");
    struct E { int id; std::string name; };
    using T = mic::multi_index<E, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&E::id>>,
        mic::hashed_unique <"by_name", mic::key<&E::name>>>>;
    T t; t.insert({2, "b"}); t.insert({1, "a"});
    CHECK(std::format("{}", t) == "multi_index(size=2)");
    CHECK(std::format("{:stats}", t).find("by_name") != std::string::npos);
    CHECK(std::format("{:audit}", t).find("OK") != std::string::npos);
    // value_type E has no formatter -> {:full} reports that, not a compile error
    CHECK(std::format("{:full}", t).find("not formattable") != std::string::npos);
    auto s = t.stats();
    CHECK(s.index<"by_id">().unique);
    CHECK(s.index<"by_name">().kind == mic::index_kind::hashed);
    CHECK(s.all().size() == 2);
}

void test_observers() {
    std::println("[cov] observed container: subscribe / RAII token / fire on insert-erase-modify");
    using T = mic::observed<Person, mic::indexed_by<mic::ordered_unique<"by_id", mic::key<&Person::id>>>>;
    T t;
    int ins = 0, ers = 0, mod = 0;
    {
        auto tok = t.subscribe({ .on_insert = [&](const Person&){ ++ins; },
                                 .on_erase  = [&](const Person&){ ++ers; },
                                 .on_modify = [&](const Person&){ ++mod; } });
        t.insert({1, "Eng", "Ada", 0});
        t.insert({2, "Eng", "Bo", 0});
        t.get<"by_id">().modify(t.get<"by_id">().find(1), [](Person& p){ p.name = "A"; });
        t.get<"by_id">().erase(t.get<"by_id">().find(2));
    }
    CHECK(ins == 2 && mod == 1 && ers == 1);
    int before = ins;
    t.insert({3, "Eng", "C", 0});                       // token gone -> silent
    CHECK(ins == before);
}

#if MIC_HAS_GENERATOR
void test_queries() {
    std::println("[cov] mic::queries::equi_join + group_by");
    using S = mic::multi_index<Person, mic::indexed_by<mic::ordered_non_unique<"by_dept", mic::key<&Person::dept>>>>;
    using M = mic::multi_index<Boss,   mic::indexed_by<mic::ordered_non_unique<"by_dept", mic::key<&Boss::dept>>>>;
    S s; s.insert({1, "Eng", "Ada", 0}); s.insert({2, "Eng", "Bo", 0}); s.insert({3, "Ops", "Cy", 0});
    s.insert({4, "Research", "Dee", 0});                 // no boss
    M m; m.insert({"Eng", "Mae"}); m.insert({"Eng", "Ned"}); m.insert({"Ops", "Ola"}); m.insert({"Sales", "Pat"});

    int pairs = 0;
    for (auto&& [p, b] : mic::queries::equi_join(s.get<"by_dept">(), m.get<"by_dept">())) { (void)p; (void)b; ++pairs; }
    CHECK(pairs == 5);                                   // Eng 2x2=4 + Ops 1x1=1

    std::vector<std::pair<std::string, std::ptrdiff_t>> groups;
    for (auto&& [dept, grp] : mic::queries::group_by(s.get<"by_dept">()))
        groups.emplace_back(dept, std::ranges::distance(grp));
    CHECK((groups == std::vector<std::pair<std::string,std::ptrdiff_t>>{{"Eng",2},{"Ops",1},{"Research",1}}));
}
#endif

void test_static_capacity() {
    std::println("[cov] static_multi_index: inline arena, no heap, capacity_exceeded");
    using Fixed = mic::static_multi_index<Person, 8, mic::indexed_by<
        mic::ordered_unique<"by_id", mic::key<&Person::id>>,
        mic::hashed_unique <"by_name", mic::key<&Person::name>>>>;
    CHECK(Fixed::capacity == 8);
    Fixed t;
    int ok = 0, cap = 0;
    for (int i = 0; i < 100000; ++i) {
        auto r = t.try_emplace(i, "d", std::format("n{}", i), 0);
        if (r) ++ok;
        else if (r.error().why == mic::insert_error<Person>::reason::capacity_exceeded) ++cap;
    }
    CHECK(ok >= 8);                                  // holds at least N with no heap
    CHECK(cap > 0);                                  // overflow reported, not crashed
    CHECK(t.size() == static_cast<std::size_t>(ok));
    CHECK(t.get<"by_id">().find(0) != t.get<"by_id">().end());   // lookups still work

    // duplicate vs capacity: on a NON-full container a clash reports duplicate
    // (a full container can't even allocate the node, so it reports capacity).
    Fixed t2;
    CHECK(t2.try_emplace(1, "d", "x", 0).has_value());
    auto dup = t2.try_emplace(1, "d", "y", 0);       // same id, room to spare
    CHECK(!dup && dup.error().why == mic::insert_error<Person>::reason::duplicate_in_unique_index);
}

void test_aggregate_key() {
    std::println("[cov] aggregate-struct key via free-fn extractor + key_of decomposition");
    struct DS { std::string dept; int salary; auto operator<=>(const DS&) const = default; };
    struct fn { static DS of(const Person& p) { return {p.dept, p.salary}; } };
    using T = mic::multi_index<Person, mic::indexed_by<mic::ordered_non_unique<"by_ds", mic::global_fun<Person, DS, &fn::of>>>>;
    T t; t.insert({1, "Eng", "Ada", 180}); t.insert({2, "Ops", "Bo", 200});
    auto f = t.get<"by_ds">().find(DS{.dept = "Eng", .salary = 180});
    CHECK(f != t.get<"by_ds">().end());
    auto [dept, salary] = t.get<"by_ds">().key_of(*f);
    CHECK(dept == "Eng" && salary == 180);
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
    test_insert_diagnostics();
    test_composite_prefix();
    test_from_range_and_format();
    test_transparent_lookup();
    test_modify_replace();
    test_copy_move_swap();
    test_moved_from_valid();
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
    test_aliases();
    test_keyof_and_prefix();
    test_range_bounds();
    test_try_mutation();
    test_format_specs();
    test_observers();
#if MIC_HAS_GENERATOR
    test_queries();
#endif
    test_static_capacity();
    test_aggregate_key();

    std::println("\n[coverage] {}/{} checks passed", g_checks - g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
