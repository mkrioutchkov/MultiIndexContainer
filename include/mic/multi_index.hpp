// =============================================================================
//  mic::multi_index_container
//  A greenfield C++23 multi-index container in the spirit of Boost.MultiIndex,
//  designed as a functional *superset*: member / member-function-pointer key
//  extraction, global-function keys, composite keys, identity, ordered/hashed/
//  sequenced/random-access/ranked indices, unique & non-unique constraints,
//  string-literal tags (enforced) plus Boost-style numeric get<N>(), automatic
//  dereference through raw pointers, std::shared_ptr / std::unique_ptr and
//  std::reference_wrapper, projection between indices, node-handle transfer,
//  modify / replace with rollback-and-keep semantics, and std::ranges support.
//
//  Header-only. Everything lives in namespace `mic`. Requires /std:c++latest
//  (tested on MSVC 14.51). Single-threaded; concurrent reads are safe, writes
//  require external synchronisation (same contract as the std:: containers).
//
//  NOTE ON INTERNALS: each element lives in one stable, single allocation; every
//  index's linkage is *intrusive* — stored inside the element node at
//  std::get<I>(node->hooks) — so element addresses and iterators stay valid until
//  the element is erased. Ordered/ranked indices are a size-augmented AVL tree
//  (balanced like std::set; subtree sizes give O(log n) nth()/rank()), hashed
//  indices are a bucketed hash table (chains threaded through the node; only the
//  bucket array allocates), sequenced is an intrusive doubly-linked list, and
//  the "all live nodes" registry is intrusive too. Only random-access keeps a
//  pointer vector (as Boost.MultiIndex does). Net result: a container of
//  ordered/ranked/hashed/sequenced indices does ONE allocation per element.
// =============================================================================
#ifndef MIC_MULTI_INDEX_HPP
#define MIC_MULTI_INDEX_HPP

#include <vector>
#include <tuple>
#include <array>
#include <memory>
#include <memory_resource>
#include <functional>
#include <utility>
#include <type_traits>
#include <concepts>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <string_view>
#include <variant>
#include <compare>
#include <format>
#include <string>
#include <expected>

#if defined(_MSC_VER)
#  define MIC_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#  define MIC_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

namespace mic {

// ---------------------------------------------------------------------------
//  fixed_string : structural NTTP usable as an index tag, e.g. ordered_unique<"by_id", ...>
// ---------------------------------------------------------------------------
template <std::size_t N>
struct fixed_string {
    char data[N]{};
    constexpr fixed_string(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) data[i] = s[i];
    }
    constexpr std::string_view view() const noexcept { return std::string_view(data, N - 1); }
    constexpr std::size_t size() const noexcept { return N - 1; }
};
template <std::size_t N, std::size_t M>
constexpr bool operator==(const fixed_string<N>& a, const fixed_string<M>& b) noexcept {
    return a.view() == b.view();
}

// ---------------------------------------------------------------------------
//  detail : pointer / wrapper unwrapping so extractors transparently reach
//           through T*, std::shared_ptr<T>, std::unique_ptr<T>,
//           std::reference_wrapper<T> and arbitrary chains thereof.
// ---------------------------------------------------------------------------
namespace detail {

template <class T> struct is_reference_wrapper : std::false_type {};
template <class T> struct is_reference_wrapper<std::reference_wrapper<T>> : std::true_type {};

template <class Class, class X>
constexpr const Class& to_ref(const X& x) {
    using U = std::remove_cvref_t<X>;
    if constexpr (std::is_same_v<U, Class> || std::is_base_of_v<Class, U>) {
        return x;
    } else if constexpr (is_reference_wrapper<U>::value) {
        return to_ref<Class>(x.get());
    } else {
        return to_ref<Class>(*x); // raw ptr / shared_ptr / unique_ptr / iterator ...
    }
}

template <class T> struct is_tuple : std::false_type {};
template <class... Us> struct is_tuple<std::tuple<Us...>> : std::true_type {};

} // namespace detail

// ---------------------------------------------------------------------------
//  Key extractors (Boost-style + modern shorthand)
// ---------------------------------------------------------------------------

namespace detail {
template <class Class, class X>
concept reachable = requires(const X& x) { ::mic::detail::to_ref<Class>(x); };
}

// identity<T> : the element itself is the key (dereferenced if held by pointer).
template <class T>
struct identity {
    using result_type = T;
    template <detail::reachable<T> X> constexpr const T& operator()(const X& x) const { return detail::to_ref<T>(x); }
};

// member<Class, Type, &Class::field>
template <class Class, class Type, Type Class::* P>
struct member {
    using result_type = Type;
    template <detail::reachable<Class> X> constexpr const Type& operator()(const X& x) const { return detail::to_ref<Class>(x).*P; }
};

// const_mem_fun<Class, Type, &Class::getter>  (member function pointer syntax)
template <class Class, class Type, Type (Class::* P)() const>
struct const_mem_fun {
    using result_type = Type;
    template <detail::reachable<Class> X> constexpr Type operator()(const X& x) const { return (detail::to_ref<Class>(x).*P)(); }
};

// global_fun<Value, Type, &free_function>
template <class Value, class Type, Type (*P)(const Value&)>
struct global_fun {
    using result_type = Type;
    template <detail::reachable<Value> X> constexpr Type operator()(const X& x) const { return P(detail::to_ref<Value>(x)); }
};

// composite_key<Extractors...> : tuple key, compared lexicographically.
template <class... Extractors>
struct composite_key {
    using result_type = std::tuple<std::remove_cvref_t<typename Extractors::result_type>...>;
    template <class X> result_type operator()(const X& x) const {
        return result_type{ Extractors{}(x)... };
    }
};

// --- key<...> NTTP shorthand : deduce the right extractor from a member/function pointer
namespace detail {

template <auto P, class T = decltype(P)>
struct one_key;
template <auto P, class C, class M>
    requires std::is_member_object_pointer_v<decltype(P)>
struct one_key<P, M C::*> { using type = ::mic::member<C, M, P>; };
template <auto P, class C, class R>
struct one_key<P, R (C::*)() const> { using type = ::mic::const_mem_fun<C, R, P>; };
template <auto P, class V, class R>
struct one_key<P, R (*)(const V&)> { using type = ::mic::global_fun<V, R, P>; };

template <auto... P> struct key_sel;
template <auto P> struct key_sel<P> { using type = typename one_key<P>::type; };
template <auto P0, auto... Pn> struct key_sel<P0, Pn...> {
    using type = composite_key<typename one_key<P0>::type, typename one_key<Pn>::type...>;
};

} // namespace detail

template <auto... P>
struct key : detail::key_sel<P...>::type {};

// ---------------------------------------------------------------------------
//  Composite comparator / hasher / equality (lexicographic, prefix-aware)
// ---------------------------------------------------------------------------
struct composite_less {
    using is_transparent = void;
    template <class X, class Y> static constexpr int three(const X& x, const Y& y) {
        if (x < y) return -1;
        if (y < x) return 1;
        return 0;
    }
    template <class A, class B, std::size_t... I>
    static constexpr int cmp(const A& a, const B& b, std::index_sequence<I...>) {
        int r = 0;
        ((r = three(std::get<I>(a), std::get<I>(b)), r != 0) || ...);
        return r;
    }
    template <class A, class B> constexpr bool operator()(const A& a, const B& b) const {
        constexpr std::size_t n = std::min(std::tuple_size_v<std::remove_cvref_t<A>>,
                                           std::tuple_size_v<std::remove_cvref_t<B>>);
        return cmp(a, b, std::make_index_sequence<n>{}) < 0;
    }
};

struct composite_hash {
    using is_transparent = void;
    template <class X> static std::size_t hash_one(const X& x) {
        return std::hash<std::remove_cvref_t<X>>{}(x);
    }
    template <class T, std::size_t... I>
    static std::size_t go(const T& t, std::index_sequence<I...>) {
        std::size_t h = 0;
        ((h ^= hash_one(std::get<I>(t)) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)), ...);
        return h;
    }
    template <class T> std::size_t operator()(const T& t) const {
        return go(t, std::make_index_sequence<std::tuple_size_v<std::remove_cvref_t<T>>>{});
    }
};

struct composite_equal {
    using is_transparent = void;
    template <class A, class B> bool operator()(const A& a, const B& b) const { return a == b; }
};

// Transparent default hashers, so a hashed-index lookup never materialises the
// key. For strings, hashing through string_view is standard-mandated to agree
// with std::hash<std::string>, so find("literal") / find(string_view) are
// zero-allocation. For other key types, a transparent wrapper over std::hash
// still lets a same-typed key be looked up without a copy.
struct string_hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};
template <class Key>
struct scalar_hash {
    using is_transparent = void;
    std::size_t operator()(const Key& k) const noexcept(noexcept(std::hash<Key>{}(k))) { return std::hash<Key>{}(k); }
};

// ---------------------------------------------------------------------------
//  Index specifications
// ---------------------------------------------------------------------------
enum class index_kind { ordered, hashed, sequenced, random_access };

