// =============================================================================
//  examples/observer_registry.cpp
//
//  An observer/subscriber registry: callbacks registered with a stable id, kept
//  in registration order, notified in that order, and removable by id. Two
//  indices over one Subscriber element:
//
//    * ordered_unique<"by_id">    -> O(log n) register / deregister, no dup ids
//    * sequenced     <"by_order"> -> iterate in registration order to notify
//
//  The point: deregistering one subscriber does NOT disturb the order or the
//  validity of the others — node-based storage keeps every other element put.
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\observer_registry.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <cassert>
#include <functional>
#include <print>
#include <string>
#include <vector>

struct Subscriber {
    int                            id;
    std::string                    label;
    std::function<void(int)>       on_event;   // not a key; the payload
};

using Registry = mic::multi_index_container<Subscriber,
    mic::indexed_by<
        mic::ordered_unique<"by_id",    mic::key<&Subscriber::id>>,
        mic::sequenced     <"by_order">
    >>;

class EventBus {
    Registry subs_;
    int      next_id_ = 1;
public:
    // Returns the id you can later unsubscribe with.
    int subscribe(std::string label, std::function<void(int)> fn) {
        int id = next_id_++;
        subs_.get<"by_order">().push_back(Subscriber{id, std::move(label), std::move(fn)});
        return id;
    }
    bool unsubscribe(int id) { return subs_.get<"by_id">().erase_key(id) > 0; }

    // Notify every subscriber in registration order.
    void publish(int value) const {
        for (const Subscriber& s : const_cast<Registry&>(subs_).get<"by_order">())
            s.on_event(value);
    }
    std::vector<std::string> order() const {
        std::vector<std::string> out;
        for (const Subscriber& s : const_cast<Registry&>(subs_).get<"by_order">())
            out.push_back(s.label);
        return out;
    }
    std::size_t size() const { return subs_.size(); }
};

int main() {
    EventBus bus;
    std::vector<std::pair<std::string, int>> log;   // (label, value) the callbacks recorded

    int a = bus.subscribe("audit",   [&](int v) { log.emplace_back("audit", v); });
    int b = bus.subscribe("metrics", [&](int v) { log.emplace_back("metrics", v); });
    int c = bus.subscribe("cache",   [&](int v) { log.emplace_back("cache", v); });
    (void)a; (void)c;

    std::println("subscribers (in order): {}", bus.size());
    for (const auto& l : bus.order()) std::print(" {}", l);
    std::println("");

    // publish -> callbacks fire in registration order
    log.clear();
    bus.publish(42);
    std::print("publish(42) fired:");
    for (auto& [label, v] : log) std::print(" {}({})", label, v);
    std::println("");
    assert((log == std::vector<std::pair<std::string,int>>{{"audit",42},{"metrics",42},{"cache",42}}));

    // deregister the middle subscriber; order and the rest are undisturbed
    bus.unsubscribe(b);
    std::println("after unsubscribe(metrics): {} left ->", bus.size());
    for (const auto& l : bus.order()) std::print(" {}", l);
    std::println("");
    assert((bus.order() == std::vector<std::string>{"audit", "cache"}));

    log.clear();
    bus.publish(7);
    assert((log == std::vector<std::pair<std::string,int>>{{"audit",7},{"cache",7}}));

    std::println("\nAll observer-registry assertions passed.");
    return 0;
}
