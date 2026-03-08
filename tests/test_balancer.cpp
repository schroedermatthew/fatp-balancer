/**
 * @file test_balancer.cpp
 * @brief Test orchestrator — runs all completed fatp-balancer test suites.
 */

/*
BALANCER_META:
  meta_version: 1
  component: fatp-balancer
  file_role: test
  path: tests/test_balancer.cpp
  namespace: balancer::testing
  layer: Testing
  summary: Top-level test orchestrator — aggregates all fatp-balancer test suites.
  api_stability: in_work
*/

#include <iostream>
#include <string>
#include <vector>

// Forward declarations of test suite entry points.
namespace balancer::testing
{
bool test_admission();
bool test_affinity_matrix();
bool test_aging();
bool test_cost_model();
bool test_policies();
bool test_node_fsm();
bool test_balancer_core();
bool test_fault_scenarios();
} // namespace balancer::testing

#define RUN_AND_RECORD(test_func) results.push_back({#test_func, test_func()})

int main()
{
    std::vector<std::pair<std::string, bool>> results;

    // Phase 1 suites
    RUN_AND_RECORD(balancer::testing::test_admission);
    RUN_AND_RECORD(balancer::testing::test_cost_model);
    RUN_AND_RECORD(balancer::testing::test_node_fsm);
    RUN_AND_RECORD(balancer::testing::test_balancer_core);

    // Phase 2 suites
    RUN_AND_RECORD(balancer::testing::test_policies);

    // Phase 3 suites
    RUN_AND_RECORD(balancer::testing::test_aging);

    // Phase 4 suites
    RUN_AND_RECORD(balancer::testing::test_fault_scenarios);

    // Phase 5 suites
    RUN_AND_RECORD(balancer::testing::test_affinity_matrix);

    std::cout << "\n=== fatp-balancer Test Summary ===\n";
    int failed = 0;
    for (auto& [name, passed] : results)
    {
        std::cout << (passed ? "PASS" : "FAIL") << "  " << name << "\n";
        if (!passed)
        {
            ++failed;
        }
    }
    std::cout << "\n" << (results.size() - failed) << "/" << results.size()
              << " suites passed.\n";

    return failed;
}