namespace detail {
template <class E> using ekey = std::remove_cvref_t<typename E::result_type>;
template <class E> using default_ordered_compare =
    std::conditional_t<is_tuple<ekey<E>>::value, composite_less, std::less<>>;
template <class E> using default_hash =
    std::conditional_t<is_tuple<ekey<E>>::value, composite_hash,
        std::conditional_t<std::is_same_v<ekey<E>, std::string> || std::is_same_v<ekey<E>, std::string_view>,
            string_hash, scalar_hash<ekey<E>>>>;
template <class E> using default_equal =
    std::conditional_t<is_tuple<ekey<E>>::value, composite_equal, std::equal_to<>>;
} // namespace detail

template <fixed_string Tag, class Extractor, class Compare = detail::default_ordered_compare<Extractor>>
struct ordered_unique {
    static constexpr auto       tag    = Tag;
    static constexpr index_kind kind   = index_kind::ordered;
    static constexpr bool       unique = true;
    static constexpr bool       ranked = false;
    using extractor = Extractor;
    using compare   = Compare;
};
template <fixed_string Tag, class Extractor, class Compare = detail::default_ordered_compare<Extractor>>
struct ordered_non_unique {
    static constexpr auto       tag    = Tag;
    static constexpr index_kind kind   = index_kind::ordered;
    static constexpr bool       unique = false;
    static constexpr bool       ranked = false;
    using extractor = Extractor;
    using compare   = Compare;
};
template <fixed_string Tag, class Extractor, class Compare = detail::default_ordered_compare<Extractor>>
struct ranked_unique {
    static constexpr auto       tag    = Tag;
    static constexpr index_kind kind   = index_kind::ordered;
    static constexpr bool       unique = true;
    static constexpr bool       ranked = true;
    using extractor = Extractor;
    using compare   = Compare;
};
template <fixed_string Tag, class Extractor, class Compare = detail::default_ordered_compare<Extractor>>
struct ranked_non_unique {
    static constexpr auto       tag    = Tag;
    static constexpr index_kind kind   = index_kind::ordered;
    static constexpr bool       unique = false;
    static constexpr bool       ranked = true;
    using extractor = Extractor;
    using compare   = Compare;
};
template <fixed_string Tag, class Extractor,
          class Hash = detail::default_hash<Extractor>,
          class Eq   = detail::default_equal<Extractor>>
struct hashed_unique {
    static constexpr auto       tag    = Tag;
    static constexpr index_kind kind   = index_kind::hashed;
    static constexpr bool       unique = true;
    static constexpr bool       ranked = false;
    using extractor = Extractor;
    using hash      = Hash;
    using equal     = Eq;
};
template <fixed_string Tag, class Extractor,
          class Hash = detail::default_hash<Extractor>,
          class Eq   = detail::default_equal<Extractor>>
struct hashed_non_unique {
    static constexpr auto       tag    = Tag;
    static constexpr index_kind kind   = index_kind::hashed;
    static constexpr bool       unique = false;
    static constexpr bool       ranked = false;
    using extractor = Extractor;
    using hash      = Hash;
    using equal     = Eq;
};
template <fixed_string Tag>
struct sequenced {
    static constexpr auto       tag    = Tag;
    static constexpr index_kind kind   = index_kind::sequenced;
    static constexpr bool       unique = false;
    static constexpr bool       ranked = false;
    using extractor = void;
};
template <fixed_string Tag>
struct random_access {
    static constexpr auto       tag    = Tag;
    static constexpr index_kind kind   = index_kind::random_access;
    static constexpr bool       unique = false;
    static constexpr bool       ranked = false;
    using extractor = void;
};

template <class... Specs>
struct indexed_by {
    using specs = std::tuple<Specs...>;
    static constexpr std::size_t count = sizeof...(Specs);
};

// ---------------------------------------------------------------------------
//  Index cores (parameterised on the node pointer)
// ---------------------------------------------------------------------------
namespace detail {

// Intrusive tree linkage stored inside the element node (one per ordered/ranked
// index). No separate allocation: the index lives in the node it indexes.
template <class NodePtr, class KeyType, bool InlineKey>
struct tree_hook {
    NodePtr      l = nullptr, r = nullptr, up = nullptr;
    std::size_t  sz     = 1;   // subtree size  -> O(log n) nth()/rank()
    std::int32_t height = 1;   // AVL height    -> balanced, std::set-class find
    // A small trivially-copyable key is cached next to the links, so a comparison
    // reads it from the same cache line instead of chasing node->value.
    MIC_NO_UNIQUE_ADDRESS std::conditional_t<InlineKey, KeyType, std::monostate> key{};
};

// Size-augmented AVL tree whose links live in std::get<I>(node->hooks). Backs
// ordered indices (find/lower_bound/...) and ranked indices (nth/rank). Height-
// balanced like std::set, so lookups do ~log2(n) comparisons. Comparisons
// re-extract the key from the node's co-located value (Compare is transparent
// over NodePtr / lookup keys).
template <std::size_t I, class NodePtr, class Extractor, class Compare, class KeyType, bool InlineKey, bool Unique>
class intrusive_tree {
    static tree_hook<NodePtr, KeyType, InlineKey>& H(NodePtr p) { return std::get<I>(p->hooks); }
    template <class T> static decltype(auto) kx(const T& t) {           // key used for comparison
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, NodePtr>) {
            if constexpr (InlineKey) return (H(t).key);                 // cached, co-located with links
            else return Extractor{}((*t).value);                        // re-extract from co-located value
        } else return (t);                                             // bare lookup key
    }
    template <class A, class B> static bool less(const A& a, const B& b) { return Compare{}(kx(a), kx(b)); }
    static std::size_t  S(NodePtr p)  { return p ? H(p).sz : 0; }
    static std::int32_t Ht(NodePtr p) { return p ? H(p).height : 0; }
    static std::int32_t bf(NodePtr p) { return Ht(H(p).l) - Ht(H(p).r); }
    static void upd(NodePtr p) {
        if (!p) return;
        H(p).sz = 1 + S(H(p).l) + S(H(p).r);
        std::int32_t hl = Ht(H(p).l), hr = Ht(H(p).r);
        H(p).height = 1 + (hl > hr ? hl : hr);
    }
    static NodePtr leftmost(NodePtr p)  { if (!p) return nullptr; while (H(p).l) p = H(p).l; return p; }
    static NodePtr rightmost(NodePtr p) { if (!p) return nullptr; while (H(p).r) p = H(p).r; return p; }
    static NodePtr succ(NodePtr p) { if (H(p).r) return leftmost(H(p).r); while (H(p).up && p == H(H(p).up).r) p = H(p).up; return H(p).up; }
    static NodePtr pred(NodePtr p) { if (H(p).l) return rightmost(H(p).l); while (H(p).up && p == H(H(p).up).l) p = H(p).up; return H(p).up; }

    NodePtr root_ = nullptr;
    std::size_t n_ = 0;

    void rot_right(NodePtr y) {
        NodePtr x = H(y).l, p = H(y).up;
        H(y).l = H(x).r; if (H(x).r) H(H(x).r).up = y;
        H(x).r = y; H(y).up = x; H(x).up = p;
        if (p) { if (H(p).l == y) H(p).l = x; else H(p).r = x; } else root_ = x;
        upd(y); upd(x);
    }
    void rot_left(NodePtr y) {
        NodePtr x = H(y).r, p = H(y).up;
        H(y).r = H(x).l; if (H(x).l) H(H(x).l).up = y;
        H(x).l = y; H(y).up = x; H(x).up = p;
        if (p) { if (H(p).l == y) H(p).l = x; else H(p).r = x; } else root_ = x;
        upd(y); upd(x);
    }
    void rebalance(NodePtr a) {
        std::int32_t b = bf(a);
        if (b > 1)       { if (bf(H(a).l) < 0) rot_left(H(a).l);  rot_right(a); }
        else if (b < -1) { if (bf(H(a).r) > 0) rot_right(H(a).r); rot_left(a); }
    }
    void fixup(NodePtr a) { while (a) { NodePtr up = H(a).up; upd(a); rebalance(a); a = up; } }
    void transplant(NodePtr u, NodePtr v) {
        NodePtr p = H(u).up;
        if (!p) root_ = v; else if (H(p).l == u) H(p).l = v; else H(p).r = v;
        if (v) H(v).up = p;
    }
    template <class K> NodePtr lb(const K& k) const {
        NodePtr t = root_, res = nullptr;
        while (t) { if (less(t, k)) t = H(t).r; else { res = t; t = H(t).l; } }
        return res;
    }
    template <class K> NodePtr ub(const K& k) const {
        NodePtr t = root_, res = nullptr;
        while (t) { if (less(k, t)) { res = t; t = H(t).l; } else t = H(t).r; }
        return res;
    }
    void avl_insert(NodePtr nn) {   // nn's hook is already initialised (key cached) by try_add
        if (!root_) { root_ = nn; ++n_; return; }
        NodePtr t = root_, par = nullptr; bool left = false;
        while (t) { par = t; if (less(nn, t)) { left = true; t = H(t).l; } else { left = false; t = H(t).r; } }
        H(nn).up = par; (left ? H(par).l : H(par).r) = nn; ++n_;
        fixup(par);
    }
    void avl_erase(NodePtr z) {
        NodePtr fix;
        if (H(z).l && H(z).r) {                 // two children: splice in the successor
            NodePtr s = leftmost(H(z).r);
            if (H(s).up != z) { fix = H(s).up; transplant(s, H(s).r); H(s).r = H(z).r; H(H(s).r).up = s; }
            else              { fix = s; }
            transplant(z, s);
            H(s).l = H(z).l; if (H(s).l) H(H(s).l).up = s;
        } else {
            NodePtr child = H(z).l ? H(z).l : H(z).r;
            fix = H(z).up;
            transplant(z, child);
        }
        fixup(fix);
        --n_;
    }

public:
    intrusive_tree() = default;
    intrusive_tree(intrusive_tree&& o) noexcept : root_(o.root_), n_(o.n_) { o.root_ = nullptr; o.n_ = 0; }
    intrusive_tree& operator=(intrusive_tree&& o) noexcept { root_ = o.root_; n_ = o.n_; o.root_ = nullptr; o.n_ = 0; return *this; }
    intrusive_tree(const intrusive_tree&) = delete;
    intrusive_tree& operator=(const intrusive_tree&) = delete;

