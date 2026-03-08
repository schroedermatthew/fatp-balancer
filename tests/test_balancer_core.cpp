/**
 * @file test_balancer_core.cpp
 * @brief End-to-end integration tests for Balancer.h with SimulatedCluster.
 */

/*
BALANCER_META:
  meta_version: 1
  component: Balancer
  file_role: test
  path: tests/test_balancer_core.cpp
  namespace: balancer::testing::balancercore
  layer: Testing
  summary: End-to-end tests — submit, route, complete, CostModel update, policy switch.
  api_stability: in_work
  related:
    headers:
      - include/balancer/Balancer.h
      - sim/SimulatedCluster.h
*/

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "balancer/Balancer.h"
#include "policies/LeastLoaded.h"
#include "policies/RoundRobin.h"
#include "sim/SimulatedCluster.h"
#include "FatPTest.h"

namespace balancer::testing::balancercore
{

using namespace std::chrono_literals;

// ============================================================================
// Helpers
// ============================================================================

inline sim::SimulatedClusterConfig clusterConfig(uint32_t nodeCount = 3)
{
    sim::SimulatedClusterConfig cfg;
    cfg.nodeCount         = nodeCount;
    cfg.workerCount       = 2;
    cfg.overloadThreshold = 20;
    cfg.recoverThreshold  = 10;
    cfg.minJobDurationUs  = 1000;
    cfg.maxJobDurationUs  = 5000;
    return cfg;
}

inline BalancerConfig balancerConfig()
{
    BalancerConfig cfg;
    cfg.nodeOverloadThreshold = 20;
    cfg.nodeRecoverThreshold  = 10;
    return cfg;
}

inline Job makeJob(Priority p = Priority::Normal, uint64_t costUs = 2000)
{
    Job j;
    j.priority      = p;
    j.estimatedCost = Cost{costUs};
    j.payload       = [] { std::this_thread::sleep_for(2ms); };
    j.submitted     = Clock::now();
    return j;
}

// ============================================================================
// Basic construction
// ============================================================================

FATP_TEST_CASE(balancer_constructs_with_cluster)
{
    sim::SimulatedCluster cluster(clusterConfig());
    cluster.start();

    Balancer balancer(cluster.nodes(),
                      std::make_unique<RoundRobin>(),
                      balancerConfig());

    FATP_ASSERT_EQ(balancer.totalSubmitted(), uint64_t(0),
                   "No jobs submitted at construction");
    FATP_ASSERT_EQ(balancer.inFlightCount(), uint32_t(0),
                   "No jobs in flight at construction");

    cluster.stop();
    return true;
}

// ============================================================================
// Submit and complete
// ============================================================================

FATP_TEST_CASE(submit_returns_handle_on_success)
{
    sim::SimulatedCluster cluster(clusterConfig());
    cluster.start();
    std::this_thread::sleep_for(10ms);

    Balancer balancer(cluster.nodes(),
                      std::make_unique<RoundRobin>(),
                      balancerConfig());

    auto result = balancer.submit(makeJob());
    FATP_ASSERT_TRUE(result.has_value(), "Submit to idle cluster must succeed");
    FATP_ASSERT_EQ(balancer.totalSubmitted(), uint64_t(1), "totalSubmitted must be 1");

    cluster.stop();
    return true;
}

FATP_TEST_CASE(multiple_submits_complete_and_decrement_in_flight)
{
    sim::SimulatedCluster cluster(clusterConfig());
    cluster.start();
    std::this_thread::sleep_for(10ms);

    Balancer balancer(cluster.nodes(),
                      std::make_unique<LeastLoaded>(),
                      balancerConfig());

    constexpr int kJobs = 6;
    for (int i = 0; i < kJobs; ++i)
    {
        auto r = balancer.submit(makeJob());
        FATP_ASSERT_TRUE(r.has_value(), "Each submit must succeed");
    }
    FATP_ASSERT_EQ(balancer.totalSubmitted(), uint64_t(kJobs), "totalSubmitted matches");

    // Wait for all jobs to complete.
    for (int i = 0; i < 200 && balancer.inFlightCount() > 0; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }
    FATP_ASSERT_EQ(balancer.inFlightCount(), uint32_t(0),
                   "All jobs must complete and in-flight count must reach zero");

    cluster.stop();
    return true;
}

// ============================================================================
// Cost model updates
// ============================================================================

FATP_TEST_CASE(cost_model_warms_after_enough_completions)
{
    sim::SimulatedCluster cluster(clusterConfig(/*nodeCount=*/1));
    cluster.start();
    std::this_thread::sleep_for(10ms);

    BalancerConfig bcfg = balancerConfig();
    bcfg.costModel.warmThreshold = 3; // warm after 3 observations
    Balancer balancer(cluster.nodes(), std::make_unique<RoundRobin>(), bcfg);

    NodeId firstNode = cluster.nodes()[0]->id();
    FATP_ASSERT_FALSE(balancer.costModel().isWarm(firstNode),
                      "Model must be cold before any completions");

    // Submit enough jobs to cross the warm threshold.
    for (int i = 0; i < 3; ++i)
    {
        (void)balancer.submit(makeJob());
    }

    // Wait for completions.
    for (int i = 0; i < 200 && balancer.inFlightCount() > 0; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }

    FATP_ASSERT_TRUE(balancer.costModel().isWarm(firstNode),
                     "Model must be warm after 3 completions");
    FATP_ASSERT_GE(balancer.costModel().observationCount(firstNode), uint32_t(3),
                   "Observation count must reach warmThreshold");

    cluster.stop();
    return true;
}

FATP_TEST_CASE(cost_model_multiplier_updates_from_actual_cost)
{
    sim::SimulatedCluster cluster(clusterConfig(1));
    cluster.start();
    std::this_thread::sleep_for(10ms);

    BalancerConfig bcfg = balancerConfig();
    bcfg.costModel.warmThreshold = 5;
    Balancer balancer(cluster.nodes(), std::make_unique<RoundRobin>(), bcfg);

    NodeId node = cluster.nodes()[0]->id();

    // Submit jobs with a very small estimate; actual will be larger.
    for (int i = 0; i < 5; ++i)
    {
        Job j = makeJob(Priority::Normal, /*costUs=*/1); // very low estimate
        j.payload = [] { std::this_thread::sleep_for(5ms); };
        (void)balancer.submit(std::move(j));
    }

    for (int i = 0; i < 200 && balancer.inFlightCount() > 0; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }

    // Multiplier must be > 1 because actual cost exceeded estimate.
    float mult = balancer.costModel().nodeMultiplier(node);
    FATP_ASSERT_GT(mult, 1.0f,
        "Multiplier must exceed 1.0 when node is consistently slower than estimate");

    cluster.stop();
    return true;
}

// ============================================================================
// Policy switching
// ============================================================================

FATP_TEST_CASE(switch_policy_changes_named_policy)
{
    sim::SimulatedCluster cluster(clusterConfig());
    cluster.start();
    std::this_thread::sleep_for(10ms);

    Balancer balancer(cluster.nodes(),
                      std::make_unique<RoundRobin>(),
                      balancerConfig());

    FATP_ASSERT_EQ(std::string(balancer.policyName()), std::string("RoundRobin"),
                   "Initial policy must be RoundRobin");

    balancer.switchPolicy(std::make_unique<LeastLoaded>());

    FATP_ASSERT_EQ(std::string(balancer.policyName()), std::string("LeastLoaded"),
                   "Policy name must update after switchPolicy");

    cluster.stop();
    return true;
}

FATP_TEST_CASE(jobs_still_route_after_policy_switch)
{
    sim::SimulatedCluster cluster(clusterConfig());
    cluster.start();
    std::this_thread::sleep_for(10ms);

    Balancer balancer(cluster.nodes(),
                      std::make_unique<RoundRobin>(),
                      balancerConfig());

    // Submit with RoundRobin.
    FATP_ASSERT_TRUE(balancer.submit(makeJob()).has_value(), "Pre-switch submit must work");

    balancer.switchPolicy(std::make_unique<LeastLoaded>());

    // Submit with LeastLoaded.
    FATP_ASSERT_TRUE(balancer.submit(makeJob()).has_value(), "Post-switch submit must work");

    FATP_ASSERT_EQ(balancer.totalSubmitted(), uint64_t(2), "Two jobs total");

    for (int i = 0; i < 200 && balancer.inFlightCount() > 0; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }
    FATP_ASSERT_EQ(balancer.inFlightCount(), uint32_t(0), "All jobs must complete");

    cluster.stop();
    return true;
}

// ============================================================================
// Deadline rejection
// ============================================================================

FATP_TEST_CASE(submit_with_already_expired_deadline_returns_DeadlineUnachievable)
{
    sim::SimulatedCluster cluster(clusterConfig());
    cluster.start();
    std::this_thread::sleep_for(10ms);

    Balancer balancer(cluster.nodes(),
                      std::make_unique<RoundRobin>(),
                      balancerConfig());

    Job j = makeJob();
    j.deadline = Clock::now() - 1s; // already in the past

    auto result = balancer.submit(std::move(j));
    FATP_ASSERT_FALSE(result.has_value(), "Expired deadline must be rejected");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(SubmitError::DeadlineUnachievable),
                   "Error must be DeadlineUnachievable");

