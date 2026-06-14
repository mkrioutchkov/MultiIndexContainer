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

* **Ordered `find`** ≈ `std::set`/`std::map` find, plus one pointer indirection
  (node → value) and a transparent-comparator call. Expect within a small factor.
* **Hashed `find` is currently slower** than `std::unordered_map` — v1
  materialises the key on lookup (no heterogeneous hashing yet), so
  `find("literal")` constructs a `std::string`. This is the first thing to fix if
  hashed lookup is your hot path.
* **`build`** is competitive but does `1 + #indices` allocations/element — a
  pooling/`pmr` allocator (`mic::pmr` is planned) will move this a lot.
* **Iterate** is a linked-structure walk (node-based), so it's pointer-chasing,
  not contiguous — a `storage::flat` policy (planned) would help cache locality.
* **Ranked `nth`/`rank` are O(n)** in v1 (documented) — don't benchmark them as
  if they were O(log n); the order-statistics tree is the planned upgrade.

If a result contradicts the above, suspect the measurement (Debug build, shared
vCPU, optimiser deleting the work) before suspecting the library.