    void clear() { root_ = nullptr; n_ = 0; }   // nodes are owned/freed by the container
    void swap(intrusive_tree& o) noexcept { using std::swap; swap(root_, o.root_); swap(n_, o.n_); }
    std::size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    std::int32_t root_height() const { return Ht(root_); }   // for AVL-balance assertions in tests

    struct iterator {
        const intrusive_tree* tree = nullptr;
        NodePtr cur = nullptr;
        using value_type        = NodePtr;
        using reference         = NodePtr;
        using pointer           = NodePtr;
        using difference_type   = std::ptrdiff_t;
        using iterator_category = std::bidirectional_iterator_tag;
        using iterator_concept  = std::bidirectional_iterator_tag;
        iterator() = default;
        iterator(const intrusive_tree* t, NodePtr c) : tree(t), cur(c) {}
        reference operator*()  const { return cur; }
        iterator& operator++() { cur = succ(cur); return *this; }
        iterator  operator++(int) { auto t = *this; ++*this; return t; }
        iterator& operator--() { cur = cur ? pred(cur) : rightmost(tree->root_); return *this; }
        iterator  operator--(int) { auto t = *this; --*this; return t; }
        bool operator==(const iterator& o) const { return cur == o.cur; }
    };
    using const_iterator = iterator;
    iterator begin() const { return { this, leftmost(root_) }; }
    iterator end()   const { return { this, nullptr }; }

    template <class K> iterator lower_bound(const K& k) const { return { this, lb(k) }; }
    template <class K> iterator upper_bound(const K& k) const { return { this, ub(k) }; }
    template <class K> iterator find(const K& k) const {
        NodePtr t = lb(k);
        return (t && !less(k, t)) ? iterator{ this, t } : end();
    }
    template <class K> std::pair<iterator, iterator> equal_range(const K& k) const { return { lower_bound(k), upper_bound(k) }; }
    template <class K> std::size_t count(const K& k) const {
        std::size_t c = 0; for (auto it = lower_bound(k), e = upper_bound(k); it != e; ++it) ++c; return c;
    }
    iterator nth(std::size_t k) const {
        NodePtr t = root_;
        while (t) { std::size_t ls = S(H(t).l); if (k < ls) t = H(t).l; else if (k == ls) return { this, t }; else { k -= ls + 1; t = H(t).r; } }
        return end();
    }
    std::size_t rank(iterator it) const {
        NodePtr t = it.cur;
        if (!t) return n_;
        std::size_t r = S(H(t).l);
        while (H(t).up) { if (t == H(H(t).up).r) r += S(H(H(t).up).l) + 1; t = H(t).up; }
        return r;
    }
    bool try_add(NodePtr p, NodePtr& conflict) {
        H(p) = tree_hook<NodePtr, KeyType, InlineKey>{};            // init hook + cache key BEFORE
        if constexpr (InlineKey) H(p).key = Extractor{}((*p).value); // the unique check uses it
        if constexpr (Unique) {
            NodePtr e = lb(p);
            if (e && !less(p, e)) { conflict = e; return false; }
        }
        avl_insert(p);
        return true;
    }
    void erase_node(NodePtr z) { avl_erase(z); }
};

template <std::size_t I, class NodePtr, class Extractor, class Compare, bool Unique, class Alloc>
struct tree_core {
    static constexpr index_kind kind = index_kind::ordered;
    using key_type = std::remove_cvref_t<typename Extractor::result_type>;

    // Cache the key inline in the tree hook when it is small and trivially
    // copyable (e.g. an int id); string/large keys re-extract from the value.
    static constexpr bool inline_key =
        std::is_trivially_copyable_v<key_type> && std::is_default_constructible_v<key_type> && sizeof(key_type) <= 16;

    template <class V> static key_type extract_key(const V& v) { return Extractor{}(v); }
    static bool keys_equal(const key_type& a, const key_type& b) { return !Compare{}(a, b) && !Compare{}(b, a); }

    using container = intrusive_tree<I, NodePtr, Extractor, Compare, key_type, inline_key, Unique>;
    using hook_type = tree_hook<NodePtr, key_type, inline_key>;
    container c;
    tree_core() = default;
    explicit tree_core(const Alloc&) {}   // intrusive: no allocation

    bool try_insert(NodePtr p, hook_type&, NodePtr& conflict) { return c.try_add(p, conflict); }
    void remove(NodePtr p, hook_type&) { c.erase_node(p); }
};

// Intrusive hash chaining stored inside the element node (one per hashed index).
template <class NodePtr>
struct hash_hook { NodePtr next = nullptr; std::size_t hash = 0; };

// Intrusive bucketed hash table: chains thread through std::get<I>(node->hooks),
// so elements add no allocation (only the bucket array, which grows on rehash).
// Transparent (heterogeneous lookup hashes/compares without materialising the
// key) and keeps equal keys contiguous so equal_range works for non-unique.
template <std::size_t I, class NodePtr, class Extractor, class Hash, class Eq, bool Unique, class Alloc>
class intrusive_hash {
    static hash_hook<NodePtr>& H(NodePtr p) { return std::get<I>(p->hooks); }
    template <class T> static decltype(auto) kof(const T& t) {
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, NodePtr>) return Extractor{}((*t).value);
        else return (t);
    }
    template <class A, class B> static bool keq(const A& a, const B& b) { return Eq{}(kof(a), kof(b)); }

    using bucket_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<NodePtr>;
    std::vector<NodePtr, bucket_alloc> buckets_;
    std::size_t n_ = 0;
    float max_lf_ = 1.0f;

    void rehash_to(std::size_t nb) {
        std::vector<NodePtr, bucket_alloc> nbk(nb, nullptr, buckets_.get_allocator());
        for (NodePtr head : buckets_)
            for (NodePtr p = head; p; ) { NodePtr nx = H(p).next; std::size_t b = H(p).hash % nb; H(p).next = nbk[b]; nbk[b] = p; p = nx; }
        buckets_.swap(nbk);
    }
    void maybe_grow() { if (static_cast<float>(n_) > max_lf_ * static_cast<float>(buckets_.size())) rehash_to(buckets_.size() * 2); }
    // Restore a just-moved-from table to a valid empty state (one null bucket): a
    // single-pointer allocation, so the modulo in every keyed op stays well-defined.
    void reseed_empty() noexcept { buckets_.assign(1, nullptr); n_ = 0; }