    cluster.stop();
    return true;
}

// ============================================================================
// No-nodes edge case
// ============================================================================

FATP_TEST_CASE(submit_with_no_nodes_returns_error)
{
    Balancer balancer({}, std::make_unique<RoundRobin>(), balancerConfig());

    auto result = balancer.submit(makeJob());
    FATP_ASSERT_FALSE(result.has_value(), "Submit to empty cluster must fail");

    return true;
}

// ============================================================================
// Concurrent submissions
// ============================================================================

FATP_TEST_CASE(concurrent_submits_are_thread_safe)
{
    sim::SimulatedCluster cluster(clusterConfig(4));
    cluster.start();
    std::this_thread::sleep_for(10ms);

    Balancer balancer(cluster.nodes(),
                      std::make_unique<LeastLoaded>(),
                      balancerConfig());

    constexpr int kThreads = 4;
    constexpr int kPerThread = 10;
    std::atomic<int> successCount{0};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&]
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                auto r = balancer.submit(makeJob());
                if (r.has_value()) { ++successCount; }
            }
        });
    }
    for (auto& th : threads) { th.join(); }

    FATP_ASSERT_GT(successCount.load(), 0,
                   "At least some concurrent submits must succeed");
    FATP_ASSERT_EQ(balancer.totalSubmitted(), uint64_t(successCount.load()),
                   "totalSubmitted must match success count");

    // Wait for all to drain.
    for (int i = 0; i < 300 && balancer.inFlightCount() > 0; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }
    FATP_ASSERT_EQ(balancer.inFlightCount(), uint32_t(0),
                   "All concurrent jobs must complete");

    cluster.stop();
    return true;
}

} // namespace balancer::testing::balancercore

