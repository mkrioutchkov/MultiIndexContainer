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
* **Modern API** — `std::expected`-returning `try_insert`, `std::ranges`-ready
  index views (compose `views::filter` / `reverse` / `take`), `std::format`
  support, and `std::from_range` construction.

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

## Documentation

* [`PROMPT.md`](PROMPT.md) — the full engineering specification this implements
  toward.
* [`EXAMPLES.md`](EXAMPLES.md) — annotated API usage examples.
* [`examples/`](examples) — complete, compiled example programs
  (LRU cache, order book).

## Status & v1 limitations

This is an early implementation focused on a correct, broad feature surface.
Known limitations, slated for later work:

* **Ranked indices**: `rank()` / `nth()` are O(n) in v1 (a sorted std::set
  backs them); an order-statistics tree for O(log n) is planned.
* **Hashed lookup** materialises the key (no heterogeneous/transparent hashing
  yet); ordered lookup *is* transparent and zero-materialisation.
* **One tag per index** (Boost allows multiple); **stateless,
  default-constructible** comparators/hashers/extractors are assumed.
* Not yet implemented: serialization, `std::pmr` allocator plumbing for the
  index structures, the concurrent variants, and coroutine query helpers.

Internally each element lives in one stable node threaded into every index, so
iterators, pointers and references stay valid until the element is erased.

## License

MIT — see [`LICENSE`](LICENSE).