public:
    explicit intrusive_hash(const Alloc& a = Alloc{}) : buckets_(8, nullptr, bucket_alloc(a)) {}
    // Moving steals the bucket array; re-seed the source with one (empty) bucket so a
    // moved-from index stays a valid, queryable empty table — find/insert/erase keep
    // working (bucket = hash % buckets_.size()) instead of dividing by zero, matching
    // std::unordered_map's "moved-from is a valid empty container" guarantee.
    intrusive_hash(intrusive_hash&& o) noexcept
        : buckets_(std::move(o.buckets_)), n_(o.n_), max_lf_(o.max_lf_) { o.reseed_empty(); }
    intrusive_hash& operator=(intrusive_hash&& o) noexcept {
        buckets_ = std::move(o.buckets_); n_ = o.n_; max_lf_ = o.max_lf_; o.reseed_empty(); return *this;
    }
    intrusive_hash(const intrusive_hash&) = delete;
    intrusive_hash& operator=(const intrusive_hash&) = delete;

    void clear() { std::fill(buckets_.begin(), buckets_.end(), nullptr); n_ = 0; }   // nodes freed by the container
    void swap(intrusive_hash& o) noexcept { using std::swap; swap(buckets_, o.buckets_); swap(n_, o.n_); swap(max_lf_, o.max_lf_); }
    std::size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    std::size_t bucket_count() const { return buckets_.size(); }
    float load_factor() const { return static_cast<float>(n_) / static_cast<float>(buckets_.size()); }
    float max_load_factor() const { return max_lf_; }
    void  max_load_factor(float z) { max_lf_ = z; }

    struct iterator {
        const intrusive_hash* tbl = nullptr;
        NodePtr cur = nullptr;
        using value_type        = NodePtr;
        using reference         = NodePtr;
        using pointer           = NodePtr;
        using difference_type   = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept  = std::forward_iterator_tag;
        iterator() = default;
        iterator(const intrusive_hash* t, NodePtr c) : tbl(t), cur(c) {}
        reference operator*()  const { return cur; }
        iterator& operator++() {
            if (H(cur).next) { cur = H(cur).next; return *this; }
            std::size_t b = H(cur).hash % tbl->buckets_.size() + 1;
            while (b < tbl->buckets_.size() && !tbl->buckets_[b]) ++b;
            cur = (b < tbl->buckets_.size()) ? tbl->buckets_[b] : nullptr;
            return *this;
        }
        iterator operator++(int) { auto t = *this; ++*this; return t; }
        bool operator==(const iterator& o) const { return cur == o.cur; }
    };
    using const_iterator = iterator;
    iterator begin() const { for (std::size_t b = 0; b < buckets_.size(); ++b) if (buckets_[b]) return { this, buckets_[b] }; return end(); }
    iterator end()   const { return { this, nullptr }; }

    template <class K> NodePtr find_node(const K& k) const {
        for (NodePtr p = buckets_[Hash{}(k) % buckets_.size()]; p; p = H(p).next) if (keq(p, k)) return p;
        return nullptr;
    }
    template <class K> iterator find(const K& k) const { return { this, find_node(k) }; }
    template <class K> std::size_t count(const K& k) const {
        std::size_t c = 0;
        for (NodePtr p = buckets_[Hash{}(k) % buckets_.size()]; p; p = H(p).next) if (keq(p, k)) ++c;
        return c;
    }
    template <class K> std::pair<iterator, iterator> equal_range(const K& k) const {
        NodePtr first = find_node(k);
        if (!first) return { end(), end() };
        NodePtr last = first;
        while (H(last).next && keq(H(last).next, k)) last = H(last).next;   // equal keys are contiguous
        iterator e{ this, last }; ++e;
        return { iterator{ this, first }, e };
    }

    bool try_add(NodePtr p, NodePtr& conflict) {
        std::size_t h = Hash{}(Extractor{}((*p).value));
        H(p).hash = h; H(p).next = nullptr;
        std::size_t b = h % buckets_.size();
        for (NodePtr q = buckets_[b]; q; q = H(q).next) {
            if (keq(q, p)) {
                if constexpr (Unique) { conflict = q; return false; }
                else { H(p).next = H(q).next; H(q).next = p; ++n_; maybe_grow(); return true; }  // splice into the group
            }
        }
        H(p).next = buckets_[b]; buckets_[b] = p; ++n_;   // new key: prepend
        maybe_grow();
        return true;
    }
    void erase_node(NodePtr z) {
        NodePtr* pp = &buckets_[H(z).hash % buckets_.size()];   // cached hash -> right bucket even after a key change
        while (*pp && *pp != z) pp = &H(*pp).next;
        if (*pp == z) { *pp = H(z).next; --n_; }
    }
};

template <std::size_t I, class NodePtr, class Extractor, class Hash, class Eq, bool Unique, class Alloc>
struct hash_core {
    static constexpr index_kind kind = index_kind::hashed;
    using key_type = std::remove_cvref_t<typename Extractor::result_type>;
    template <class V> static key_type extract_key(const V& v) { return Extractor{}(v); }
    static bool keys_equal(const key_type& a, const key_type& b) { return Eq{}(a, b); }
    using container = intrusive_hash<I, NodePtr, Extractor, Hash, Eq, Unique, Alloc>;
    using hook_type = hash_hook<NodePtr>;
    container c;
    hash_core() = default;
    explicit hash_core(const Alloc& a) : c(a) {}
    bool try_insert(NodePtr p, hook_type&, NodePtr& conflict) { return c.try_add(p, conflict); }
    void remove(NodePtr p, hook_type&) { c.erase_node(p); }
};

// Intrusive doubly-linked list for sequenced indices (links in the node).
template <class NodePtr>
struct seq_hook { NodePtr prev = nullptr, next = nullptr; };

template <std::size_t I, class NodePtr>
class intrusive_list {
    static seq_hook<NodePtr>& H(NodePtr p) { return std::get<I>(p->hooks); }
    NodePtr head_ = nullptr, tail_ = nullptr;
    void unlink(NodePtr p) {
        (H(p).prev ? H(H(p).prev).next : head_) = H(p).next;
        (H(p).next ? H(H(p).next).prev : tail_) = H(p).prev;
    }
public:
    intrusive_list() = default;
    intrusive_list(intrusive_list&& o) noexcept : head_(o.head_), tail_(o.tail_) { o.head_ = o.tail_ = nullptr; }
    intrusive_list& operator=(intrusive_list&& o) noexcept { head_ = o.head_; tail_ = o.tail_; o.head_ = o.tail_ = nullptr; return *this; }
    intrusive_list(const intrusive_list&) = delete;
    intrusive_list& operator=(const intrusive_list&) = delete;
    void clear() { head_ = tail_ = nullptr; }   // nodes freed by the container
    void swap(intrusive_list& o) noexcept { using std::swap; swap(head_, o.head_); swap(tail_, o.tail_); }

    void push_back(NodePtr p) { H(p).prev = tail_; H(p).next = nullptr; (tail_ ? H(tail_).next : head_) = p; tail_ = p; }
    void erase(NodePtr p) { unlink(p); }
    void move_to_front(NodePtr p) { unlink(p); H(p).prev = nullptr; H(p).next = head_; (head_ ? H(head_).prev : tail_) = p; head_ = p; }
    void relocate(NodePtr before, NodePtr p) {        // move p to just before `before` (null -> back)
        if (p == before) return;
        unlink(p);
        if (!before) { push_back(p); return; }
        NodePtr pr = H(before).prev;
        H(p).prev = pr; H(p).next = before;
        (pr ? H(pr).next : head_) = p;
        H(before).prev = p;
    }
    void reverse() {
        for (NodePtr c = head_; c; ) { NodePtr nx = H(c).next; std::swap(H(c).prev, H(c).next); c = nx; }
        std::swap(head_, tail_);
    }
    struct iterator {
        const intrusive_list* lst = nullptr;
        NodePtr cur = nullptr;
        using value_type        = NodePtr;
        using reference         = NodePtr;
        using pointer           = NodePtr;
        using difference_type   = std::ptrdiff_t;
        using iterator_category = std::bidirectional_iterator_tag;
        using iterator_concept  = std::bidirectional_iterator_tag;
        iterator() = default;
        iterator(const intrusive_list* l, NodePtr c) : lst(l), cur(c) {}
        reference operator*()  const { return cur; }
        iterator& operator++() { cur = H(cur).next; return *this; }
        iterator  operator++(int) { auto t = *this; ++*this; return t; }
        iterator& operator--() { cur = cur ? H(cur).prev : lst->tail_; return *this; }
        iterator  operator--(int) { auto t = *this; --*this; return t; }
        bool operator==(const iterator& o) const { return cur == o.cur; }
    };
    using const_iterator = iterator;
    iterator begin() const { return { this, head_ }; }
    iterator end()   const { return { this, nullptr }; }
};

template <std::size_t I, class NodePtr, class Alloc>
struct seq_core {
    static constexpr index_kind kind = index_kind::sequenced;
    using container = intrusive_list<I, NodePtr>;
    using hook_type = seq_hook<NodePtr>;
    container c;
    seq_core() = default;
    explicit seq_core(const Alloc&) {}
    bool try_insert(NodePtr p, hook_type&, NodePtr&) { c.push_back(p); return true; }
    void remove(NodePtr p, hook_type&) { c.erase(p); }
};

template <class NodePtr, class Alloc>
struct rand_core {
    static constexpr index_kind kind = index_kind::random_access;
    using ptr_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<NodePtr>;
    using container = std::vector<NodePtr, ptr_alloc>;
    using hook_type = std::monostate;
    container c;
    rand_core() = default;
    explicit rand_core(const Alloc& a) : c(ptr_alloc(a)) {}
    bool try_insert(NodePtr p, hook_type&, NodePtr&) { c.push_back(p); return true; }
    void remove(NodePtr p, hook_type&) {
        auto it = std::find(c.begin(), c.end(), p);
        if (it != c.end()) c.erase(it);
    }
};

