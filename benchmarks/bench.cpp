// =============================================================================
//  benchmarks/bench.cpp
//
//  A self-contained, dependency-free micro-benchmark for mic::multi_index_container.
//  It compares mic against the realistic alternative you'd hand-roll WITHOUT a
//  multi-index library: a stable element store plus one std:: associative
//  container per index, kept in sync by hand. If Boost.MultiIndex headers are on
//  the include path, a Boost column is added automatically.
//
//  Build for real numbers with optimisation ON, e.g. (MSVC):
//      cl /std:c++latest /O2 /DNDEBUG /EHsc /I include benchmarks\bench.cpp
//  or (clang/gcc):
//      clang++ -std=c++23 -O3 -DNDEBUG -I include benchmarks/bench.cpp -o bench
//
//  Optional args:  bench [N1 N2 ...]      (element counts; default 10000 100000)
//
//  IMPORTANT: this is a quick first-look harness. For publishable numbers use a
//  quiet, fixed-frequency machine and a real framework (Google Benchmark) — see
//  BENCHMARKING.md.
// =============================================================================

#include <mic/multi_index.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory_resource>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#if __has_include(<boost/multi_index_container.hpp>)
#  include <boost/multi_index_container.hpp>
#  include <boost/multi_index/ordered_index.hpp>
#  include <boost/multi_index/hashed_index.hpp>
#  include <boost/multi_index/member.hpp>
#  define MIC_HAVE_BOOST 1
#endif

namespace {

struct Record {
    int          id;
    std::string  name;
    std::string  email;
    std::int64_t payload;
};

// ---- mic container ---------------------------------------------------------
using MicIndices = mic::indexed_by<
        mic::ordered_unique     <"by_id",    mic::key<&Record::id>>,
        mic::ordered_non_unique <"by_name",  mic::key<&Record::name>>,
        mic::hashed_unique      <"by_email", mic::key<&Record::email>>
    >;
using MicTable    = mic::multi_index_container<Record, MicIndices>;
using PmrMicTable = mic::pmr::multi_index_container<Record, MicIndices>;

// ---- hand-rolled std composition (the thing you'd write without a lib) ------
struct StdTable {
    std::deque<Record>                            store;   // stable addresses
    std::map<int, Record*>                        by_id;
    std::multimap<std::string, Record*>           by_name;
    std::unordered_map<std::string, Record*>      by_email;

