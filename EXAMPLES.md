# Multi-Index Container — Usage Examples

> **Status:** this began as a design proposal and is now **almost entirely
> implemented**. Nearly every snippet below compiles and runs against the shipped
> header; the runnable programs in [`examples/`](examples) exercise the same APIs.
> Where a detail differs from the original sketch (e.g. `get<"tag">()` returns a
> proxy **by value** so bind with `auto`, not `auto&`), the snippet has been
> updated to the real API. The **one** feature kept as *future work* is fully
> `constexpr` tables (§13) — incompatible with the intrusive pointer design; a
> working close equivalent is shown inline. (`static_multi_index`, §10b, is now
> implemented as an inline-arena container.) The source of truth is always
> [`README.md`](README.md) and the compiled programs under [`examples/`](examples).

All examples assume:

```cpp
#include <mic/multi_index.hpp>   // header-only
// mic::multi_index<...> is a short alias for mic::multi_index_container<...>
```

---

## 1. Declaration, insertion, lookup (the employee table)

```cpp
struct Employee {
    std::uint64_t id;
    std::string   name;
    std::string   email;
    std::string   dept;
    int           salary;
};

using EmployeeTable = mic::multi_index<Employee,
    mic::indexed_by<
        mic::ordered_unique    <"by_id",    mic::key<&Employee::id>>,
        mic::ordered_non_unique <"by_name",  mic::key<&Employee::name>>,
        mic::hashed_unique     <"by_email", mic::key<&Employee::email>>,
        mic::ranked_non_unique <"by_salary", mic::key<&Employee::salary>>,
        mic::sequenced         <"by_arrival">
    >>;

EmployeeTable staff;

// Container-level insert threads the element into EVERY index at once.
// Classic return: {iterator-into-first-index, inserted?}
auto [it, ok] = staff.insert(Employee{.id = 1, .name = "Ada",
                                      .email = "ada@x.io", .dept = "Eng",
                                      .salary = 180'000});
assert(ok);

staff.emplace(2, "Linus", "linus@x.io", "Eng", 170'000);

// Transparent lookup — no std::string is materialised from the literal.
if (auto f = staff.get<"by_email">().find("ada@x.io");
    f != staff.get<"by_email">().end())
{
    std::println("found {}", f->name);
}

// Every index sees the same element. Iterate any of them:
for (const Employee& e : staff.get<"by_name">()) std::println("{}", e.name);
for (const Employee& e : staff.get<"by_salary">()) { /* ascending salary */ }
```

**Decisions ratified here:** tag is the **first** template arg of each index spec (a
`"string"` NTTP); `key<&Member>` is the extractor shorthand; container-level `insert`
returns an iterator into the **first declared index**; access is `get<"tag">()` only.

---

## 2. The fallible API (`std::expected`)

```cpp
// try_insert never returns a bare bool — on failure it names the offending index.
std::expected<EmployeeTable::iterator, mic::insert_error<Employee>> r =
    staff.try_insert(Employee{.id = 1, .email = "dup@x.io"}); // id 1 already exists

if (!r) {
    const mic::insert_error<Employee>& e = r.error();
    std::println("rejected by index '{}' (#{})", e.index_tag, e.index_pos);
    // e.why      == mic::insert_error<Employee>::reason::duplicate_in_unique_index
    // e.index_tag -> std::string_view  ("by_id")
    // e.blocking  -> const Employee*    (the live element that owns the conflicting key)
    std::println("conflicts with existing id {}", e.blocking->id);
}

// Monadic composition (note *it dereferences the iterator the expected holds):
auto name = staff.try_insert(newHire)
                 .transform([](auto it) { return it->name; })
                 .value_or("<not inserted>");
```

See [`examples/insert_diagnostics.cpp`](examples/insert_diagnostics.cpp) for a runnable
version that reports which of several unique indices rejected each insert.

**Decision:** `insert`/`emplace` keep the classic `pair<iterator,bool>`; `try_insert`/
`try_emplace` are the `expected`-returning twins. Same for `try_modify`/`try_replace`.

---

## 3. In-place mutation: `modify`, `modify_key`, `replace`

