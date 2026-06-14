#include "mic/multi_index.hpp"

#include <cassert>
#include <print>
#include <string>
#include <memory>
#include <ranges>
#include <algorithm>

namespace {

int g_checks = 0;
int g_fail   = 0;
void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_fail; std::println("  FAIL: {}", what); }
}
#define CHECK(x) check((x), #x)

struct Employee {
    std::uint64_t id;
    std::string   name;
    std::string   email;
    int           salary;
    std::string full_name() const { return name; }   // member-function key target
};

// ---------------------------------------------------------------------------
void test_basic_and_member_function_pointer() {
    std::println("[test] basic + member-function-pointer extractor");
    using Table = mic::multi_index_container<Employee,
        mic::indexed_by<
            mic::ordered_unique    <"by_id",    mic::key<&Employee::id>>,
            mic::ordered_non_unique<"by_name",  mic::key<&Employee::name>>,
            mic::hashed_unique     <"by_email", mic::key<&Employee::email>>,
            // member function pointer syntax via const_mem_fun:
            mic::ordered_non_unique<"by_full",  mic::const_mem_fun<Employee, std::string, &Employee::full_name>>,
            mic::sequenced         <"by_arrival">,
            mic::random_access     <"by_pos">
        >>;

    Table t;
    auto [it, ok] = t.insert(Employee{1, "Ada", "ada@x.io", 180000});
    CHECK(ok);
    t.emplace(2, "Linus", "linus@x.io", 170000);
    t.insert(Employee{3, "Ada", "ada2@x.io", 90000}); // duplicate name allowed (non-unique)

    CHECK(t.size() == 3);

    // duplicate id rejected by by_id, blocking element surfaced
    auto [it2, ok2] = t.insert(Employee{1, "Clone", "clone@x.io", 1});
    CHECK(!ok2);
    CHECK(it2->name == "Ada");
    CHECK(t.size() == 3);

    // lookups
    CHECK(t.get<"by_email">().find("ada@x.io")->name == "Ada");
    CHECK(t.get<"by_name">().count("Ada") == 2);
    CHECK(t.get<"by_full">().count("Ada") == 2);            // computed key
    CHECK(t.get<"by_id">().contains(2));

    // numeric (Boost-style) access still works -> superset
    CHECK(t.get<0>().size() == 3);

    // ordered iteration by id
    std::vector<std::uint64_t> ids;
    for (const Employee& e : t.get<"by_id">()) ids.push_back(e.id);
    CHECK((ids == std::vector<std::uint64_t>{1, 2, 3}));

    // random-access index
    CHECK(t.get<"by_pos">()[0].id == 1);
    CHECK(t.get<"by_pos">().at(1).id == 2);
}

// ---------------------------------------------------------------------------
void test_shared_ptr_elements() {
    std::println("[test] shared_ptr element support (auto-deref extractors)");
    using Ptr = std::shared_ptr<Employee>;
    using Table = mic::multi_index_container<Ptr,
        mic::indexed_by<
            mic::ordered_unique<"by_id",   mic::member<Employee, std::uint64_t, &Employee::id>>,
            mic::hashed_unique <"by_email", mic::const_mem_fun<Employee, std::string, &Employee::full_name>>
        >>;

    Table t;
    t.insert(std::make_shared<Employee>(Employee{10, "Grace", "grace@x.io", 200000}));
    t.insert(std::make_shared<Employee>(Employee{11, "Dennis", "dennis@x.io", 150000}));

    CHECK(t.size() == 2);
    auto f = t.get<"by_id">().find(10);
    CHECK(f != t.get<"by_id">().end());
    CHECK((*f)->name == "Grace");                 // element is shared_ptr<Employee>
    CHECK(t.get<"by_email">().find("Dennis") != t.get<"by_email">().end());
}

// ---------------------------------------------------------------------------
void test_composite_key() {
    std::println("[test] composite keys + prefix range query");
    using Table = mic::multi_index_container<Employee,
        mic::indexed_by<
            mic::ordered_unique    <"by_id", mic::key<&Employee::id>>,
            mic::ordered_non_unique<"by_dept_salary",
                mic::key<&Employee::name, &Employee::salary>>   // (name, salary) composite
        >>;
    Table t;
    t.insert(Employee{1, "Eng", "a", 100});
    t.insert(Employee{2, "Eng", "b", 200});
    t.insert(Employee{3, "Sales", "c", 150});

    // full-key lookup
    CHECK(t.get<"by_dept_salary">().count(std::tuple{std::string("Eng"), 200}) == 1);
    // prefix (partial) lookup: all "Eng" regardless of salary
    auto [lo, hi] = t.get<"by_dept_salary">().equal_range(std::tuple{std::string("Eng")});
    CHECK(std::distance(lo, hi) == 2);
}

