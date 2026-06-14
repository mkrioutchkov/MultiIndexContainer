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
//  NOTE ON INTERNALS (v1): each element is stored once in a stable node and
//  threaded into every index via a back-hook, so element addresses and
//  iterators stay valid until the element is erased. Ordered/ranked indices are
//  red-black trees (std::set/std::multiset), hashed indices are bucketed hash
//  tables (std::unordered_set/_multiset), sequenced is a doubly-linked list and
//  random-access is a pointer vector. Ranked rank()/nth() are O(n) in v1 (a
//  documented limitation; an order-statistics tree is the planned upgrade).
// =============================================================================
#ifndef MIC_MULTI_INDEX_HPP
#define MIC_MULTI_INDEX_HPP

#include <set>
#include <unordered_set>
#include <list>
#include <vector>
#include <tuple>
#include <array>
#include <memory>
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

// ---------------------------------------------------------------------------
//  Index specifications
// ---------------------------------------------------------------------------
enum class index_kind { ordered, hashed, sequenced, random_access };

namespace detail {
template <class E> using ekey = std::remove_cvref_t<typename E::result_type>;
template <class E> using default_ordered_compare =
    std::conditional_t<is_tuple<ekey<E>>::value, composite_less, std::less<>>;
template <class E> using default_hash =
    std::conditional_t<is_tuple<ekey<E>>::value, composite_hash, std::hash<ekey<E>>>;
template <class E> using default_equal =
    std::conditional_t<is_tuple<ekey<E>>::value, composite_equal, std::equal_to<ekey<E>>>;
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

template <class NodePtr, class Extractor, class Compare, bool Unique>
struct ordered_core {
    static constexpr index_kind kind = index_kind::ordered;
    using key_type = std::remove_cvref_t<typename Extractor::result_type>;

    template <class T> static decltype(auto) keyof(const T& t) {
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, NodePtr>) return Extractor{}((*t).value);
        else return (t);
    }
    struct node_compare {
        using is_transparent = void;
        template <class A, class B> bool operator()(const A& a, const B& b) const {
            return Compare{}(keyof(a), keyof(b));
        }
    };
    using container = std::conditional_t<Unique,
        std::set<NodePtr, node_compare>, std::multiset<NodePtr, node_compare>>;
    using hook_type = typename container::iterator;
    container c{};

    bool try_insert(NodePtr p, hook_type& out, NodePtr& conflict) {
        if constexpr (Unique) {
            auto [it, ok] = c.insert(p);
            if (!ok) { conflict = *it; return false; }
            out = it; return true;
        } else {
            out = c.insert(p); return true;
        }
    }
    void remove(NodePtr p, hook_type& h) { (void)p; c.erase(h); }
};

template <class NodePtr, class Extractor, class Hash, class Eq, bool Unique>
struct hashed_core {
    static constexpr index_kind kind = index_kind::hashed;
    using key_type = std::remove_cvref_t<typename Extractor::result_type>;

    template <class T> static decltype(auto) keyof(const T& t) {
        if constexpr (std::is_same_v<std::remove_cvref_t<T>, NodePtr>) return Extractor{}((*t).value);
        else return (t);
    }
    struct node_hash {
        using is_transparent = void;
        template <class T> std::size_t operator()(const T& t) const { return Hash{}(keyof(t)); }
    };
    struct node_eq {
        using is_transparent = void;
        template <class A, class B> bool operator()(const A& a, const B& b) const {
            return Eq{}(keyof(a), keyof(b));
        }
    };
    using container = std::conditional_t<Unique,
        std::unordered_set<NodePtr, node_hash, node_eq>,
        std::unordered_multiset<NodePtr, node_hash, node_eq>>;
    using hook_type = typename container::iterator;
    container c{};

    bool try_insert(NodePtr p, hook_type& out, NodePtr& conflict) {
        if constexpr (Unique) {
            auto [it, ok] = c.insert(p);
            if (!ok) { conflict = *it; return false; }
            out = it; return true;
        } else {
            out = c.insert(p); return true;
        }
    }
    void remove(NodePtr p, hook_type& h) { (void)p; c.erase(h); }
};