// map a spec to its core type (I = its position in the index tuple, used by the
// intrusive cores to find their hook inside std::get<I>(node->hooks))
template <class Spec, class NodePtr, class Alloc, std::size_t I> struct core_for;
template <fixed_string T, class E, class C, class NP, class A, std::size_t I>
struct core_for<ordered_unique<T, E, C>, NP, A, I> { using type = tree_core<I, NP, E, C, true, A>; };
template <fixed_string T, class E, class C, class NP, class A, std::size_t I>
struct core_for<ordered_non_unique<T, E, C>, NP, A, I> { using type = tree_core<I, NP, E, C, false, A>; };
template <fixed_string T, class E, class C, class NP, class A, std::size_t I>
struct core_for<ranked_unique<T, E, C>, NP, A, I> { using type = tree_core<I, NP, E, C, true, A>; };
template <fixed_string T, class E, class C, class NP, class A, std::size_t I>
struct core_for<ranked_non_unique<T, E, C>, NP, A, I> { using type = tree_core<I, NP, E, C, false, A>; };
template <fixed_string T, class E, class H, class Q, class NP, class A, std::size_t I>
struct core_for<hashed_unique<T, E, H, Q>, NP, A, I> { using type = hash_core<I, NP, E, H, Q, true, A>; };
template <fixed_string T, class E, class H, class Q, class NP, class A, std::size_t I>
struct core_for<hashed_non_unique<T, E, H, Q>, NP, A, I> { using type = hash_core<I, NP, E, H, Q, false, A>; };
template <fixed_string T, class NP, class A, std::size_t I>
struct core_for<sequenced<T>, NP, A, I> { using type = seq_core<I, NP, A>; };
template <fixed_string T, class NP, class A, std::size_t I>
struct core_for<random_access<T>, NP, A, I> { using type = rand_core<NP, A>; };

template <class C, class = void> struct core_key { using type = void; };
template <class C> struct core_key<C, std::void_t<typename C::key_type>> { using type = typename C::key_type; };

} // namespace detail

// ---------------------------------------------------------------------------
//  Error type for the std::expected-flavoured API
// ---------------------------------------------------------------------------
template <class Value>
struct insert_error {
    enum class reason { duplicate_in_unique_index };
    reason           why = reason::duplicate_in_unique_index;
    std::size_t      index_pos = 0;        // which index (0-based) rejected the insert
    std::string_view index_tag;            // ...and its tag, e.g. "by_email"
    const Value*     blocking = nullptr;   // the existing element it conflicts with
};

// ---------------------------------------------------------------------------
//  Pointer-like detection + element-access policy
// ---------------------------------------------------------------------------
namespace detail {
template <class T>
concept pointer_like =
    requires { typename std::pointer_traits<T>::element_type; } &&
    requires(const T& p) { *p; };
template <class T, bool = pointer_like<T>> struct pointee { using type = T; };
template <class T> struct pointee<T, true> { using type = typename std::pointer_traits<T>::element_type; };
template <class T> using pointee_t = typename pointee<T>::type;
} // namespace detail

// Element-access policy. With const_element_policy AND a pointer-like Value
// (T*, std::shared_ptr<T>, std::unique_ptr<T>, ...), index iterators expose a
// const view of the pointee (const T&) so a key can never be mutated behind the
// indices' backs through the stored pointer — writes must go through
// modify()/replace(). It is a no-op for value (non-pointer) elements, which are
// already const-protected. See const_element_container below.
struct standard_policy      { static constexpr bool const_elements = false; };
struct const_element_policy { static constexpr bool const_elements = true;  };

// ===========================================================================
//  multi_index_container
// ===========================================================================
template <class Value, class IndexedBy, class Alloc = std::allocator<Value>, class Policy = standard_policy>
class multi_index_container {
public:
    using value_type     = Value;
    using size_type      = std::size_t;
    using allocator_type = Alloc;

private:
    using specs_tuple = typename IndexedBy::specs;
    static constexpr std::size_t N = std::tuple_size_v<specs_tuple>;
    static_assert(N >= 1, "mic::multi_index_container requires at least one index");
    template <std::size_t I> using spec_at = std::tuple_element_t<I, specs_tuple>;

    // When enabled, iterators present pointer-like elements as a const pointee view.
    static constexpr bool kConstElem = Policy::const_elements && detail::pointer_like<Value>;
    using elem_value = detail::pointee_t<Value>;

    struct node;
    template <std::size_t I> using core_at = typename detail::core_for<spec_at<I>, node*, Alloc, I>::type;

    template <class Seq> struct hooks_t;
    template <std::size_t... I> struct hooks_t<std::index_sequence<I...>> {
        using type = std::tuple<typename core_at<I>::hook_type...>;
    };
    using hook_tuple = typename hooks_t<std::make_index_sequence<N>>::type;

    template <class Seq> struct cores_t;
    template <std::size_t... I> struct cores_t<std::index_sequence<I...>> {
        using type = std::tuple<core_at<I>...>;
    };
    using cores_tuple = typename cores_t<std::make_index_sequence<N>>::type;

    using node_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<node>;

    struct node {
        Value     value;
        hook_tuple hooks{};
        node* rprev = nullptr;   // intrusive registry list (every live node, in
        node* rnext = nullptr;   // insertion order) — no separate allocation
        template <class... A>
        explicit node(A&&... a) : value(std::forward<A>(a)...) {}
    };

    // v1 supports stateless, default-constructible extractors/comparators/hashers
    // only (they are invoked as Extractor{} / Compare{} / Hash{}). Reject anything
    // else with a clear message rather than silently dropping state.
    template <std::size_t I>
    static constexpr bool check_index() {
        using S = spec_at<I>;
        if constexpr (!std::is_void_v<typename S::extractor>) {
            using E = typename S::extractor;
            static_assert(std::is_default_constructible_v<E>,
                "mic: key extractors must be default-constructible (stateful extractors are unsupported in v1)");
            static_assert(std::is_invocable_v<E, const Value&>,
                "mic: key extractor is not invocable on 'const Value&' — check the member-pointer class, "
                "or that the element type can be dereferenced to it (pointer / smart-pointer / reference_wrapper)");
        }
        return true;
    }
    static constexpr bool checked_ =
        []<std::size_t... I>(std::index_sequence<I...>) { return (check_index<I>() && ...); }(std::make_index_sequence<N>{});

    MIC_NO_UNIQUE_ADDRESS Alloc      alloc_{};
    MIC_NO_UNIQUE_ADDRESS node_alloc nalloc_{ alloc_ };
    cores_tuple   cores_{ make_cores(alloc_, std::make_index_sequence<N>{}) };
    node* rhead_ = nullptr;          // intrusive registry: head/tail of all live nodes
    node* rtail_ = nullptr;
    std::size_t   size_{0};

    template <std::size_t... I>
    static cores_tuple make_cores(const Alloc& a, std::index_sequence<I...>) {
        return cores_tuple{ core_at<I>(a)... };
    }

    void reg_push_back(node* p) {
        p->rprev = rtail_; p->rnext = nullptr;
        (rtail_ ? rtail_->rnext : rhead_) = p;
        rtail_ = p;
    }
    void reg_unlink(node* p) {
        (p->rprev ? p->rprev->rnext : rhead_) = p->rnext;
        (p->rnext ? p->rnext->rprev : rtail_) = p->rprev;
    }

    template <class... A> node* make_node(A&&... a) {
        node* p = std::allocator_traits<node_alloc>::allocate(nalloc_, 1);
        try { std::allocator_traits<node_alloc>::construct(nalloc_, p, std::forward<A>(a)...); }
        catch (...) { std::allocator_traits<node_alloc>::deallocate(nalloc_, p, 1); throw; }
        return p;
    }
    void destroy_node(node* p) {
        std::allocator_traits<node_alloc>::destroy(nalloc_, p);
        std::allocator_traits<node_alloc>::deallocate(nalloc_, p, 1);
    }

    template <std::size_t I>
    bool insert_all(node* p, node*& conflict, std::size_t& failed) {
        if constexpr (I == N) { return true; }
        else {
            auto& core = std::get<I>(cores_);
            node* cf = nullptr;
            if (!core.try_insert(p, std::get<I>(p->hooks), cf)) { conflict = cf; failed = I; return false; }
            if (!insert_all<I + 1>(p, conflict, failed)) { core.remove(p, std::get<I>(p->hooks)); return false; }
            return true;
        }
    }
    template <std::size_t I>
    void remove_all(node* p) {
        if constexpr (I < N) {
            std::get<I>(cores_).remove(p, std::get<I>(p->hooks));
            remove_all<I + 1>(p);
        }
    }

    // -- public iterator type / proxies are friends of the container ----------
public:
    template <class It>
    class index_iterator {
        It it_{};
    public:
        using under             = It;
        using value_type        = std::conditional_t<kConstElem, elem_value, Value>;
        using reference         = std::conditional_t<kConstElem, const elem_value&, const Value&>;
        using pointer           = std::conditional_t<kConstElem, const elem_value*, const Value*>;
        using difference_type   = std::ptrdiff_t;
        using iterator_category = typename std::iterator_traits<It>::iterator_category;
        using iterator_concept  = std::conditional_t<std::random_access_iterator<It>, std::random_access_iterator_tag,
            std::conditional_t<std::bidirectional_iterator<It>, std::bidirectional_iterator_tag,
            std::conditional_t<std::forward_iterator<It>, std::forward_iterator_tag, std::input_iterator_tag>>>;

        index_iterator() = default;
        explicit index_iterator(It it) : it_(it) {}

        reference operator*()  const {
            node* n = get_node();
            if constexpr (kConstElem) return *std::to_address(n->value);
            else                      return n->value;
        }
        pointer   operator->() const {
            node* n = get_node();
            if constexpr (kConstElem) return std::to_address(n->value);
            else                      return &n->value;
        }

