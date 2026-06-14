// =============================================================================
//  examples/order_book.cpp
//  A price-time-priority limit order book built on mic::multi_index_container.
//
//  Two indices over the same Order element:
//    * hashed_unique     <"by_id">      -> O(1) average cancel / lookup by order id
//    * ordered_non_unique<"by_px_time"> -> composite (price, timestamp) key giving
//                                          price-time priority for free
//
//  For a BID book, the best order is the highest price and, among equal prices,
//  the earliest timestamp. The composite key sorts ASCENDING by (price, ts), so
//  the last element carries the highest price, while within a price group the
//  earliest ts sorts first. best_bid() therefore reads the top price off the
//  last element, then lower_bounds to the earliest-ts order at that price.
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\order_book.cpp
//
//  Demonstrates: adding orders, finding the best bid, cancelling by id, and
//  partially filling an order via modify().
// =============================================================================

#include <mic/multi_index.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <tuple>

// -----------------------------------------------------------------------------
//  The element stored in the book.
// -----------------------------------------------------------------------------
struct Order {
    std::uint64_t id;     // unique order id
    char          side;   // 'B' (buy/bid) or 'S' (sell/ask)
    std::int64_t  price;  // limit price in ticks
    std::uint64_t ts;     // arrival timestamp == time priority
    std::uint32_t qty;    // remaining quantity
};

// -----------------------------------------------------------------------------
//  The book: id index for cancel/lookup, (price, ts) index for priority.
// -----------------------------------------------------------------------------
using Book = mic::multi_index_container<Order,
    mic::indexed_by<
        mic::hashed_unique     <"by_id",      mic::key<&Order::id>>,
        mic::ordered_non_unique<"by_px_time", mic::key<&Order::price, &Order::ts>>
    >>;

// Best bid = highest price, and within that price the earliest (smallest) ts.
//
// The composite key sorts ASCENDING by (price, ts), so:
//   * the highest price is whatever the last element carries; and
//   * within a price group the earliest ts sorts FIRST.
// So: read the highest price off the last element, then lower_bound to the
// first (earliest-ts) order at that price. lower_bound on the partial tuple
// (price, 0) lands on the first entry whose key is >= (price, 0), i.e. the
// earliest-ts order at that price.
static const Order& best_bid(Book& book) {
    auto px = book.get<"by_px_time">();
    assert(!px.empty());
    auto last = px.end();
    --last;                                         // highest (price, ts)
    const std::int64_t top_price = last->price;     // highest price level
    auto it = px.lower_bound(std::tuple{top_price, std::uint64_t{0}});
    assert(it != px.end());
    return *it;                                     // earliest ts at top price
}

int main() {
    Book bids;

    // ---- adding orders -----------------------------------------------------
    // Three resting bids. Note ids 1 and 2 share a price (10'100): time priority
    // breaks the tie in favour of the earlier timestamp (ts = 1).
    bids.insert(Order{.id = 1, .side = 'B', .price = 10'100, .ts = 1, .qty = 50});
    bids.insert(Order{.id = 2, .side = 'B', .price = 10'100, .ts = 2, .qty = 30});
    bids.insert(Order{.id = 3, .side = 'B', .price = 10'050, .ts = 1, .qty = 10});

    assert(bids.size() == 3);
    std::cout << "Inserted " << bids.size() << " orders.\n";

    // A duplicate id is rejected by the hashed_unique index; the rest of the
    // book is untouched.
    auto [dupIt, dupOk] = bids.insert(
        Order{.id = 1, .side = 'B', .price = 9'999, .ts = 9, .qty = 1});
    assert(!dupOk);
    (void)dupIt;
    assert(bids.size() == 3);

    // ---- finding the best bid ----------------------------------------------
    // Highest price is 10'100; the earliest of the two 10'100 orders is id 1.
    {
        const Order& best = best_bid(bids);
        std::cout << "Best bid: id=" << best.id
                  << " price=" << best.price
                  << " ts=" << best.ts
                  << " qty=" << best.qty << '\n';
        assert(best.id == 1);
        assert(best.price == 10'100);
        assert(best.ts == 1);
    }

    // O(1)-average lookup by id through the hashed index.
    {
        auto byId = bids.get<"by_id">();
        assert(byId.contains(2));
        assert(byId.count(2) == 1);
        auto f = byId.find(3);
        assert(f != byId.end());
        assert(f->price == 10'050);
        std::cout << "Lookup id=3 -> price=" << f->price << '\n';
    }

    // ---- cancelling by id --------------------------------------------------
    // Cancel order 1 (the current best) in O(1) average via the id index.
    {
        auto byId = bids.get<"by_id">();
        auto it = byId.find(1);
        assert(it != byId.end());
        byId.erase(it);
        assert(bids.size() == 2);
        assert(!byId.contains(1));
        std::cout << "Cancelled id=1; size now " << bids.size() << '\n';

        // With id 1 gone, the best bid is now id 2 (same price, next-earliest).
        const Order& best = best_bid(bids);
        assert(best.id == 2);
        assert(best.price == 10'100);
        std::cout << "New best bid: id=" << best.id << '\n';
    }

    // erase_key on the id index is an equivalent one-shot cancel-by-key.
    {
        auto byId = bids.get<"by_id">();
        std::size_t removed = byId.erase_key(3);
        assert(removed == 1);
        assert(bids.size() == 1);
        std::cout << "Cancel-by-key id=3 removed " << removed << " order.\n";
    }

    // ---- partially filling an order via modify() ---------------------------
    // A 20-lot incoming sell hits resting bid id 2 (currently 30). Decrement the
    // remaining quantity in place. qty is not part of any index key, so modify()
    // succeeds without repositioning and never collides.
    {
        auto byId = bids.get<"by_id">();
        auto it = byId.find(2);
        assert(it != byId.end());
        assert(it->qty == 30);

        bool ok = byId.modify(it, [](Order& o) { o.qty -= 20; });
        assert(ok);
        assert(it->qty == 10);
        std::cout << "Partial fill: id=2 remaining qty=" << it->qty << '\n';

        // The order still lives in both indices and keeps its priority position.
        const Order& best = best_bid(bids);
        assert(best.id == 2);
        assert(best.qty == 10);
        assert(bids.size() == 1);
    }

    // A full fill drains the remaining quantity and cancels the order.
    {
        auto byId = bids.get<"by_id">();
        auto it = byId.find(2);
        assert(it != byId.end());

        const std::uint32_t incoming = 10;     // exactly the remaining qty
        if (it->qty <= incoming) {
            byId.erase(it);                     // fully filled -> remove
        } else {
            byId.modify(it, [&](Order& o) { o.qty -= incoming; });
        }
        assert(bids.empty());
        std::cout << "Full fill: id=2 removed; book empty.\n";
    }

    std::cout << "All order-book assertions passed.\n";
    return 0;
}