template <class NodePtr>
struct seq_core {
    static constexpr index_kind kind = index_kind::sequenced;
    using container = std::list<NodePtr>;
    using hook_type = typename container::iterator;
    container c{};
    bool try_insert(NodePtr p, hook_type& out, NodePtr&) { c.push_back(p); out = std::prev(c.end()); return true; }
    void remove(NodePtr, hook_type& h) { c.erase(h); }
};

template <class NodePtr>
struct rand_core {
    static constexpr index_kind kind = index_kind::random_access;
    using container = std::vector<NodePtr>;
    using hook_type = std::monostate;
    container c{};
    bool try_insert(NodePtr p, hook_type&, NodePtr&) { c.push_back(p); return true; }
    void remove(NodePtr p, hook_type&) {
        auto it = std::find(c.begin(), c.end(), p);
        if (it != c.end()) c.erase(it);
    }
};

// map a spec to its core type
template <class Spec, class NodePtr> struct core_for;
template <fixed_string T, class E, class C, class NP>
struct core_for<ordered_unique<T, E, C>, NP> { using type = ordered_core<NP, E, C, true>; };
template <fixed_string T, class E, class C, class NP>
struct core_for<ordered_non_unique<T, E, C>, NP> { using type = ordered_core<NP, E, C, false>; };
template <fixed_string T, class E, class C, class NP>
struct core_for<ranked_unique<T, E, C>, NP> { using type = ordered_core<NP, E, C, true>; };
template <fixed_string T, class E, class C, class NP>
struct core_for<ranked_non_unique<T, E, C>, NP> { using type = ordered_core<NP, E, C, false>; };
template <fixed_string T, class E, class H, class Q, class NP>
struct core_for<hashed_unique<T, E, H, Q>, NP> { using type = hashed_core<NP, E, H, Q, true>; };
template <fixed_string T, class E, class H, class Q, class NP>
struct core_for<hashed_non_unique<T, E, H, Q>, NP> { using type = hashed_core<NP, E, H, Q, false>; };
template <fixed_string T, class NP>
struct core_for<sequenced<T>, NP> { using type = seq_core<NP>; };
template <fixed_string T, class NP>
struct core_for<random_access<T>, NP> { using type = rand_core<NP>; };

template <class C, class = void> struct core_key { using type = void; };
template <class C> struct core_key<C, std::void_t<typename C::key_type>> { using type = typename C::key_type; };

} // namespace detail

// ---------------------------------------------------------------------------
//  Error type for the std::expected-flavoured API
// ---------------------------------------------------------------------------
struct insert_error {
    enum class reason { duplicate_in_unique_index };
    reason       why = reason::duplicate_in_unique_index;
    std::size_t  index_pos = 0;
    std::string_view index_tag;
};

// ===========================================================================
//  multi_index_container
// ===========================================================================
template <class Value, class IndexedBy, class Alloc = std::allocator<Value>>
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

    struct node;
    template <std::size_t I> using core_at = typename detail::core_for<spec_at<I>, node*>::type;

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

    struct node {
        Value     value;
        hook_tuple hooks{};
        typename std::list<node*>::iterator self{};
        template <class... A>
        explicit node(A&&... a) : value(std::forward<A>(a)...) {}
    };

    using node_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<node>;

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

    MIC_NO_UNIQUE_ADDRESS node_alloc nalloc_{};
    std::list<node*> registry_{};
    cores_tuple      cores_{};
    std::size_t      size_{0};

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
        using value_type        = Value;
        using reference         = const Value&;
        using pointer           = const Value*;
        using difference_type   = std::ptrdiff_t;
        using iterator_category = typename std::iterator_traits<It>::iterator_category;
        using iterator_concept  = std::conditional_t<std::random_access_iterator<It>, std::random_access_iterator_tag,
            std::conditional_t<std::bidirectional_iterator<It>, std::bidirectional_iterator_tag,
            std::conditional_t<std::forward_iterator<It>, std::forward_iterator_tag, std::input_iterator_tag>>>;

        index_iterator() = default;
        explicit index_iterator(It it) : it_(it) {}

        reference operator*()  const { return (*it_)->value; }
        pointer   operator->() const { return &(*it_)->value; }

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
        node* get_node() const { return *it_; }
    };