        index_iterator& operator++() { ++it_; return *this; }
        index_iterator  operator++(int) { auto t = *this; ++it_; return t; }
        index_iterator& operator--() requires std::bidirectional_iterator<It> { --it_; return *this; }
        index_iterator  operator--(int) requires std::bidirectional_iterator<It> { auto t = *this; --it_; return t; }

        index_iterator& operator+=(difference_type n) requires std::random_access_iterator<It> { it_ += n; return *this; }
        index_iterator& operator-=(difference_type n) requires std::random_access_iterator<It> { it_ -= n; return *this; }
        friend index_iterator operator+(index_iterator a, difference_type n) requires std::random_access_iterator<It> { a.it_ += n; return a; }
        friend index_iterator operator+(difference_type n, index_iterator a) requires std::random_access_iterator<It> { a.it_ += n; return a; }
        friend index_iterator operator-(index_iterator a, difference_type n) requires std::random_access_iterator<It> { a.it_ -= n; return a; }
        friend difference_type operator-(index_iterator a, index_iterator b) requires std::random_access_iterator<It> { return a.it_ - b.it_; }
        reference operator[](difference_type n) const requires std::random_access_iterator<It> { return *(*this + n); }
        auto operator<=>(const index_iterator& o) const requires std::random_access_iterator<It> { return it_ <=> o.it_; }

        bool operator==(const index_iterator& o) const { return it_ == o.it_; }
        It    base()     const { return it_; }
        node* get_node() const { return *it_; }   // every index iterator dereferences to the element node*
    };

private:
    template <std::size_t I> using iter_t = index_iterator<typename core_at<I>::container::iterator>;

    template <std::size_t I> iter_t<I> make_iter(node* p) {
        using Core = core_at<I>;
        if constexpr (Core::kind == index_kind::random_access) {
            auto& c = std::get<I>(cores_).c;
            return iter_t<I>{ std::find(c.begin(), c.end(), p) };
        } else {   // ordered (tree), hashed, sequenced: all intrusive -> build iterator from (container, node)
            return iter_t<I>{ typename Core::container::iterator{ &std::get<I>(cores_).c, p } };
        }
    }
    template <std::size_t I> iter_t<I> end_iter() { return iter_t<I>{ std::get<I>(cores_).c.end() }; }

    template <fixed_string Tag>
    static constexpr std::size_t index_of_tag() {
        std::size_t r = N;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((spec_at<I>::tag == Tag ? (r = I, true) : false) || ...);
        }(std::make_index_sequence<N>{});
        return r;
    }

    std::pair<iter_t<0>, bool> insert_node(node* p) {
        node* conflict = nullptr; std::size_t failed = N;
        if (!insert_all<0>(p, conflict, failed)) {
            auto it = make_iter<0>(conflict);
            destroy_node(p);
            return { it, false };
        }
        reg_push_back(p); ++size_;
        return { make_iter<0>(p), true };
    }

    // Old keys must be captured BY VALUE before the mutator runs: when Value is a
    // pointer (shared_ptr<T> ...), a copy of Value shares the pointee, so it would
    // not preserve the pre-mutation key. monostate for keyless (seq/random) indices.
    template <std::size_t I> using core_key_t = typename detail::core_key<core_at<I>>::type;
    template <class K> using key_slot = std::conditional_t<std::is_void_v<K>, std::monostate, K>;
    template <class Seq> struct oldkeys_tt;
    template <std::size_t... I> struct oldkeys_tt<std::index_sequence<I...>> {
        using type = std::tuple<key_slot<core_key_t<I>>...>;
    };
    using oldkeys_tuple = typename oldkeys_tt<std::make_index_sequence<N>>::type;

    template <std::size_t I> void capture_one(const Value& v, oldkeys_tuple& out) const {
        using Core = core_at<I>;
        if constexpr (Core::kind != index_kind::sequenced && Core::kind != index_kind::random_access)
            std::get<I>(out) = Core::extract_key(v);
    }
    void capture_keys(const Value& v, oldkeys_tuple& out) const {
        [&]<std::size_t... I>(std::index_sequence<I...>) { (capture_one<I>(v, out), ...); }(std::make_index_sequence<N>{});
    }
    template <std::size_t I, class Arr> void changed_one(const oldkeys_tuple& old, const Value& newv, Arr& ch) const {
        using Core = core_at<I>;
        if constexpr (Core::kind == index_kind::sequenced || Core::kind == index_kind::random_access)
            ch[I] = false;
        else
            ch[I] = !Core::keys_equal(std::get<I>(old), Core::extract_key(newv));
    }
    template <class Arr>
    void compute_changed(const oldkeys_tuple& old, const Value& newv, Arr& ch) const {
        [&]<std::size_t... I>(std::index_sequence<I...>) { (changed_one<I>(old, newv, ch), ...); }(std::make_index_sequence<N>{});
    }
    template <class Arr>
    void erase_changed(node* p, const Arr& ch) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((ch[I] ? (std::get<I>(cores_).remove(p, std::get<I>(p->hooks)), void()) : void()), ...);
        }(std::make_index_sequence<N>{});
    }
    template <std::size_t I, class Arr>
    bool insert_changed(node* p, const Arr& ch, node*& conflict, std::size_t& failed) {
        if constexpr (I == N) { return true; }
        else {
            if (ch[I]) {
                node* cf = nullptr;
                if (!std::get<I>(cores_).try_insert(p, std::get<I>(p->hooks), cf)) { conflict = cf; failed = I; return false; }
                if (!insert_changed<I + 1>(p, ch, conflict, failed)) { std::get<I>(cores_).remove(p, std::get<I>(p->hooks)); return false; }
                return true;
            }
            return insert_changed<I + 1>(p, ch, conflict, failed);
        }
    }

    // modify()/replace() reposition ONLY the indices whose key actually changed,
    // leaving the rest — and any iterators/references into them — untouched
    // (matching Boost's "modify does not invalidate iterators"). Rollback-and-keep
    // on a unique clash; strong guarantee if the mutator throws (no index touched).
    template <class F>
    bool modify_node(node* p, F&& f) {
        static_assert(std::is_copy_constructible_v<Value>,
                      "modify() rollback requires a copy-constructible value_type");
        oldkeys_tuple oldk; capture_keys(p->value, oldk);
        Value snapshot = p->value;
        try { std::forward<F>(f)(p->value); }
        catch (...) { p->value = std::move(snapshot); throw; }
        return reposition(p, oldk, snapshot);
    }
    template <class V>
    bool replace_node(node* p, V&& v) {
        static_assert(std::is_move_constructible_v<Value>,
                      "replace() requires a move-constructible value_type for rollback");
        oldkeys_tuple oldk; capture_keys(p->value, oldk);
        Value snapshot = std::move(p->value);
        try { p->value = std::forward<V>(v); }
        catch (...) { p->value = std::move(snapshot); throw; }
        return reposition(p, oldk, snapshot);
    }
    // p is already mutated; `oldk` holds the pre-mutation keys, `snapshot` the
    // previous value. Move p within the indices whose key changed (all-or-nothing
    // uniqueness); on clash, restore the snapshot and return false (element stays).
    bool reposition(node* p, const oldkeys_tuple& oldk, Value& snapshot) {
        std::array<bool, N> ch{};
        compute_changed(oldk, p->value, ch);             // p->value holds the NEW value
        // Erase the changed indices while the element still reflects its OLD keys:
        // a hashed index's erase-by-iterator re-derives the bucket from the current
        // value, so it must see the old key to unlink the node from the right bucket.
        std::swap(p->value, snapshot);                   // p->value = OLD, snapshot = NEW
        erase_changed(p, ch);
        std::swap(p->value, snapshot);                   // p->value = NEW again
        node* conflict = nullptr; std::size_t failed = N;
        if (insert_changed<0>(p, ch, conflict, failed)) return true;
        p->value = std::move(snapshot);                  // rollback-and-keep (restore OLD keys)
        node* c2 = nullptr; std::size_t f2 = N;
        if (!insert_changed<0>(p, ch, c2, f2)) { reg_unlink(p); destroy_node(p); --size_; }
        return false;
    }
    void erase_node_full(node* p) {
        remove_all<0>(p);
        reg_unlink(p);
        destroy_node(p);
        --size_;
    }

    // index of the first sequenced/random_access index, or N if there is none
    static constexpr std::size_t first_sequence_index() {
        std::size_t r = N;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (((r == N && (spec_at<I>::kind == index_kind::sequenced ||
                          spec_at<I>::kind == index_kind::random_access)) ? (r = I, true) : false) || ...);
        }(std::make_index_sequence<N>{});
        return r;
    }
    // Rebuild from another container. If a sequenced/random_access index exists,
    // replay in that index's order so its element order is reproduced exactly;
    // otherwise insertion (registry) order, which reproduces every keyed index.
    void copy_from(const multi_index_container& o) {
        constexpr std::size_t S = first_sequence_index();
        if constexpr (S < N) { for (node* p : std::get<S>(o.cores_).c) insert(p->value); }
        else                 { for (node* p = o.rhead_; p; p = p->rnext) insert(p->value); }
    }