// ---------------------------------------------------------------------------
void test_modify_replace_rollback() {
    std::println("[test] modify / replace with rollback-and-keep");
    using Table = mic::multi_index_container<Employee,
        mic::indexed_by<
            mic::ordered_unique<"by_id",    mic::key<&Employee::id>>,
            mic::hashed_unique <"by_email", mic::key<&Employee::email>>
        >>;
    Table t;
    t.insert(Employee{1, "A", "a@x", 1});
    t.insert(Employee{2, "B", "b@x", 2});

    auto it = t.get<"by_id">().find(2);
    bool ok = t.get<"by_id">().modify(it, [](Employee& e){ e.name = "B2"; });
    CHECK(ok);
    CHECK(t.get<"by_id">().find(2)->name == "B2");

    // collide email with element 1 -> rollback, element kept, false returned
    bool bad = t.get<"by_id">().modify(it, [](Employee& e){ e.email = "a@x"; });
    CHECK(!bad);
    CHECK(t.get<"by_id">().find(2)->email == "b@x");
    CHECK(t.size() == 2);

    // replace
    Employee r = *t.get<"by_id">().find(1);
    r.salary = 999;
    CHECK(t.get<"by_id">().replace(t.get<"by_id">().find(1), r));
    CHECK(t.get<"by_id">().find(1)->salary == 999);
}

// ---------------------------------------------------------------------------
void test_projection_and_nodehandle() {
    std::println("[test] projection + node-handle extract/insert + merge");
    using Table = mic::multi_index_container<Employee,
        mic::indexed_by<
            mic::ordered_unique<"by_id",   mic::key<&Employee::id>>,
            mic::sequenced     <"by_seq">
        >>;
    Table t;
    t.insert(Employee{1, "A", "a", 1});
    t.insert(Employee{2, "B", "b", 2});

    auto h = t.get<"by_id">().find(2);
    auto s = t.project<"by_seq">(h);          // O(1) cross-index projection
    CHECK(s->id == 2);

    // node handle: extract, re-key, re-insert
    auto nh = t.extract(t.get<"by_id">().find(1));
    CHECK(t.size() == 1);
    nh.value().id = 100;
    auto ret = t.insert(std::move(nh));
    CHECK(ret.inserted);
    CHECK(t.get<"by_id">().contains(100));
    CHECK(t.size() == 2);

    // merge
    Table other;
    other.insert(Employee{2, "dup", "d", 0});  // collides with t's id 2 -> stays
    other.insert(Employee{7, "new", "n", 0});
    t.merge(other);
    CHECK(t.get<"by_id">().contains(7));
    CHECK(other.get<"by_id">().contains(2));   // rejected element remained behind
}

// ---------------------------------------------------------------------------
void test_expected_and_ranges() {
    std::println("[test] std::expected API + ranges views + ranked");
    using Table = mic::multi_index_container<Employee,
        mic::indexed_by<
            mic::ranked_non_unique<"by_salary", mic::key<&Employee::salary>>,
            mic::hashed_unique    <"by_email",  mic::key<&Employee::email>>
        >>;
    Table t;
    t.insert(Employee{1, "A", "a", 100});
    t.insert(Employee{2, "B", "b", 300});
    t.insert(Employee{3, "C", "c", 200});

    auto r = t.try_insert(Employee{4, "D", "a", 50});  // email collides
    CHECK(!r.has_value());
    CHECK(r.error().index_tag == "by_email");

    // ranges: top-2 salaries (reverse over ranked index)
    std::vector<int> top;
    for (const Employee& e : t.get<"by_salary">() | std::views::reverse | std::views::take(2))
        top.push_back(e.salary);
    CHECK((top == std::vector<int>{300, 200}));

    // ranked nth/rank
    CHECK(t.get<"by_salary">().nth(0)->salary == 100);
    CHECK(t.get<"by_salary">().rank(t.get<"by_salary">().find(300)) == 2);

    std::println("  formatted: {}", t);
}

} // namespace

int main() {
    test_basic_and_member_function_pointer();
    test_shared_ptr_elements();
    test_composite_key();
    test_modify_replace_rollback();
    test_projection_and_nodehandle();
    test_expected_and_ranges();

    std::println("\n{}/{} checks passed", g_checks - g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
