# mic — a C++23 multi-index container

A greenfield, header-only **multi-index container** in the spirit of
[Boost.MultiIndex](https://www.boost.org/doc/libs/release/libs/multi_index/),
designed as a functional **superset**: store each element once and access it
through any number of differently-ordered indices, with modern C++23 ergonomics
on top.

```cpp
#include "mic/multi_index.hpp"

struct Employee { std::uint64_t id; std::string name, email; int salary;
                  std::string full_name() const { return name; } };

using Staff = mic::multi_index_container<Employee, mic::indexed_by<
    mic::ordered_unique     <"by_id",    mic::key<&Employee::id>>,
    mic::ordered_non_unique <"by_name",  mic::key<&Employee::name>>,
    mic::hashed_unique      <"by_email", mic::key<&Employee::email>>,
    mic::ranked_non_unique  <"by_salary",mic::key<&Employee::salary>>,
    mic::sequenced          <"by_arrival">
>>;

Staff s;
s.insert({1, "Ada", "ada@x.io", 180000});
s.emplace(2, "Linus", "linus@x.io", 170000);

s.get<"by_email">().find("ada@x.io")->name;   // "Ada"   (hashed lookup)
for (const Employee& e : s.get<"by_salary">() | std::views::reverse) { /* high→low */ }
```

## Features

* **All the index types** — `ordered_unique` / `ordered_non_unique`,
  `hashed_unique` / `hashed_non_unique`, `sequenced`, `random_access`,
  `ranked_unique` / `ranked_non_unique`.
* **Uniqueness across every index** — an insert succeeds only if it satisfies
  *all* unique indices simultaneously; on failure the blocking element is
  returned.
* **Every key-extraction style** (a superset of Boost):
  * `mic::member<C, T, &C::field>`
  * **member-function-pointer keys** — `mic::const_mem_fun<C, T, &C::getter>`
  * `mic::global_fun<V, T, &free_fn>`
  * `mic::identity<T>`
  * `mic::composite_key<...>` with **lexicographic prefix range queries**
  * `mic::key<&C::a, &C::b, ...>` — deduces the right extractor from the pointer
* **Smart-pointer elements** — store `std::shared_ptr<T>` / `std::unique_ptr<T>`
  / raw pointers / `std::reference_wrapper<T>`; extractors transparently
  dereference to the underlying object. Opt into `mic::const_element_container`
  for **key-mutation safety** (see below).
* **Enforced string-literal tags** — `get<"by_id">()` — *and* Boost-style
  numeric `get<0>()`. Tag typos and unknown indices are compile-time errors.
* **Projection** — `project<"other_tag">(it)` jumps an iterator to the same
  element in another index.
* **`modify` / `replace`** with **rollback-and-keep** semantics, plus
  **node-handle** `extract` / `insert` for zero-copy re-keying, and `merge`.
* **Ranked indices** — `ranked_unique` / `ranked_non_unique` give order
  statistics, `nth(k)` and `rank(it)`, in **O(log n)** via a size-augmented
  order-statistics tree.
* **Transparent hashed lookup** — `get<"by_email">().find("literal")` /
  `find(string_view)` hashes through `string_view` with **zero key
  materialisation** (string keys are transparent by default).
* **Single-allocation, fully intrusive** — every index's linkage lives *inside*
  the element node (ordered/ranked = size-augmented **AVL** tree, hashed =
  bucketed table with chains in the node, sequenced = doubly-linked list), so a
  container of those index kinds does **one allocation per element** — matching
  Boost. `find` matches `std::set`/Boost (small keys cached next to the links);
  `build` and `iterate` meet or beat Boost. (random-access keeps a pointer
  vector, as Boost does.)
* **`std::pmr` support** — `mic::pmr::multi_index_container<...>` routes *every*
  allocation (element nodes **and** all per-index structures) through one
  `std::pmr::memory_resource`, so a pool / `monotonic_buffer_resource` serves the
  whole container.
* **Modern API** — `std::expected`-returning `try_insert` / `try_emplace` /
  `try_modify` / `try_replace` whose `insert_error` names *which* unique index
  rejected the change (by tag) **and** points at the blocking element — a
  diagnostic Boost doesn't surface; `std::ranges`-ready index views (compose
  `views::filter` / `reverse` / `take`), and `std::from_range` construction.
* **Open-ended range scans** — `index.range(mic::key_ge(lo), mic::key_lt(hi))`
  with `mic::unbounded` sides; `mic::prefix(...)` for composite-key prefix queries.
* **Relational helpers** — `mic::queries::equi_join(a, b)` (lazy sort-merge inner
  join of two ordered indices on a shared key) and `mic::queries::group_by(a)`,
  both returning `std::generator` (O(n+m), O(1) space).
* **`std::format` introspection** — `"{}"` summary, `"{:stats}"` (per-index kind /
  uniqueness / hashed load factors), `"{:audit}"` (invariant check), `"{:full}"`
  and `"{:index=tag}"` element dumps, plus a structured `container.stats()`.
* **Lifecycle observers** — opt into `mic::observed<...>`; `subscribe({.on_insert=…,
  .on_erase=…, .on_modify=…})` returns an RAII token. Zero per-op cost until the
  first subscriber, nothing at all on a plain container.
* **`mic::modify_key` / `on_collision::erase`** — edit just the key, and opt into
  Boost's legacy erase-on-collision per call.

## Examples at a glance

Each snippet uses the real, verified API (with `Staff s;` from the example above);
complete, compiled programs live in [`examples/`](examples).