public:
    // ---- proxy ------------------------------------------------------------
    template <std::size_t I>
    class index_proxy {
        multi_index_container* c_;
        using Core = core_at<I>;
        static constexpr index_kind kind   = Core::kind;
        static constexpr bool       ranked = spec_at<I>::ranked;
        static constexpr bool       unique = spec_at<I>::unique;
        using key_t = typename detail::core_key<Core>::type;
        // never let 'void' reach a 'const K&' parameter, even under early substitution
        using lookup_key_t = std::conditional_t<std::is_void_v<key_t>, int, key_t>;

        Core& core() const { return std::get<I>(c_->cores_); }
    public:
        using iterator       = iter_t<I>;
        using const_iterator = iter_t<I>;
        using value_type     = Value;

        explicit index_proxy(multi_index_container* c) : c_(c) {}

        static constexpr std::string_view tag() { return spec_at<I>::tag.view(); }

        iterator begin() const { return iterator{ core().c.begin() }; }
        iterator end()   const { return iterator{ core().c.end() }; }
        auto rbegin() const requires (kind != index_kind::hashed) { return std::make_reverse_iterator(end()); }
        auto rend()   const requires (kind != index_kind::hashed) { return std::make_reverse_iterator(begin()); }
        std::size_t size()  const { return c_->size_; }
        bool        empty() const { return c_->size_ == 0; }

        iterator iterator_to(node* p) const { return c_->template make_iter<I>(p); }

        // ---- insertion through this index (Boost-style); returns this index's
        //      iterator to the new element, or to the conflicting one on failure.
        template <class V> std::pair<iterator, bool> insert(V&& v) const {
            auto [it0, ok] = c_->insert(std::forward<V>(v));
            return { c_->template make_iter<I>(it0.get_node()), ok };
        }

        // ---- mutation (delegates across all indices) ----
        template <class F> bool modify(iterator it, F&& f) const { return c_->modify_node(it.get_node(), std::forward<F>(f)); }
        template <class V> bool replace(iterator it, V&& v) const { return c_->replace_node(it.get_node(), std::forward<V>(v)); }
        iterator erase(iterator it) const {
            node* p = it.get_node();
            iterator nx = it; ++nx;
            node* nextNode = (nx == end()) ? nullptr : nx.get_node();
            c_->erase_node_full(p);
            return nextNode ? c_->template make_iter<I>(nextNode) : end();
        }

        // ---- ordered / ranked lookups ----
        template <class K = lookup_key_t> iterator find(const K& k) const requires (kind == index_kind::ordered) { return iterator{ core().c.find(k) }; }
        template <class K = lookup_key_t> std::size_t count(const K& k) const requires (kind == index_kind::ordered) { return core().c.count(k); }
        template <class K = lookup_key_t> bool contains(const K& k) const requires (kind == index_kind::ordered) { return core().c.find(k) != core().c.end(); }
        template <class K = lookup_key_t> iterator lower_bound(const K& k) const requires (kind == index_kind::ordered) { return iterator{ core().c.lower_bound(k) }; }
        template <class K = lookup_key_t> iterator upper_bound(const K& k) const requires (kind == index_kind::ordered) { return iterator{ core().c.upper_bound(k) }; }
        template <class K = lookup_key_t> std::pair<iterator, iterator> equal_range(const K& k) const requires (kind == index_kind::ordered) {
            auto pr = core().c.equal_range(k);
            return { iterator{ pr.first }, iterator{ pr.second } };
        }

        // ---- hashed lookups (transparent: no key materialisation when possible) ----
        template <class K = lookup_key_t> iterator find(const K& k) const requires (kind == index_kind::hashed) {
            auto& c = core().c;
            if constexpr (requires { c.find(k); }) return iterator{ c.find(k) };
            else { key_t kk(k); return iterator{ c.find(kk) }; }
        }
        template <class K = lookup_key_t> std::size_t count(const K& k) const requires (kind == index_kind::hashed) {
            auto& c = core().c;
            if constexpr (requires { c.count(k); }) return c.count(k);
            else { key_t kk(k); return c.count(kk); }
        }
        template <class K = lookup_key_t> bool contains(const K& k) const requires (kind == index_kind::hashed) {
            return find(k) != end();
        }
        template <class K = lookup_key_t> std::pair<iterator, iterator> equal_range(const K& k) const requires (kind == index_kind::hashed) {
            auto& c = core().c;
            if constexpr (requires { c.equal_range(k); }) { auto pr = c.equal_range(k); return { iterator{ pr.first }, iterator{ pr.second } }; }
            else { key_t kk(k); auto pr = c.equal_range(kk); return { iterator{ pr.first }, iterator{ pr.second } }; }
        }
        std::size_t bucket_count() const requires (kind == index_kind::hashed) { return core().c.bucket_count(); }
        float load_factor() const requires (kind == index_kind::hashed) { return core().c.load_factor(); }

        // ---- erase by key (ordered & hashed) ----
        template <class K = lookup_key_t>
        std::size_t erase_key(const K& k) const requires (kind == index_kind::ordered || kind == index_kind::hashed) {
            std::vector<node*> victims;
            auto& c = core().c;
            if constexpr (kind == index_kind::hashed && !requires { c.equal_range(k); }) {
                key_t kk(k);
                auto pr = c.equal_range(kk);
                for (auto it = pr.first; it != pr.second; ++it) victims.push_back(iterator{ it }.get_node());
            } else {
                auto pr = c.equal_range(k);
                for (auto it = pr.first; it != pr.second; ++it) victims.push_back(iterator{ it }.get_node());
            }
            for (node* p : victims) c_->erase_node_full(p);
            return victims.size();
        }

        // ---- ranked extras (O(log n) via the order-statistics tree) ----
        iterator nth(std::size_t n) const requires (ranked) { return iterator{ core().c.nth(n) }; }
        std::size_t rank(iterator it) const requires (ranked) { return core().c.rank(it.base()); }
        // test/diagnostic hook: height of the backing AVL tree (ordered/ranked indices)
        std::int32_t tree_height() const requires (kind == index_kind::ordered) { return core().c.root_height(); }

        // ---- sequenced ops ----
        const Value& front() const requires (kind == index_kind::sequenced || kind == index_kind::random_access) { return *begin(); }
        const Value& back()  const requires (kind == index_kind::sequenced || kind == index_kind::random_access) { auto e = end(); --e; return *e; }

        template <class V> std::pair<iterator, bool> push_back(V&& v) const
            requires (kind == index_kind::sequenced || kind == index_kind::random_access) {
            auto [it0, ok] = c_->insert(std::forward<V>(v));
            return { c_->template make_iter<I>(it0.get_node()), ok };  // conflicting node on failure
        }
        template <class V> std::pair<iterator, bool> push_front(V&& v) const requires (kind == index_kind::sequenced) {
            auto r = push_back(std::forward<V>(v));
            if (r.second) { core().c.move_to_front(r.first.get_node()); }
            return r;
        }
        void pop_front() const requires (kind == index_kind::sequenced) { erase(begin()); }
        void pop_back()  const requires (kind == index_kind::sequenced) { auto e = end(); --e; erase(e); }
        void relocate(iterator pos, iterator it) const requires (kind == index_kind::sequenced) {
            core().c.relocate(pos.get_node(), it.get_node());   // move `it` before `pos` (pos==end -> back)
        }
        void reverse() const requires (kind == index_kind::sequenced) { core().c.reverse(); }

        // ---- random-access ops ----
        const Value& operator[](std::size_t n) const requires (kind == index_kind::random_access) { return core().c[n]->value; }
        const Value& at(std::size_t n) const requires (kind == index_kind::random_access) { return core().c.at(n)->value; }
        iterator nth_at(std::size_t n) const requires (kind == index_kind::random_access) { return iterator{ core().c.begin() + static_cast<std::ptrdiff_t>(n) }; }
        std::size_t index_of(iterator it) const requires (kind == index_kind::random_access) {
            return static_cast<std::size_t>(it.base() - core().c.begin());
        }
        std::size_t capacity() const requires (kind == index_kind::random_access) { return core().c.capacity(); }
        void reserve(std::size_t n) const requires (kind == index_kind::random_access) { core().c.reserve(n); }
    };

    // ---- node handle ------------------------------------------------------
    //  Self-sufficient: owns its node AND a copy of the node allocator, so it
    //  can free the node even if its source container is already gone.
    class node_handle {
        MIC_NO_UNIQUE_ADDRESS node_alloc alloc_{};
        node* p_ = nullptr;
        friend class multi_index_container;
    public:
        node_handle() = default;
        node_handle(const node_alloc& a, node* p) : alloc_(a), p_(p) {}
        node_handle(node_handle&& o) noexcept : alloc_(std::move(o.alloc_)), p_(o.p_) { o.p_ = nullptr; }
        node_handle& operator=(node_handle&& o) noexcept {
            if (this != &o) { reset(); alloc_ = std::move(o.alloc_); p_ = o.p_; o.p_ = nullptr; }
            return *this;
        }
        node_handle(const node_handle&) = delete;
        node_handle& operator=(const node_handle&) = delete;
        ~node_handle() { reset(); }
        void reset() {
            if (p_) {
                std::allocator_traits<node_alloc>::destroy(alloc_, p_);
                std::allocator_traits<node_alloc>::deallocate(alloc_, p_, 1);
                p_ = nullptr;
            }
        }
        bool empty() const { return p_ == nullptr; }
        explicit operator bool() const { return p_ != nullptr; }
        Value& value() const { return p_->value; }
    };
    struct insert_return_type {
        iter_t<0>   position;
        bool        inserted;
        node_handle node;
    };

    // ---- ctors ------------------------------------------------------------
    multi_index_container() = default;
    explicit multi_index_container(const Alloc& a) : alloc_(a) {}   // nalloc_/cores_ cascade from alloc_

    multi_index_container(const multi_index_container& o)
        : alloc_(std::allocator_traits<Alloc>::select_on_container_copy_construction(o.alloc_)) { copy_from(o); }
    multi_index_container(const multi_index_container& o, const Alloc& a) : alloc_(a) { copy_from(o); }
    multi_index_container(multi_index_container&& o) noexcept
        : alloc_(std::move(o.alloc_)), nalloc_(std::move(o.nalloc_)), cores_(std::move(o.cores_)),
          rhead_(o.rhead_), rtail_(o.rtail_), size_(o.size_) { o.rhead_ = o.rtail_ = nullptr; o.size_ = 0; }
    multi_index_container& operator=(const multi_index_container& o) {
        if (this != &o) { clear(); copy_from(o); }
        return *this;
    }
    multi_index_container& operator=(multi_index_container&& o) noexcept {
        if (this != &o) {
            clear();
            alloc_ = std::move(o.alloc_); nalloc_ = std::move(o.nalloc_); cores_ = std::move(o.cores_);
            rhead_ = o.rhead_; rtail_ = o.rtail_; size_ = o.size_;
            o.rhead_ = o.rtail_ = nullptr; o.size_ = 0;
        }
        return *this;
    }
    ~multi_index_container() { clear(); }

    template <std::input_iterator It>
    multi_index_container(It first, It last, const Alloc& a = Alloc{}) : alloc_(a) { for (; first != last; ++first) insert(*first); }
    multi_index_container(std::initializer_list<Value> il, const Alloc& a = Alloc{}) : alloc_(a) { for (const auto& v : il) insert(v); }
    template <std::ranges::input_range R>
        requires std::constructible_from<Value, std::ranges::range_reference_t<R>>
    multi_index_container(std::from_range_t, R&& r, const Alloc& a = Alloc{}) : alloc_(a) { for (auto&& v : r) emplace(std::forward<decltype(v)>(v)); }

    Alloc get_allocator() const noexcept { return alloc_; }

    // ---- capacity ---------------------------------------------------------
    std::size_t size()  const noexcept { return size_; }
    bool        empty() const noexcept { return size_ == 0; }
    void clear() {
        for (node* p = rhead_; p; ) { node* nx = p->rnext; destroy_node(p); p = nx; }
        rhead_ = rtail_ = nullptr;
        std::apply([](auto&... co) { (co.c.clear(), ...); }, cores_);
        size_ = 0;
    }
    void swap(multi_index_container& o) noexcept {
        using std::swap;
        swap(alloc_, o.alloc_);   swap(nalloc_, o.nalloc_); swap(rhead_, o.rhead_); swap(rtail_, o.rtail_);
        swap(cores_, o.cores_);   swap(size_, o.size_);
    }
    friend void swap(multi_index_container& a, multi_index_container& b) noexcept { a.swap(b); }

    // ---- index access -----------------------------------------------------
    template <fixed_string Tag> auto get() {
        constexpr std::size_t I = index_of_tag<Tag>();
        static_assert(I < N, "mic: unknown index tag");
        return index_proxy<I>{ this };
    }
    template <std::size_t I> auto get() {
        static_assert(I < N, "mic: index number out of range");
        return index_proxy<I>{ this };
    }
    template <fixed_string Tag> auto index() { return get<Tag>(); }
    template <std::size_t I>     auto index() { return get<I>(); }

    template <fixed_string ToTag, class It> auto project(It it) {
        constexpr std::size_t J = index_of_tag<ToTag>();
        static_assert(J < N, "mic: unknown projection tag");
        return make_iter<J>(it.get_node());
    }

    // ---- container-level begin/end (index 0) ------------------------------
    using iterator       = iter_t<0>;
    using const_iterator = iter_t<0>;
    iterator begin() { return make_begin0(); }
    iterator end()   { return end_iter<0>(); }
    iterator begin() const { return const_cast<multi_index_container*>(this)->make_begin0(); }
    iterator end()   const { return const_cast<multi_index_container*>(this)->end_iter<0>(); }

