// =============================================================================
//  examples/relational_queries.cpp
//
//  SQL-ish queries over mic indices — joins, grouping and range scans that fall
//  out of the fact that an ordered index is already sorted on its key.
//
//    * mic::queries::equi_join(a, b)  -> lazy INNER JOIN of two ordered indices
//                                        on a shared key (sort-merge, O(n+m))
//    * mic::queries::group_by(a)      -> {key, subrange} per run of equal keys
//    * index.range(key_ge(..), key_lt(..)) / mic::prefix(..) -> half-open and
//                                        composite-prefix range scans
//
//  These return std::generator (C++23), so iteration is lazy and allocation-free.
//
//  Build (MSVC):  cl /std:c++latest /W4 /EHsc /I include examples\relational_queries.cpp
// =============================================================================

#include <mic/multi_index.hpp>

#include <print>
#include <string>
#include <tuple>
#include <ranges>

struct Employee { int id; std::string dept; std::string name; int salary; };
struct Manager  { std::string dept; std::string name; };

using Staff = mic::multi_index<Employee, mic::indexed_by<
    mic::ordered_unique     <"by_id",        mic::key<&Employee::id>>,
    mic::ordered_non_unique <"by_dept",      mic::key<&Employee::dept>>,
    mic::ordered_non_unique <"by_dept_sal",  mic::key<&Employee::dept, &Employee::salary>>
>>;
using Managers = mic::multi_index<Manager, mic::indexed_by<
    mic::ordered_non_unique <"by_dept", mic::key<&Manager::dept>>
>>;

int main() {
    Staff staff;
    staff.insert({1, "Eng",   "Ada",   180'000});
    staff.insert({2, "Eng",   "Bjarne",150'000});
    staff.insert({3, "Eng",   "Carol", 240'000});
    staff.insert({4, "Sales", "Dan",   120'000});
    staff.insert({5, "Sales", "Eve",   200'000});
    staff.insert({6, "Research","Finn", 130'000});      // dept with no manager

    Managers managers;
    managers.insert({"Eng",   "Grace"});
    managers.insert({"Eng",   "Hedy"});                 // two Eng managers
    managers.insert({"Sales", "Ivan"});
    managers.insert({"Legal", "Judy"});                 // dept with no employee

#if MIC_HAS_GENERATOR
    // ---- INNER JOIN employees x managers on dept --------------------------
    // Eng has 3 employees x 2 managers = 6 rows; Sales 2x1 = 2; Research/Legal
    // drop out (no match on the other side).
    std::println("Who reports to whom (employee x dept-manager):");
    for (auto&& [e, m] : mic::queries::equi_join(staff.get<"by_dept">(), managers.get<"by_dept">()))
        std::println("  {:<7} {:<7} -> {}", e.dept, e.name, m.name);

    // ---- GROUP BY dept: headcount and a peek at members -------------------
    std::println("\nHeadcount by department:");
    for (auto&& [dept, group] : mic::queries::group_by(staff.get<"by_dept">())) {
        std::println("  {:<9} {} people", dept, std::ranges::distance(group));
    }
#endif

    // ---- composite range scan: Eng salaries in [150k, 200k] ---------------
    // by_dept_sal is sorted by (dept, salary); key_ge/key_le bracket the band,
    // mic::prefix("Eng") caps it at the end of the Eng group.
    std::println("\nEng earning 150k..200k inclusive:");
    auto band = staff.get<"by_dept_sal">().range(
        mic::key_ge(std::tuple{std::string("Eng"), 150'000}),
        mic::key_le(std::tuple{std::string("Eng"), 200'000}));
    for (const Employee& e : band)
        std::println("  {:<7} ${}", e.name, e.salary);

    // ---- composite prefix: everyone in Eng, any salary --------------------
    auto [lo, hi] = staff.get<"by_dept_sal">().equal_range(mic::prefix(std::string("Eng")));
    std::println("\nEng headcount via prefix: {}", std::distance(lo, hi));

    std::println("\nrelational_queries done.");
    return 0;
}