```cpp
// Which index rejected an insert? (a diagnostic Boost doesn't surface)
if (auto r = s.try_insert(dup); !r)
    std::println("rejected by '{}', conflicts with id {}", r.error().index_tag, r.error().blocking->id);
```
```cpp
// Open-ended range scan: everyone earning [150k, 200k)
for (const Employee& e : s.get<"by_salary">().range(mic::key_ge(150'000), mic::key_lt(200'000)))
    use(e);
```
```cpp
// Relational helpers (lazy std::generator): INNER JOIN + GROUP BY
for (auto&& [emp, mgr] : mic::queries::equi_join(s.get<"by_dept">(), managers.get<"by_dept">()))
    std::println("{} reports to {}", emp.name, mgr.name);
for (auto&& [dept, group] : mic::queries::group_by(s.get<"by_dept">()))
    std::println("{}: {}", dept, std::ranges::distance(group));
```
```cpp
// Join on a key prefix (table on K  ⋈  table on (K, …)) — composite equal_range
for (const Quota& q : quotas.get<"by_region">()) {
    auto [lo, hi] = sales.get<"by_region_cat">().equal_range(mic::prefix(q.region));
    for (auto it = lo; it != hi; ++it) join(q, *it);
}
```
```cpp
// Lifecycle observers: an RAII subscription
auto token = s2.subscribe({ .on_insert = [](const Employee& e){ log("hired", e.name); } });
```
```cpp
// std::format introspection
std::println("{:stats}", s);                                  // per-index kinds + load factors
auto lf = s.stats().index<"by_email">().load_factor;          // structured access
```

See [`EXAMPLES.md`](EXAMPLES.md) for the annotated, section-by-section tour.

## Key safety with pointer elements

A multi-index container only stays correct if **keys change exclusively through
`modify()` / `replace()`** — those re-thread the element into every index. For
value-stored elements this is enforced for free: iterators hand out `const T&`,
so `it->key = x` won't compile.

Pointer elements are the sharp edge (the same one Boost has): a
`const std::shared_ptr<T>&` still dereferences to a **mutable** `T&`, so
`it->key = x` compiles and silently corrupts the indices. Two ways to stay safe:

* **`mic::const_element_container<...>`** (opt-in) — for pointer-like elements,
  iterators expose the pointee as `const T&`. Reads and ranges work; mutating a
  key through an iterator is a compile error; `modify()` is still the write path
  (its mutator receives the mutable pointer). Plain `mic::multi_index_container`
  keeps Boost-style raw access. See [examples/const_elements.cpp](examples/const_elements.cpp).
* **Keys by value, payload behind a pointer** — keep keys as plain members
  (const-protected) and put only mutable payload behind a `shared_ptr`; then
  non-key mutation is free and keys can't be corrupted.

## Requirements

* A C++23 toolchain. Primary target: **MSVC 14.5x** (`/std:c++latest`).
  clang 18+ / gcc 14+ parity is a goal.
* Header-only — just add `include/` to your include path.

## Build & test

With CMake:

```bash
cmake -B build -S . -DMIC_BUILD_TESTS=ON -DMIC_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build
```

Directly with MSVC (no CMake required):

```bat
tools\msvc.bat /std:c++latest /EHsc /I include /Fe:build\tests.exe tests\test_main.cpp
build\tests.exe
```

## Benchmarking & profiling

```bash
cmake -B build -S . -DMIC_BUILD_BENCHMARKS=ON && cmake --build build --config Release
./build/mic_bench_bench 100000 1000000          # mic vs hand-rolled std (vs Boost if present)
```

The harness in [`benchmarks/bench.cpp`](benchmarks/bench.cpp) is dependency-free
and runs anywhere. For numbers you can trust, **don't use a laptop** — see
[`BENCHMARKING.md`](BENCHMARKING.md) for a quiet cloud "sandpit" recipe, build
flags, allocation profiling, and `perf` / VTune / Google Benchmark workflows.

## Documentation

* [`PROMPT.md`](PROMPT.md) — the full engineering specification this implements
  toward.
* [`EXAMPLES.md`](EXAMPLES.md) — annotated API usage examples.
* [`BENCHMARKING.md`](BENCHMARKING.md) — how to profile and benchmark properly.
* [`examples/`](examples) — complete, compiled example programs: LRU cache,
  order book, const-element safety, observer registry, symbol table,
  [insert diagnostics](examples/insert_diagnostics.cpp) (*which* index rejected
  an insert), [composite keys](examples/composite_keys.cpp) (full + prefix
  lookups), a [modern-API tour](examples/modern_api.cpp),
  [relational queries](examples/relational_queries.cpp) (`equi_join` / `group_by`
  / range scans), and an [observed table](examples/observed_table.cpp)
  (lifecycle observers + `std::format` introspection).
* [`playground/`](playground) — a Visual Studio project; open the `.sln`, press F5.

## Status & v1 limitations

This is an early implementation focused on a correct, broad feature surface.
Known limitations, slated for later work:

* **One tag per index** (Boost allows multiple); **stateless,
  default-constructible** comparators/hashers/extractors are assumed.
* **`modify()`** repositions only the indices whose key actually changed, so
  iterators into *unchanged* indices stay valid (as in Boost). The one
  unsupported combination is changing a **hashed** index's key on a
  **pointer-stored** element via `modify()` — use value storage or `replace()`.
* **Single-allocation:** all index kinds are intrusive (ordered/ranked AVL,
  hashed table, sequenced list) — one allocation per element, like Boost. Only
  random-access adds a pointer vector (so does Boost). `mic::pmr::multi_index_container`
  pools the element nodes + the hashed bucket / random-access arrays.
* Not yet implemented: serialization, the concurrent variants, and coroutine
  query helpers.

Internally each element lives in one stable node threaded into every index, so
iterators, pointers and references stay valid until the element is erased.

## License

MIT — see [`LICENSE`](LICENSE).
