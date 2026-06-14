// =============================================================================
//  examples/lru_cache.cpp
//
//  An LRU (least-recently-used) cache built on top of
//  mic::multi_index_container, demonstrating two cooperating indices:
//
//    * a `sequenced` index ("by_recency") that keeps elements in a doubly
//      linked list. We treat the FRONT as the most-recently-used slot and the
//      BACK as the least-recently-used (eviction) slot.
//
//    * a `hashed_unique` index ("by_key") that gives O(1) average lookup by key
//      and simultaneously enforces that a key appears at most once.
//
//  The two interesting operations are:
//
//    * promote-on-access: after a hit, we splice the touched node to the front
//      of the recency list. We find the node via the hash index, then use
//      project<"by_recency">() to obtain the SAME node's iterator in the
//      sequenced index in O(1) (no copy, no re-hash), and relocate() it to the
//      front. relocate() is a list splice -> O(1).
//
//    * eviction: when a put() pushes the size past capacity, the element at the
//      back of the recency list (the least-recently-used one) is erased from
//      EVERY index at once via the container-level erase().
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\lru_cache.cpp
//
//  This program prints what it does and asserts the invariants. It returns 0
//  on success (all asserts pass).
// =============================================================================

#include <cassert>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <mic/multi_index.hpp>

// The public namespace is already `mic` (defined by the header); no alias needed.

// -----------------------------------------------------------------------------
//  LruCache<Key, Val>
// -----------------------------------------------------------------------------
template <class Key, class Val>
class LruCache {
    // The element stored once and threaded into both indices.
    struct Entry {
        Key key;
        Val val;
    };

    // Two indices over Entry:
    //   index 0 / "by_recency"  : sequenced  (front = MRU, back = LRU)
    //   index 1 / "by_key"      : hashed_unique on Entry::key (O(1) lookup)
    using Store = mic::multi_index_container<Entry,
        mic::indexed_by<
            mic::sequenced     <"by_recency">,
            mic::hashed_unique <"by_key", mic::key<&Entry::key>>
        >>;

    Store       store_;
    std::size_t cap_;

public:
    explicit LruCache(std::size_t cap) : cap_(cap) {
        assert(cap_ > 0 && "capacity must be positive");
    }

    std::size_t size()     const { return store_.size(); }
    std::size_t capacity() const { return cap_; }

    // Look a key up. On a hit, promote the entry to most-recently-used and
    // return a pointer to its value; on a miss, return nullptr.
    //
    // Promotion is O(1): hash-find the node, project it into the recency index,
    // and splice it to the front.
    Val* get(const Key& k) {
        auto byKey = store_.template get<"by_key">();
        auto it = byKey.find(k);
        if (it == byKey.end())
            return nullptr;

        // Project the hashed-index iterator onto the sequenced index. Same
        // underlying node, O(1), no copy and no re-hash.
        auto seqIt = store_.template project<"by_recency">(it);

        auto bySeq = store_.template get<"by_recency">();
        // Splice the touched node to the front (most-recently-used). relocate()
        // moves the element pointed to by seqIt to just before begin().
        bySeq.relocate(bySeq.begin(), seqIt);

        // it->val is a const reference (indices expose const elements); the
        // value itself is logically mutable cache payload, so cast away const
        // to hand the caller a writable pointer. The key is NOT changed, so no
        // index invariant is touched.
        return const_cast<Val*>(&it->val);
    }

    // Insert or update. A brand-new key is pushed to the front (MRU). If the
    // key already exists, its value is updated in place and it is promoted.
    // After insertion, if we are over capacity, evict the least-recently-used
    // element (the back of the recency list).
    void put(const Key& k, const Val& v) {
        auto byKey = store_.template get<"by_key">();
        if (auto it = byKey.find(k); it != byKey.end()) {
            // Update existing value in place. The key does not change, so this
            // does not disturb the hashed index; modify() repositions across
            // all indices safely and returns true on success.
            bool ok = byKey.modify(it, [&](Entry& e) { e.val = v; });
            assert(ok && "value-only modify cannot collide");
            (void)ok;
            // Promote to most-recently-used.
            (void)get(k);
            return;
        }

        // New key: push to the front of the recency list. push_front goes
        // through the container's insert, so the hashed_unique index is
        // consulted; ok == false would mean a duplicate key (impossible here
        // because we just checked find()).
        auto bySeq = store_.template get<"by_recency">();
        auto [pos, ok] = bySeq.push_front(Entry{k, v});
        assert(ok && "key was absent, push_front must succeed");
        (void)pos;
        (void)ok;

        // Evict the least-recently-used element while over capacity. The back
        // of the recency list is the LRU slot. Container-level erase() unlinks
        // it from EVERY index at once.
        while (store_.size() > cap_) {
            auto seq = store_.template get<"by_recency">();
            auto last = seq.end();
            --last; // points at the LRU element
            store_.erase(store_.template project<"by_recency">(last));
        }
    }

