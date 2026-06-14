// =============================================================================
//  examples/leaderboard.cpp
//
//  A game leaderboard built on a ranked index — order statistics for free.
//
//    * ranked_non_unique<"by_score">  -> a size-augmented order-statistics tree:
//        nth(k)  = the k-th lowest score          (O(log n))
//        rank(it)= how many scores are below `it` (O(log n))
//    * ordered_unique<"by_id">        -> look a player up by id
//
//  From those two operations you get top-N, "what place am I?", the median, and
//  percentiles — with no sorting loops and no recomputation on every update.
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\leaderboard.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <cassert>
#include <print>
#include <ranges>
#include <string>

struct Player {
    int         id;
    std::string name;
    int         score;
};

using Leaderboard = mic::multi_index<Player,
    mic::indexed_by<
        mic::ranked_non_unique<"by_score", mic::key<&Player::score>>,
        mic::ordered_unique   <"by_id",    mic::key<&Player::id>>
    >>;

int main() {
    Leaderboard lb;
    lb.insert({1, "Ada",   9420});
    lb.insert({2, "Bjarne",11830});
    lb.insert({3, "Carol", 11830});   // tie with Bjarne
    lb.insert({4, "Dan",   6100});
    lb.insert({5, "Eve",   14210});
    lb.insert({6, "Finn",  7700});
    lb.insert({7, "Grace", 15990});

    auto by_score = lb.get<"by_score">();
    const std::size_t N = lb.size();

    // ---- top 3: highest scores first --------------------------------------
    std::println("Top 3:");
    int place = 1;
    for (const Player& p : by_score | std::views::reverse | std::views::take(3))
        std::println("  #{}  {:>6} pts  {}", place++, p.score, p.name);

    // ---- "what place am I?" in O(log n) -----------------------------------
    // rank(it) counts how many sit *below* it; flip it to count from the top.
    {
        auto ada_by_score = lb.project<"by_score">(lb.get<"by_id">().find(1));
        std::size_t from_top = N - by_score.rank(ada_by_score);   // 1-based from the top
        std::println("\nAda has {} pts and is in #{} place of {}.",
                     ada_by_score->score, from_top, N);
        // above Ada: Grace, Eve, Carol, Bjarne -> Ada is 5th from the top
        assert(from_top == 5);
    }

    // ---- order statistics: lowest, highest, median ------------------------
    std::println("\nlowest  : {} pts ({})", by_score.nth(0)->score,     by_score.nth(0)->name);
    std::println("highest : {} pts ({})", by_score.nth(N - 1)->score, by_score.nth(N - 1)->name);
    std::println("median  : {} pts ({})", by_score.nth(N / 2)->score, by_score.nth(N / 2)->name);

    // ---- a score update re-ranks in place ---------------------------------
    // Dan grinds from 6100 to 12000; modify() repositions him in by_score only.
    lb.get<"by_id">().modify(lb.get<"by_id">().find(4), [](Player& p) { p.score = 12000; });
    {
        auto dan = lb.project<"by_score">(lb.get<"by_id">().find(4));
        std::println("\nAfter Dan's run to {} pts he is #{} of {}.",
                     dan->score, N - by_score.rank(dan), N);
    }

    // ---- top-half cutoff via nth() ----------------------------------------
    int cutoff = by_score.nth(N / 2)->score;
    std::size_t in_top_half = 0;
    for (const Player& p : by_score)
        if (p.score >= cutoff) ++in_top_half;
    std::println("\n{} players are at or above the median ({} pts).", in_top_half, cutoff);

    std::println("\nAll leaderboard assertions passed.");
    return 0;
}
