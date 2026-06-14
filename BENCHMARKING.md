# Benchmarking & profiling `mic`

Short version: **don't benchmark on your laptop.** Laptops thermally throttle,
boost/de-boost unpredictably, park cores, and run background work — you'll get
20–50% run-to-run noise that swamps the differences you care about. Use a quiet,
fixed-frequency machine (a cloud "sandpit"), pin the clock, isolate a core, and
take the *minimum* of many runs.

This repo ships two things:

* [`benchmarks/bench.cpp`](benchmarks/bench.cpp) — a zero-dependency `std::chrono`
  harness comparing `mic` against the hand-rolled std-container composition you'd
  otherwise write (and against Boost.MultiIndex if its headers are on the include
  path). Good for a quick first look; runs anywhere.
* This guide — how to get numbers you can actually trust, and how to find *why*
  something is slow.

---

## 0. Online sandpits (incl. vs Boost)

You don't need to own a machine. These all run Boost.MultiIndex too — the
benchmark adds a Boost column automatically when `libboost-dev` is installed
(`#if __has_include(<boost/multi_index_container.hpp>)`).

| Sandpit | How | Boost | Profiling | Best for |
|---|---|---|---|---|
| **GitHub Actions** (this repo) | Actions tab → **benchmark** → *Run workflow* | yes (auto) | no (just timings) | one-click mic-vs-Boost numbers + a GCC parity check |
| **GitHub Codespaces** (this repo) | green *Code* → *Codespaces* → create | `apt install libboost-dev` (the [devcontainer](.devcontainer/devcontainer.json) does it for you) | callgrind / cachegrind / Google Benchmark (hardware `perf` usually blocked in the container) | interactive hacking + deterministic profiling |
| **Oracle Cloud Always Free (Ampere A1)** | free aarch64 VM, up to 4 cores | `apt install libboost-dev` | full `perf` + VTune-equivalents (real VM) | a free, dedicated-ish box (ARM, so note the arch) |
| **Compiler Explorer** ([godbolt.org](https://godbolt.org)) | paste a snippet, add Boost from the libraries dropdown | yes | asm + opt-remarks, not throughput | comparing the *generated code* of a mic op vs the Boost op |
| **Hetzner CCX / AWS c7i** by the hour | spin up, run, destroy (pennies) | `apt install libboost-dev` | full `perf` + VTune | trustworthy **x86 absolute** numbers |

**Caveat that matters:** Actions and Codespaces are *shared* hosts, so absolute
ns/op is noisy. But comparing **mic vs Boost vs std within one run** is fair — they
all suffer the same noise — so those *relative* ratios are trustworthy. For
absolute headline throughput, use a dedicated box (§1) or `callgrind` (which
counts instructions, not wall-clock, so it's immune to host noise).

**One-click recipes for this repo:**

* *GitHub Actions:* Actions tab → **benchmark** → *Run workflow* → enter sizes →
  read the log / download the `bench-results` artifact. (Also runs the suite and
  examples on GCC 14, telling us if anything is non-GCC-clean.)
* *Codespaces:* open a Codespace (the devcontainer pre-installs GCC 14 + Boost +
  valgrind), then:
  ```bash
  g++-14 -std=c++23 -O3 -DNDEBUG -I include benchmarks/bench.cpp -o bench
  ./bench 100000 1000000                       # Boost column included
  valgrind --tool=callgrind ./bench 10000      # deterministic profile, host-noise-proof
  callgrind_annotate callgrind.out.*           # per-function instruction counts
  ```

> The library is developed on MSVC; **GCC/Clang are a parity goal, not yet
> CI-proven** — the Actions `parity` job is exactly how we find and fix any
> GCC-specific nits. If it's red, that's information, not alarm.

---

## 1. Where to run it (the "sandpit")

You want a dedicated CPU, not a shared/burstable one.

| Option | Good for | Notes |
|---|---|---|
| **Bare-metal / dedicated-vCPU cloud** (AWS `c7i`/`c7a`/`*.metal`, Hetzner `CCX` dedicated, GCP `c3`, Azure `Fsv2`) | trustworthy numbers | Pick a **dedicated** vCPU SKU. Avoid burstable `t3`/`t4g`, shared-core `e2`, etc. |
| **GitHub Codespaces** | convenient, repeatable | Shared host → noisier; fine for relative regressions, not for headline figures. |
| **GitHub Actions** | CI regression gate | Very noisy; only trust large deltas, always compare within the same run. |
| **A spare physical box** | best of all | If you have one idle, it beats most cloud instances. |

Cheapest "good enough": a Hetzner **CCX13/CCX23** (dedicated vCPU) or an AWS
**`c7i.large`/`c7i.xlarge`** spun up for an hour. Tear it down after.

### One-shot Linux sandpit recipe

```bash
# fresh Ubuntu 24.04 instance
sudo apt update && sudo apt install -y build-essential clang-18 cmake linux-tools-common linux-tools-$(uname -r) git
git clone https://github.com/mkrioutchkov/MultiIndexContainer.git && cd MultiIndexContainer

# --- pin the machine so timings are stable ---
sudo cpupower frequency-set -g performance              # no on-demand scaling
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost # disable turbo (Intel: intel_pstate/no_turbo)
# isolate core 3 for the benchmark (best: add `isolcpus=3 nohz_full=3` to the kernel cmdline)

clang++-18 -std=c++23 -O3 -DNDEBUG -march=native -I include benchmarks/bench.cpp -o bench
taskset -c 3 nice -n -20 ./bench 100000 1000000
```

`taskset -c 3` pins to one isolated core; `nice -n -20` raises priority. Run it a
few times — the harness already reports the *best* of its internal repetitions,
which is what you want (the minimum is the run least disturbed by the OS).

---

## 2. Get the build right first

Wrong build flags make benchmarks meaningless:

* **Optimise:** `-O3 -DNDEBUG` (clang/gcc) or `/O2 /DNDEBUG` (MSVC). `-DNDEBUG`
  drops `assert`s; on MSVC it also avoids the debug iterator checks.
* **Never benchmark a Debug build.** MSVC Debug sets `_ITERATOR_DEBUG_LEVEL=2`
  (checked iterators) and uses the debug CRT — easily 10× slower and not
  representative.
* Consider `-march=native` (clang/gcc) / `/arch:AVX2` (MSVC) and LTO
  (`-flto` / `/GL /LTCG`) if that matches how you'll ship.
* Match the standard library you ship with (libstdc++ vs libc++ vs MSVC STL) —
  their `std::set`/`std::unordered_set` differ noticeably.

---

## 3. Measure the right things

The harness covers `build`, `find(id)`, `find(email)`, `iterate`. When you extend
it, keep these honest:

* **ns per element**, not total — so numbers compare across `N`.
* **Sweep `N`** across cache regimes: ~10³ (L1/L2), ~10⁵ (L3), ~10⁶–10⁷ (RAM).
  Relative performance changes a lot between them.
* **Key distributions matter** for hashed indices — add an adversarial /
  high-collision set, not just uniform random.
* **Prevent the optimiser** from deleting the work: accumulate into a `volatile`
  sink (the harness does this) or use `DoNotOptimize` (Google Benchmark).
* **Take the minimum**, report median+IQR if you want spread. Never the mean of a
  noisy set.
* **Warm up** (the first timed pass pays page-fault/cache cost) — the harness's
  repetition loop handles this.

### Memory / allocations — the metric people forget

`mic` v1 stores each element in one node **and** keeps a separate `std::set` /
`std::unordered_set` node per index, so it does roughly `1 + (#keyed indices)`
allocations per element. That's a real cost vs a single-allocation intrusive
design. Measure it:

* **Linux:** `heaptrack ./bench` then `heaptrack_gui` (or `heaptrack_print`) —
  shows allocation count, bytes, and call sites. `valgrind --tool=massif` for a
  heap profile over time.
* **Counting allocator:** swap a small instrumented `std::allocator` that bumps a
  global counter into the container and print allocs/element.
* **MSVC:** the CRT debug heap (`_CrtMemCheckpoint`/`_CrtMemDifference`) or VS's
  "Memory Usage" diagnostic tool.

---

## 4. Profile *why* it's slow

Benchmarks tell you *what* is slow; profilers tell you *where*.

**Linux (best toolchain):**
```bash
# what's hot — sampling profiler, near-zero overhead
perf record -g --call-graph=dwarf taskset -c 3 ./bench 1000000
perf report                       # interactive; or pipe to a flame graph

# counters: cache misses, branch mispredicts, IPC
perf stat -d ./bench 1000000

# exact instruction counts & cache model (slow but deterministic — great for
# tiny deltas that perf's sampling can't resolve)
valgrind --tool=callgrind ./bench 10000 && kcachegrind callgrind.out.*
valgrind --tool=cachegrind ./bench 10000
```
Flame graphs: `perf script | stackcollapse-perf.pl | flamegraph.pl > out.svg`
(Brendan Gregg's FlameGraph scripts).

**Windows / MSVC:**
* **Visual Studio Profiler** (Debug ▸ Performance Profiler, or `Alt+F2`) — CPU
  Usage + Instrumentation; build **Release** first. This is the easiest path on
  your current setup once you move to a quiet machine.
* **Intel VTune** (free) — far deeper: microarchitecture analysis, memory access,
  hotspots. Best-in-class for "why is this cache-bound".
* **Windows Performance Analyzer (WPA/xperf)** for system-wide traces.

**Cross-platform:** [Tracy](https://github.com/wolfpld/tracy) for frame/zone-level
instrumentation if you want to annotate specific operations.

---

## 5. Rigorous microbenchmarks: Google Benchmark

For numbers you'd publish, graduate from `bench.cpp` to
[Google Benchmark](https://github.com/google/benchmark) — it auto-tunes iteration
counts, reports stddev, supports `DoNotOptimize`/`ClobberMemory`, and emits JSON
for tracking over time.

```cmake
# add to your build (needs network at configure time)
include(FetchContent)
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git GIT_TAG v1.9.1)
set(BENCHMARK_ENABLE_TESTING OFF)
FetchContent_MakeAvailable(benchmark)
target_link_libraries(my_bench PRIVATE mic::mic benchmark::benchmark)
```

```cpp
static void BM_FindById(benchmark::State& state) {
    auto data = make_data(state.range(0));
    MicTable t; for (auto& r : data) t.insert(r);
    for (auto _ : state)
        for (auto& r : data)
            benchmark::DoNotOptimize(t.get<"by_id">().find(r.id)->payload);
}
BENCHMARK(BM_FindById)->RangeMultiplier(10)->Range(1000, 1'000'000);
```

Run with `--benchmark_repetitions=10 --benchmark_report_aggregates_only=true`
and pin the process as in §1. Emit `--benchmark_out=results.json` and diff with
`compare.py` across commits to catch regressions.

---

## 6. What to expect (known v1 characteristics)

So you can sanity-check your results against the implementation:

* **Ordered `find`** ≈ `std::set`/`std::map` find. Small trivially-copyable keys
  (e.g. `int`) are stored inline in the index node, so the comparison reads the
  key directly — `find(id)` is at or slightly above `std::map`. String/large keys
  keep pointer storage (one extra indirection per compare).
* **Hashed `find` is transparent** — string keys hash through `string_view`, so
  `find("literal")` / `find(string_view)` does no key materialisation; expect it
  near `std::unordered_map`. (A non-transparent *user-supplied* hasher falls back
  to materialising the key.)
* **`build`** does `1 + #indices` allocations/element. With the default allocator
  it's competitive with a hand-rolled std composition; with
  `mic::pmr::multi_index_container` + a `monotonic_buffer_resource` it is roughly
  **1.5–1.7× faster** (one arena serves every node and index structure).
* **Iterate** is a linked-structure walk (node-based), so it's pointer-chasing,
  not contiguous — a `storage::flat` policy (planned) would help cache locality.
* **Ranked `nth`/`rank` are O(log n)** — backed by a size-augmented
  order-statistics tree (a randomised treap), so `nth(k)` stays in the hundreds
  of nanoseconds even at N = 10⁶ (it should grow log-shaped, not linearly).

If a result contradicts the above, suspect the measurement (Debug build, shared
vCPU, optimiser deleting the work) before suspecting the library.