private:
    iterator make_begin0() { return iterator{ std::get<0>(cores_).c.begin() }; }
public:

    // ---- insert / emplace -------------------------------------------------
    std::pair<iterator, bool> insert(const Value& v) { return insert_node(make_node(v)); }
    std::pair<iterator, bool> insert(Value&& v)      { return insert_node(make_node(std::move(v))); }
    template <class... A> std::pair<iterator, bool> emplace(A&&... a) { return insert_node(make_node(std::forward<A>(a)...)); }
    template <std::input_iterator It> void insert(It first, It last) { for (; first != last; ++first) insert(*first); }
    void insert(std::initializer_list<Value> il) { for (const auto& v : il) insert(v); }

    std::expected<iterator, insert_error<Value>> try_insert(const Value& v) { return try_insert_impl(make_node(v)); }
    std::expected<iterator, insert_error<Value>> try_insert(Value&& v)      { return try_insert_impl(make_node(std::move(v))); }
private:
    std::expected<iterator, insert_error<Value>> try_insert_impl(node* p) {
        node* conflict = nullptr; std::size_t failed = N;
        if (!insert_all<0>(p, conflict, failed)) {
            destroy_node(p);
            insert_error<Value> e;
            e.index_pos = failed;
            e.blocking  = conflict ? &conflict->value : nullptr;
            visit_tag(failed, [&](std::string_view t) { e.index_tag = t; });
            return std::unexpected(e);
        }
        reg_push_back(p); ++size_;
        return make_iter<0>(p);
    }
    template <class F> void visit_tag(std::size_t pos, F&& f) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((I == pos ? (f(spec_at<I>::tag.view()), true) : false) || ...);
        }(std::make_index_sequence<N>{});
    }
public:

    // ---- erase ------------------------------------------------------------
    iterator erase(iterator pos) {
        node* p = pos.get_node();
        iterator nx = pos; ++nx;
        node* nextNode = (nx == end()) ? nullptr : nx.get_node();
        erase_node_full(p);
        return nextNode ? make_iter<0>(nextNode) : end_iter<0>();
    }

    // ---- node-handle transfer & merge ------------------------------------
    node_handle extract(iterator pos) {
        node* p = pos.get_node();
        remove_all<0>(p); reg_unlink(p); --size_;
        return node_handle{ nalloc_, p };
    }
    insert_return_type insert(node_handle&& h) {
        if (!h) return { end_iter<0>(), false, node_handle{} };
        node* p = h.p_;
        node* conflict = nullptr; std::size_t failed = N;
        if (!insert_all<0>(p, conflict, failed)) {
            return { make_iter<0>(conflict), false, std::move(h) };
        }
        h.p_ = nullptr;   // ownership adopted by this container
        reg_push_back(p); ++size_;
        return { make_iter<0>(p), true, node_handle{} };
    }
    void merge(multi_index_container& other) {
        if (this == &other) return;
        std::vector<node*> all;
        for (node* p = other.rhead_; p; p = p->rnext) all.push_back(p);
        for (node* p : all) {
            other.remove_all<0>(p); other.reg_unlink(p); --other.size_;
            node_handle h{ other.nalloc_, p };
            auto r = insert(std::move(h));
            if (!r.inserted) (void)other.insert(std::move(r.node));
        }
    }
};

template <class V, class I, class A, class P>
void swap(multi_index_container<V, I, A, P>& a, multi_index_container<V, I, A, P>& b) noexcept { a.swap(b); }

// Convenience alias: a container whose iterators present pointer-like elements
// as a const pointee view (keys can only change via modify()/replace()).
template <class Value, class IndexedBy, class Alloc = std::allocator<Value>>
using const_element_container = multi_index_container<Value, IndexedBy, Alloc, const_element_policy>;

// std::pmr support: every allocation (element nodes AND the per-index structures)
// flows through one std::pmr::memory_resource, so a pool / monotonic_buffer
// resource serves the whole container. Construct with a memory_resource*.
namespace pmr {
template <class Value, class IndexedBy, class Policy = ::mic::standard_policy>
using multi_index_container =
    ::mic::multi_index_container<Value, IndexedBy, std::pmr::polymorphic_allocator<Value>, Policy>;
}

} // namespace mic

// ---- std::format summary ---------------------------------------------------
template <class V, class I, class A, class P>
struct std::formatter<mic::multi_index_container<V, I, A, P>> : std::formatter<std::string> {
    auto format(const mic::multi_index_container<V, I, A, P>& m, std::format_context& ctx) const {
        return std::formatter<std::string>::format(std::format("multi_index(size={})", m.size()), ctx);
    }
};

#endif // MIC_MULTI_INDEX_HPP