    // True if the key is currently cached. Does NOT count as an access (no
    // promotion) so tests can probe recency without perturbing it.
    bool contains(const Key& k) const {
        // get<> is non-const on the container; take a fresh proxy off a
        // non-const reference to this->store_ for the read-only query.
        auto& s = const_cast<Store&>(store_);
        auto byKey = s.template get<"by_key">();
        return byKey.find(k) != byKey.end();
    }

    // Snapshot the keys from most- to least-recently-used. Handy for asserting
    // the recency order in tests.
    std::vector<Key> keys_mru_to_lru() const {
        std::vector<Key> out;
        auto& s = const_cast<Store&>(store_);
        for (const Entry& e : s.template get<"by_recency">())
            out.push_back(e.key);
        return out;
    }
};

// -----------------------------------------------------------------------------
//  Demonstration / self-test
// -----------------------------------------------------------------------------
namespace {

template <class Key>
void print_order(const char* label, const std::vector<Key>& ks) {
    std::cout << label << " [MRU..LRU]:";
    for (const auto& k : ks) std::cout << ' ' << k;
    std::cout << '\n';
}

} // namespace

int main() {
    std::cout << "== LRU cache on mic::multi_index_container ==\n";

    LruCache<std::string, int> cache(3);
    assert(cache.capacity() == 3);
    assert(cache.size() == 0);

    // --- fill to capacity ----------------------------------------------------
    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);
    assert(cache.size() == 3);

    // push_front means the most recently inserted key is at the front.
    // Order so far, MRU..LRU: c, b, a
    {
        auto order = cache.keys_mru_to_lru();
        print_order("after inserting a,b,c", order);
        assert((order == std::vector<std::string>{"c", "b", "a"}));
    }

    // --- promote-on-access ---------------------------------------------------
    // Touch "a": it must jump to the front. New order: a, c, b
    {
        int* pa = cache.get("a");
        assert(pa != nullptr && *pa == 1);
        auto order = cache.keys_mru_to_lru();
        print_order("after get(a)", order);
        assert((order == std::vector<std::string>{"a", "c", "b"}));
    }

    // A miss returns nullptr and does not change order.
    {
        int* miss = cache.get("zzz");
        assert(miss == nullptr);
        auto order = cache.keys_mru_to_lru();
        assert((order == std::vector<std::string>{"a", "c", "b"}));
    }

    // --- eviction of the least-recently-used --------------------------------
    // Current MRU..LRU: a, c, b  -> "b" is the LRU.
    // Inserting a new key "d" overflows capacity (3) and must evict "b".
    {
        cache.put("d", 4);
        assert(cache.size() == 3);
        assert(!cache.contains("b") && "LRU 'b' should have been evicted");
        assert(cache.contains("a"));
        assert(cache.contains("c"));
        assert(cache.contains("d"));
        auto order = cache.keys_mru_to_lru();
        print_order("after put(d) (evicts b)", order);
        // d freshly pushed to front; a was MRU before, c after it.
        assert((order == std::vector<std::string>{"d", "a", "c"}));
    }

    // --- update existing key in place (no growth, value changes, promotes) ---
    {
        cache.put("c", 30); // c exists -> update value + promote, size unchanged
        assert(cache.size() == 3);
        int* pc = cache.get("c");
        assert(pc != nullptr && *pc == 30);
        // After put(c) it was promoted to front, then get(c) keeps it at front.
        auto order = cache.keys_mru_to_lru();
        print_order("after put(c,30)", order);
        assert(order.front() == "c");
    }

    // --- writable payload via get() -----------------------------------------
    {
        int* pd = cache.get("d");
        assert(pd != nullptr);
        *pd = 400;                 // mutate cached value through the pointer
        int* pd2 = cache.get("d"); // re-fetch
        assert(pd2 != nullptr && *pd2 == 400);
    }

    // --- a small stress loop: never exceeds capacity ------------------------
    {
        LruCache<int, int> nums(4);
        for (int i = 0; i < 100; ++i) {
            nums.put(i, i * i);
            assert(nums.size() <= 4);
        }
        // Only the last 4 inserted keys (96..99) can remain.
        assert(nums.size() == 4);
        for (int i = 0; i < 96; ++i) assert(!nums.contains(i));
        for (int i = 96; i < 100; ++i) {
            assert(nums.contains(i));
            int* p = nums.get(i);
            assert(p != nullptr && *p == i * i);
        }
        auto order = nums.keys_mru_to_lru();
        // The last get() touched 99 last, so 99 is MRU.
        assert(order.size() == 4);
        assert(order.front() == 99);
    }

    std::cout << "All LRU cache assertions passed.\n";
    return 0;
}