```cpp
auto it = staff.get<"by_id">().find(2);

// modify() runs the mutator, then repositions the element in EVERY index.
// Changing a non-unique key (name) just re-sorts by_name — always succeeds.
bool ok1 = staff.get<"by_id">().modify(it, [](Employee& e){ e.name = "Linus T."; });

// Changing a UNIQUE key that collides triggers rollback. Default semantics:
// rollback-and-KEEP — the element stays put, unchanged, and false is returned.
bool ok2 = staff.get<"by_id">().modify(it, [](Employee& e){ e.email = "ada@x.io"; });
assert(!ok2);                 // collided with Ada
assert(it->email != "ada@x.io"); // element preserved, not erased

// modify_key: hand the mutator only the (writable) key. Works where the index
// key is a plain data member; computed/composite keys use modify() instead.
// Call it through the index that OWNS the key (by_salary); project<>() jumps the
// by_id iterator across to a by_salary one.
staff.get<"by_salary">().modify_key(staff.project<"by_salary">(it), [](int& s){ s += 5'000; });

// replace: whole-element swap, strong guarantee, position stable.
Employee updated = *it; updated.dept = "Platform";
bool ok3 = staff.get<"by_id">().replace(it, updated);

// expected variants name what blocked a rollback (insert_error<Employee> fields,
// same as try_insert): try_modify / try_replace / try_emplace.
auto rr = staff.get<"by_id">().try_modify(it, [](Employee& e){ e.email = "ada@x.io"; });
if (!rr) std::println("blocked by '{}', conflicts with id {}",
                      rr.error().index_tag, rr.error().blocking->id);
```

**Default is rollback-and-keep.** Opt into Boost's legacy erase-on-failure per call
with `staff.get<"by_id">().modify(it, fn, mic::on_collision::erase)`.

---

## 4. Composite keys — tuple form and aggregate-struct form

```cpp
// (a) Tuple-style composite key, compared lexicographically.
using ByDeptSalary = mic::ordered_non_unique<"by_dept_salary",
                          mic::key<&Employee::dept, &Employee::salary>>;

// full-key query:
auto [lo, hi] = staff.get<"by_dept_salary">().equal_range(std::tuple{"Eng", 180'000});

// prefix (partial-key) query — first component only, still O(log n):
auto eng = staff.get<"by_dept_salary">().equal_range(mic::prefix("Eng"));

// open / half-open range with mic::key_ge/key_gt/key_le/key_lt and mic::unbounded.
// Here: Eng employees earning >= 150k, up to the end of the Eng group.
auto well_paid_eng = staff.get<"by_dept_salary">()
        .range(mic::key_ge(std::tuple{"Eng", 150'000}),
               mic::key_le(mic::prefix("Eng")));
// other forms: range(mic::unbounded, mic::key_lt(k)) / range(mic::key_gt(k), mic::unbounded)
```

```cpp
// (b) Aggregate-struct key: operator<=> auto-derives the comparator, designated
//     initialisers build the query, structured bindings read it back. The
//     extractor is a free function (key<&fn>) — closures aren't usable as the
//     NTTP here because their result type can't be deduced without the element.
struct DeptSalary {
    std::string dept;
    int         salary;
    auto operator<=>(const DeptSalary&) const = default;
};
DeptSalary ds_of(const Employee& e) { return {e.dept, e.salary}; }

using ByDS = mic::ordered_non_unique<"by_ds", mic::key<&ds_of>>;

auto f = staff.get<"by_ds">().find({.dept = "Eng", .salary = 180'000});
auto&& [dept, salary] = staff.get<"by_ds">().key_of(*f);   // structured-binding decomposition
```

**Notes:** hashed composite keys are **full-key only** — `prefix(...)`/`range(...)`
need ordering and so are ordered-index only. `proxy.key_of(elem)` re-extracts an
element's key for that index (used above, and by the relational helpers in §12).

---

## 5. Ranges & views

```cpp
// Every index is a borrowed range; compose std::views directly.
auto top3 = staff.get<"by_salary">()
          | std::views::reverse
          | std::views::take(3);
for (const Employee& e : top3) std::println("{}: {}", e.name, e.salary);

// equal_range as a subrange (member form; or std::ranges::equal_range with a
// projection: std::ranges::equal_range(idx, "Ada", {}, &Employee::name)):
auto [lo, hi] = staff.get<"by_name">().equal_range("Ada");
for (auto it = lo; it != hi; ++it) std::println("{}", it->email);

// Build a container straight from a range:
std::vector<Employee> seed = load();
EmployeeTable t2(std::from_range, seed);

// Round-trip back out:
auto names = staff.get<"by_name">()
           | std::views::transform(&Employee::name)
           | std::ranges::to<std::vector>();
```

---

## 6. LRU cache (sequenced recency + hashed lookup)