// ============================================================================
// Public interface
// ============================================================================

namespace balancer::testing
{

bool test_balancer_core()
{
    FATP_PRINT_HEADER(BALANCER CORE)

    fat_p::testing::TestRunner runner;

    FATP_RUN_TEST_NS(runner, balancercore, balancer_constructs_with_cluster);
    FATP_RUN_TEST_NS(runner, balancercore, submit_returns_handle_on_success);
    FATP_RUN_TEST_NS(runner, balancercore, multiple_submits_complete_and_decrement_in_flight);
    FATP_RUN_TEST_NS(runner, balancercore, cost_model_warms_after_enough_completions);
    FATP_RUN_TEST_NS(runner, balancercore, cost_model_multiplier_updates_from_actual_cost);
    FATP_RUN_TEST_NS(runner, balancercore, switch_policy_changes_named_policy);
    FATP_RUN_TEST_NS(runner, balancercore, jobs_still_route_after_policy_switch);
    FATP_RUN_TEST_NS(runner, balancercore, submit_with_already_expired_deadline_returns_DeadlineUnachievable);
    FATP_RUN_TEST_NS(runner, balancercore, submit_with_no_nodes_returns_error);
    FATP_RUN_TEST_NS(runner, balancercore, concurrent_submits_are_thread_safe);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_balancer_core() ? 0 : 1;
}
#endif