    bool insert(const Record& r) {
        if (by_id.contains(r.id) || by_email.contains(r.email)) return false;
        store.push_back(r);
        Record* p = &store.back();
        by_id.emplace(p->id, p);
        by_name.emplace(p->name, p);
        by_email.emplace(p->email, p);
        return true;
    }
};

#if MIC_HAVE_BOOST
namespace bmi = boost::multi_index;
using BoostTable = boost::multi_index_container<Record,
    bmi::indexed_by<
        bmi::ordered_unique     <bmi::tag<struct b_id>,    bmi::member<Record, int,         &Record::id>>,
        bmi::ordered_non_unique <bmi::tag<struct b_name>,  bmi::member<Record, std::string, &Record::name>>,
        bmi::hashed_unique      <bmi::tag<struct b_email>, bmi::member<Record, std::string, &Record::email>>
    >>;
#endif

// ---- timing ----------------------------------------------------------------
using clk = std::chrono::steady_clock;
[[maybe_unused]] volatile std::int64_t g_sink = 0;

template <class F>
double best_ns(F&& f, int reps) {
    double best = 1e300;
    for (int i = 0; i < reps; ++i) {
        auto t0 = clk::now();
        f();
        auto t1 = clk::now();
        best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    return best;
}

std::vector<Record> make_data(std::size_t n) {
    std::mt19937_64 rng(0xC0FFEE);
    std::vector<int> ids(n);
    for (std::size_t i = 0; i < n; ++i) ids[i] = static_cast<int>(i);
    std::shuffle(ids.begin(), ids.end(), rng);
    std::vector<Record> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        v.push_back(Record{ ids[i],
                            "name" + std::to_string(ids[i] % 4096),
                            "user" + std::to_string(ids[i]) + "@example.com",
                            static_cast<std::int64_t>(rng()) });
    return v;
}

void row(const char* op, double mic, double stdc, double boost, std::size_t n) {
    auto perop = [&](double total) { return total / static_cast<double>(n); };
    std::printf("  %-12s  %10.1f  %10.1f", op, perop(mic), perop(stdc));
    if (boost > 0) std::printf("  %10.1f", perop(boost)); else std::printf("  %10s", "-");
    std::printf("   (x%.2f vs std)\n", stdc / mic);
}

void run(std::size_t n) {
    auto data = make_data(n);
    const int rd_reps = 5;

    std::printf("\nN = %zu\n", n);
    std::printf("  %-12s  %10s  %10s  %10s\n", "op (ns/elem)", "mic", "std-hand", "boost");

    // build (fresh each rep)
    double mic_build = best_ns([&] { MicTable t; for (auto& r : data) t.insert(r); }, 3);
    double std_build = best_ns([&] { StdTable t; for (auto& r : data) t.insert(r); }, 3);
    double boost_build = -1;
#if MIC_HAVE_BOOST
    boost_build = best_ns([&] { BoostTable t; for (auto& r : data) t.insert(r); }, 3);
#endif

    // persistent instances for read tests
    MicTable mic; for (auto& r : data) mic.insert(r);
    StdTable stb; for (auto& r : data) stb.insert(r);
#if MIC_HAVE_BOOST
    BoostTable bt; for (auto& r : data) bt.insert(r);
#endif

    double mic_findid = best_ns([&] { std::int64_t s = 0; for (auto& r : data) s += mic.get<"by_id">().find(r.id)->payload; g_sink += s; }, rd_reps);
    double std_findid = best_ns([&] { std::int64_t s = 0; for (auto& r : data) s += stb.by_id.find(r.id)->second->payload; g_sink += s; }, rd_reps);
    double boost_findid = -1;
#if MIC_HAVE_BOOST
    boost_findid = best_ns([&] { std::int64_t s = 0; auto& ix = bt.get<b_id>(); for (auto& r : data) s += ix.find(r.id)->payload; g_sink += s; }, rd_reps);
#endif

    double mic_findem = best_ns([&] { std::int64_t s = 0; for (auto& r : data) s += mic.get<"by_email">().find(r.email)->payload; g_sink += s; }, rd_reps);
    double std_findem = best_ns([&] { std::int64_t s = 0; for (auto& r : data) s += stb.by_email.find(r.email)->second->payload; g_sink += s; }, rd_reps);
    double boost_findem = -1;
#if MIC_HAVE_BOOST
    boost_findem = best_ns([&] { std::int64_t s = 0; auto& ix = bt.get<b_email>(); for (auto& r : data) s += ix.find(r.email)->payload; g_sink += s; }, rd_reps);
#endif

    double mic_iter = best_ns([&] { std::int64_t s = 0; for (const Record& r : mic.get<"by_id">()) s += r.payload; g_sink += s; }, rd_reps);
    double std_iter = best_ns([&] { std::int64_t s = 0; for (auto& kv : stb.by_id) s += kv.second->payload; g_sink += s; }, rd_reps);
    double boost_iter = -1;
#if MIC_HAVE_BOOST
    boost_iter = best_ns([&] { std::int64_t s = 0; for (const Record& r : bt.get<b_id>()) s += r.payload; g_sink += s; }, rd_reps);
#endif

    row("build",      mic_build,  std_build,  boost_build,  n);
    row("find(id)",   mic_findid, std_findid, boost_findid, n);
    row("find(email)",mic_findem, std_findem, boost_findem, n);
    row("iterate",    mic_iter,   std_iter,   boost_iter,   n);

    // mic with a pooling allocator: all node/index allocations come from one arena.
    double mic_pmr_build = best_ns([&] {
        std::pmr::monotonic_buffer_resource res(std::size_t{1} << 20);
        PmrMicTable t(&res);
        for (auto& r : data) t.insert(r);
    }, 3);
    std::printf("  %-12s  %10.1f   (mic + monotonic_buffer_resource; x%.2f vs mic default alloc)\n",
                "build(pmr)", mic_pmr_build / static_cast<double>(n), mic_build / mic_pmr_build);
}

} // namespace

int main(int argc, char** argv) {
#if MIC_HAVE_BOOST
    std::puts("mic micro-benchmark  (Boost.MultiIndex column ENABLED)");
#else
    std::puts("mic micro-benchmark  (Boost not found; build with Boost on the include path to add a column)");
#endif
    std::vector<std::size_t> sizes;
    for (int i = 1; i < argc; ++i) sizes.push_back(std::strtoull(argv[i], nullptr, 10));
    if (sizes.empty()) sizes = { 10'000, 100'000 };
    for (std::size_t n : sizes) run(n);
    std::printf("\n(ns per element; lower is better. sink=%lld)\n", static_cast<long long>(g_sink));
    return 0;
}