```cpp
template <class Key, class Val>
class LruCache {
    struct Entry { Key key; Val val; };
    using Store = mic::multi_index<Entry,
        mic::indexed_by<
            mic::sequenced     <"by_recency">,             // front = most recent
            mic::hashed_unique <"by_key", mic::key<&Entry::key>>
        >>;
    Store store_;
    std::size_t cap_;

public:
    explicit LruCache(std::size_t cap) : cap_(cap) {}

    Val* get(const Key& k) {
        auto byKey = store_.get<"by_key">();   // get<>() returns the proxy by value
        auto it = byKey.find(k);
        if (it == byKey.end()) return nullptr;
        // promote: O(1) splice to front via projection — no re-hash, no copy.
        auto seqIt = store_.project<"by_recency">(it);
        store_.get<"by_recency">().relocate(store_.get<"by_recency">().begin(), seqIt);
        return const_cast<Val*>(&it->val);
    }

    void put(Key k, Val v) {
        auto [it, ok] = store_.get<"by_recency">().push_front(Entry{std::move(k), std::move(v)});
        if (!ok) return; // key already present (rejected by hashed_unique)
        if (store_.size() > cap_)
            store_.erase(std::prev(store_.get<"by_recency">().end())); // evict LRU
    }
};
```

---

## 7. Order book (price-time priority)

```cpp
struct Order {
    std::uint64_t id;
    char          side;     // 'B' / 'S'
    std::int64_t  price;
    std::uint64_t ts;       // arrival timestamp = time priority
    std::uint32_t qty;
};

using Book = mic::multi_index<Order,
    mic::indexed_by<
        mic::hashed_unique     <"by_id",    mic::key<&Order::id>>,
        // price-time priority: sort by (price, ts) lexicographically
        mic::ordered_non_unique<"by_px_time", mic::key<&Order::price, &Order::ts>>
    >>;

Book bids;
bids.insert(Order{.id = 1, .side = 'B', .price = 10'100, .ts = 1, .qty = 50});
bids.insert(Order{.id = 2, .side = 'B', .price = 10'100, .ts = 2, .qty = 30});
bids.insert(Order{.id = 3, .side = 'B', .price = 10'050, .ts = 1, .qty = 10});

// Best bid = highest price, earliest time = last in ascending (price,ts) order.
const Order& best = *std::prev(bids.get<"by_px_time">().end());

// Cancel by id in O(1) avg (erase by key is spelled erase_key):
bids.get<"by_id">().erase_key(2);

// Partial fill: modify qty in place (no key changes -> cheap, no repositioning).
auto it = bids.get<"by_id">().find(1);
bids.get<"by_id">().modify(it, [](Order& o){ o.qty -= 20; });
```

---

## 8. Projection between indices

```cpp
// Found via hash, want its position in the recency list, or its ordered neighbours.
auto h = staff.get<"by_email">().find("ada@x.io");

auto ordIt = staff.project<"by_name">(h);   // O(1), same element, by_name iterator
auto seqIt = staff.project<"by_arrival">(h);

// project<>() of a non-existent tag is a COMPILE error listing the real tags.
// Cross-container projection is a debug-mode precondition failure.
```

---

## 9. Node handles: re-key without copying, and merge

```cpp
// extract() detaches a node from ALL indices without deallocating.
auto node = staff.get<"by_id">().extract(1);   // by key (or .extract(iterator))
node.value().id = 1000;            // mutate a key safely while detached
auto res = staff.insert(std::move(node)); // re-link across all indices
if (!res.inserted) {
    // node retained in res.node on failure — nothing leaked
}

// Splice every transferable element from one table into another (uniqueness-checked).
EmployeeTable acquired = load_other_company();
staff.merge(acquired);            // rejected (duplicate) elements stay in `acquired`
```

---

## 10. Allocators: pmr and a fixed-capacity static variant

```cpp
// (a) pmr — pool all the same-sized nodes in a monotonic buffer.
std::array<std::byte, 1 << 20> arena;
std::pmr::monotonic_buffer_resource res{arena.data(), arena.size()};
mic::pmr::multi_index<Employee, mic::indexed_by</*...*/>> fast{&res};

// (b) static_multi_index — inline-arena storage, NO heap allocation. Every node
//     and index structure comes from a buffer embedded in the object; it holds
//     at least N elements. try_insert / try_emplace report capacity_exceeded
//     (instead of throwing) once the arena is full. Best for build-mostly-once
//     tables — the arena reclaims nothing until the container is destroyed.
using Fixed = mic::static_multi_index<Employee, /*N=*/64,
    mic::indexed_by<mic::ordered_unique<"by_id", mic::key<&Employee::id>>>>;
Fixed embedded;
auto r = embedded.try_insert(Employee{/*...*/});
if (!r && r.error().why == mic::insert_error<Employee>::reason::capacity_exceeded) { /* full */ }
```