private:
    template <std::size_t I> using iter_t = index_iterator<typename core_at<I>::container::iterator>;

    template <std::size_t I> iter_t<I> make_iter(node* p) {
        using Core = core_at<I>;
        if constexpr (Core::kind == index_kind::random_access) {
            auto& c = std::get<I>(cores_).c;
            return iter_t<I>{ std::find(c.begin(), c.end(), p) };
        } else {
            return iter_t<I>{ std::get<I>(p->hooks) };
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
        registry_.push_back(p); p->self = std::prev(registry_.end()); ++size_;
        return { make_iter<0>(p), true };
    }

    // Re-thread p under its current keys; if that somehow fails (a throwing
    // comparator/hasher, bad_alloc), drop the node so the invariant
    // "registered == indexed in every core" is never violated.
    void rethread_or_drop(node* p) {
        node* c = nullptr; std::size_t f = N;
        if (!insert_all<0>(p, c, f)) { registry_.erase(p->self); destroy_node(p); --size_; }
    }
    template <class F>
    bool modify_node(node* p, F&& f) {
        static_assert(std::is_copy_constructible_v<Value>,
                      "modify() rollback requires a copy-constructible value_type");
        remove_all<0>(p);
        Value snapshot = p->value;
        try {
            std::forward<F>(f)(p->value);
        } catch (...) {                    // mutator threw: restore and re-thread
            p->value = std::move(snapshot);
            rethread_or_drop(p);
            throw;
        }
        node* conflict = nullptr; std::size_t failed = N;
        if (insert_all<0>(p, conflict, failed)) return true;
        p->value = std::move(snapshot);    // rollback-and-keep
        rethread_or_drop(p);
        return false;
    }
    template <class V>
    bool replace_node(node* p, V&& v) {
        static_assert(std::is_move_constructible_v<Value>,
                      "replace() requires a move-constructible value_type for rollback");
        remove_all<0>(p);
        Value snapshot = std::move(p->value);
        try {
            p->value = std::forward<V>(v);
        } catch (...) {
            p->value = std::move(snapshot);
            rethread_or_drop(p);
            throw;
        }
        node* conflict = nullptr; std::size_t failed = N;
        if (insert_all<0>(p, conflict, failed)) return true;
        p->value = std::move(snapshot);
        rethread_or_drop(p);
        return false;
    }
    void erase_node_full(node* p) {
        remove_all<0>(p);
        registry_.erase(p->self);
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
        else                 { for (node* p : o.registry_) insert(p->value); }
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

        // ---- hashed lookups (key materialised to key_t) ----
        template <class K = lookup_key_t> iterator find(const K& k) const requires (kind == index_kind::hashed) { key_t kk(k); return iterator{ core().c.find(kk) }; }
        template <class K = lookup_key_t> std::size_t count(const K& k) const requires (kind == index_kind::hashed) { key_t kk(k); return core().c.count(kk); }
        template <class K = lookup_key_t> bool contains(const K& k) const requires (kind == index_kind::hashed) { key_t kk(k); return core().c.find(kk) != core().c.end(); }
        template <class K = lookup_key_t> std::pair<iterator, iterator> equal_range(const K& k) const requires (kind == index_kind::hashed) {
            key_t kk(k);
            auto pr = core().c.equal_range(kk);
            return { iterator{ pr.first }, iterator{ pr.second } };
        }
        std::size_t bucket_count() const requires (kind == index_kind::hashed) { return core().c.bucket_count(); }
        float load_factor() const requires (kind == index_kind::hashed) { return core().c.load_factor(); }

        // ---- erase by key (ordered & hashed) ----
        template <class K = lookup_key_t>
        std::size_t erase_key(const K& k) const requires (kind == index_kind::ordered || kind == index_kind::hashed) {
            std::vector<node*> victims;
            if constexpr (kind == index_kind::hashed) {
                key_t kk(k);
                auto pr = core().c.equal_range(kk);
                for (auto it = pr.first; it != pr.second; ++it) victims.push_back(*it);
            } else {
                auto pr = core().c.equal_range(k);
                for (auto it = pr.first; it != pr.second; ++it) victims.push_back(*it);
            }
            for (node* p : victims) c_->erase_node_full(p);
            return victims.size();
        }

        // ---- ranked extras (O(n) in v1) ----
        iterator nth(std::size_t n) const requires (ranked) {
            auto it = core().c.begin(); std::advance(it, static_cast<std::ptrdiff_t>(n)); return iterator{ it };
        }
        std::size_t rank(iterator it) const requires (ranked) {
            return static_cast<std::size_t>(std::distance(core().c.begin(), it.base()));
        }

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
            if (r.second) { core().c.splice(core().c.begin(), core().c, r.first.base()); }
            return r;
        }
        void pop_front() const requires (kind == index_kind::sequenced) { erase(begin()); }
        void pop_back()  const requires (kind == index_kind::sequenced) { auto e = end(); --e; erase(e); }
        void relocate(iterator pos, iterator it) const requires (kind == index_kind::sequenced) {
            core().c.splice(pos.base(), core().c, it.base());
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
    explicit multi_index_container(const Alloc& a) : nalloc_(a) {}

    multi_index_container(const multi_index_container& o) { copy_from(o); }
    multi_index_container(multi_index_container&& o) noexcept
        : nalloc_(std::move(o.nalloc_)), registry_(std::move(o.registry_)),
          cores_(std::move(o.cores_)), size_(o.size_) { o.size_ = 0; }
    multi_index_container& operator=(const multi_index_container& o) {
        if (this != &o) { clear(); copy_from(o); }
        return *this;
    }
    multi_index_container& operator=(multi_index_container&& o) noexcept {
        if (this != &o) {
            clear();
            nalloc_ = std::move(o.nalloc_); registry_ = std::move(o.registry_);
            cores_ = std::move(o.cores_); size_ = o.size_; o.size_ = 0;
        }
        return *this;
    }
    ~multi_index_container() { clear(); }

    template <std::input_iterator It>
    multi_index_container(It first, It last) { for (; first != last; ++first) insert(*first); }
    multi_index_container(std::initializer_list<Value> il) { for (const auto& v : il) insert(v); }
    template <std::ranges::input_range R>
        requires std::constructible_from<Value, std::ranges::range_reference_t<R>>
    multi_index_container(std::from_range_t, R&& r) { for (auto&& v : r) emplace(std::forward<decltype(v)>(v)); }

    // ---- capacity ---------------------------------------------------------
    std::size_t size()  const noexcept { return size_; }
    bool        empty() const noexcept { return size_ == 0; }
    void clear() {
        for (node* p : registry_) destroy_node(p);
        registry_.clear();
        std::apply([](auto&... co) { (co.c.clear(), ...); }, cores_);
        size_ = 0;
    }
    void swap(multi_index_container& o) noexcept {
        using std::swap;
        swap(nalloc_, o.nalloc_); swap(registry_, o.registry_);
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

    std::expected<iterator, insert_error> try_insert(const Value& v) { return try_insert_impl(make_node(v)); }
    std::expected<iterator, insert_error> try_insert(Value&& v)      { return try_insert_impl(make_node(std::move(v))); }
private:
    std::expected<iterator, insert_error> try_insert_impl(node* p) {
        node* conflict = nullptr; std::size_t failed = N;
        if (!insert_all<0>(p, conflict, failed)) {
            destroy_node(p);
            insert_error e;
            e.index_pos = failed;
            visit_tag(failed, [&](std::string_view t) { e.index_tag = t; });
            return std::unexpected(e);
        }
        registry_.push_back(p); p->self = std::prev(registry_.end()); ++size_;
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
        remove_all<0>(p); registry_.erase(p->self); --size_;
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
        registry_.push_back(p); p->self = std::prev(registry_.end()); ++size_;
        return { make_iter<0>(p), true, node_handle{} };
    }
    void merge(multi_index_container& other) {
        if (this == &other) return;
        std::vector<node*> all(other.registry_.begin(), other.registry_.end());
        for (node* p : all) {
            other.remove_all<0>(p); other.registry_.erase(p->self); --other.size_;
            node_handle h{ other.nalloc_, p };
            auto r = insert(std::move(h));
            if (!r.inserted) (void)other.insert(std::move(r.node));
        }
    }
};

template <class V, class I, class A>
void swap(multi_index_container<V, I, A>& a, multi_index_container<V, I, A>& b) noexcept { a.swap(b); }

} // namespace mic

// ---- std::format summary ---------------------------------------------------
template <class V, class I, class A>
struct std::formatter<mic::multi_index_container<V, I, A>> : std::formatter<std::string> {
    auto format(const mic::multi_index_container<V, I, A>& m, std::format_context& ctx) const {
        return std::formatter<std::string>::format(std::format("multi_index(size={})", m.size()), ctx);
    }
};

#endif // MIC_MULTI_INDEX_HPP