---

## 11. Formatting & introspection (`std::format`)

Implemented; see [`examples/observed_table.cpp`](examples/observed_table.cpp).
`{:full}` / `{:index=tag}` need a formattable `value_type`; the others always work.

```cpp
std::println("{}", staff);                 // summary: multi_index(size=N)
std::println("{:full}", staff);            // every element (needs formattable Employee)
std::println("{:index=by_salary}", staff); // dump in by_salary order
std::println("{:stats}", staff);           // load factors, collisions, hash_quality()
std::println("{:audit}", staff);           // invariant-audit report

auto s = staff.stats();                    // structured, same data as {:stats}
std::println("by_email load = {:.2f}", s.index<"by_email">().load_factor);
```

---

## 12. Relational query helpers (lazy, `std::generator`)

Implemented (sort-merge join + grouping over ordered indices, both already sorted
on their key). Runnable: [`examples/relational_queries.cpp`](examples/relational_queries.cpp).

```cpp
// Lazy SQL-ish INNER JOIN / GROUP BY returning std::generator — O(n+m), O(1) space.
for (auto&& [emp, mgr] :
        mic::queries::equi_join(staff.get<"by_dept">(),
                                managers.get<"by_dept">()))
{
    std::println("{} reports into {}", emp.name, mgr.name);
}

for (auto&& [dept, group] : mic::queries::group_by(staff.get<"by_dept">()))
    std::println("{}: {} people", dept, std::ranges::distance(group));
```

---

## 13. Compile-time tables (`constexpr`) — *future work*

**Not supported in v1.** The single-allocation intrusive design relies on runtime
pointer manipulation that isn't valid in a constant expression, so a `mic` container
can't be built at compile time. The sketch below is the intended shape *if* a
constexpr-friendly storage backend is ever added:

```cpp
// FUTURE WORK — does not compile today.
consteval auto build_keywords() {
    mic::multi_index<std::pair<std::string_view,int>,
        mic::indexed_by<mic::ordered_unique<"by_word",
            mic::key<&std::pair<std::string_view,int>::first>>>> t;
    t.insert({"if", 1}); t.insert({"else", 2}); t.insert({"while", 3});
    return t;
}
constexpr auto KEYWORDS = build_keywords();
static_assert(KEYWORDS.get<"by_word">().contains("while"));
```

The **working close equivalent today** is a `constexpr` sorted `std::array` with a
binary search — exactly the lookup an ordered index would do, just evaluated at
compile time:

```cpp
struct KW { std::string_view word; int id; };
constexpr std::array KEYWORDS = std::to_array<KW>({   // keep sorted by word
    {"else", 2}, {"if", 1}, {"while", 3} });

constexpr const KW* find_kw(std::string_view w) {
    auto it = std::ranges::lower_bound(KEYWORDS, w, {}, &KW::word);
    return (it != KEYWORDS.end() && it->word == w) ? &*it : nullptr;
}
static_assert(find_kw("while") && find_kw("while")->id == 3);
static_assert(find_kw("switch") == nullptr);
```

---

## 14. Observers (opt-in, zero-overhead when absent)

Use the `mic::observed<...>` alias to opt in; `subscribe(...)` takes a callback set
(any subset, designated initialisers) and returns an RAII token. Observation stops
when the token is destroyed. A plain `mic::multi_index` has no `subscribe()` and
pays nothing. Runnable: [`examples/observed_table.cpp`](examples/observed_table.cpp).

```cpp
using AuditedTable = mic::observed<Employee,
    mic::indexed_by<mic::ordered_unique<"by_id", mic::key<&Employee::id>>>>;

AuditedTable t;
auto token = t.subscribe({
    .on_insert = [](const Employee& e){ log("hired {}", e.name); },
    .on_erase  = [](const Employee& e){ log("left {}",  e.name); },
    .on_modify = [](const Employee& e){ log("changed {}", e.name); },
});
// token is an RAII handle — observation stops when it goes out of scope.
```

---

## Compile-time errors you should expect (diagnostics contract)

```cpp
mic::multi_index<Employee, mic::indexed_by<>>;          // ERROR: requires >=1 index
staff.get<"by_typo">();                                  // ERROR: unknown tag; valid: by_id, by_name, ...
mic::ordered_unique<"by_id", mic::key<&Employee::name>,
                    std::less<int>>;                     // ERROR: less<int> not callable with key 'std::string'
staff.get<"by_email">().lower_bound("a");                // ERROR: hashed index has no ordering ops
```
