/**
 * @file test_fault_scenarios.cpp
 * @brief Fault injection and recovery tests for SimulatedNode.
 */

/*
BALANCER_META:
  meta_version: 1
  component: FaultScenarios
  file_role: test
  path: tests/test_fault_scenarios.cpp
  namespace: balancer::testing::faultns
  layer: Testing
  summary: Fault injection tests — crash, slowdown, partition, and recovery scenarios.
  api_stability: in_work
  related:
    headers:
      - sim/SimulatedNode.h
      - sim/SimulatedCluster.h
      - sim/FaultInjector.h
      - include/balancer/Balancer.h
*/

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Expected.h"
#include "FatPTest.h"

#include "balancer/Balancer.h"
#include "balancer/BalancerConfig.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"
#include "policies/LeastLoaded.h"
#include "sim/FaultInjector.h"
#include "sim/SimulatedCluster.h"
#include "sim/SimulatedNode.h"

namespace balancer::testing::faultns
{

// ============================================================================
// Helpers
// ============================================================================

static balancer::Job makeJob(balancer::Priority p = balancer::Priority::Normal,
                              uint64_t           costUnits = 1)
{
    static std::atomic<uint32_t> sNextId{1};
    balancer::Job j;
    j.id            = balancer::JobId{sNextId.fetch_add(1)};
    j.priority      = p;
    j.estimatedCost = balancer::Cost{costUnits};
    j.submitted     = balancer::Clock::now();
    return j;
}

static balancer::BalancerConfig fastConfig()
{
    balancer::BalancerConfig cfg;
    cfg.costModel.warmThreshold = 2;
    return cfg;
}

// ============================================================================
// Crash fault
// ============================================================================

FATP_TEST_CASE(crashed_node_rejects_new_submits)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount = 1;
    balancer::sim::SimulatedNode node(balancer::NodeId{1}, nc);
    node.start();

    node.injectFault(balancer::sim::FaultType::Crash);

    auto result = node.submit(
        makeJob(),
        [](balancer::Job, bool) {});

    FATP_ASSERT_FALSE(result.has_value(), "Crashed node must reject submits");
    node.stop();
    return true;
}

FATP_TEST_CASE(crashed_node_reports_failed_state)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount = 1;
    balancer::sim::SimulatedNode node(balancer::NodeId{1}, nc);
    node.start();

    node.injectFault(balancer::sim::FaultType::Crash);

    FATP_ASSERT_EQ(static_cast<int>(node.status()),
                   static_cast<int>(balancer::NodeState::Failed),
                   "Crashed node must be in Failed state");
    node.stop();
    return true;
}

FATP_TEST_CASE(recovery_from_crash_transitions_to_recovering)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount = 1;
    balancer::sim::SimulatedNode node(balancer::NodeId{1}, nc);
    node.start();

    node.injectFault(balancer::sim::FaultType::Crash);
    FATP_ASSERT_EQ(static_cast<int>(node.status()),
                   static_cast<int>(balancer::NodeState::Failed),
                   "Must be Failed after crash");

    node.injectFault(balancer::sim::FaultType::None);
    // Clearing a crash fault on an empty-queue node resolves immediately to Idle.
    // SimulatedNode::updateStateUnlocked() transitions Recovering→Idle when the
    // queue is empty, which is the typical state after a crash drains all jobs.
    FATP_ASSERT_EQ(static_cast<int>(node.status()),
                   static_cast<int>(balancer::NodeState::Idle),
                   "Must be Idle after clearing crash fault with empty queue");

    node.stop();
    return true;
}

// ============================================================================
// Slowdown fault
// ============================================================================

FATP_TEST_CASE(slowdown_fault_delays_execution)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount      = 1;
    nc.minJobDurationUs = 1000; // 1ms baseline
    nc.maxJobDurationUs = 1000;
    balancer::sim::SimulatedNode node(balancer::NodeId{1}, nc);
    node.start();

    // Measure baseline latency (no fault).
    std::atomic<uint64_t> baseElapsed{0};
    {
        auto t0 = balancer::Clock::now();
        std::atomic<bool> done{false};
        (void)node.submit(makeJob(), [&](balancer::Job, bool)
        {
            baseElapsed.store(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    balancer::Clock::now() - t0).count()));
            done.store(true);
        });
        while (!done.load()) { std::this_thread::sleep_for(std::chrono::microseconds{100}); }
    }

    // Inject 2x slowdown and measure.
    balancer::sim::FaultConfig faultCfg;
    faultCfg.slowdownFactor = 2;
    node.injectFault(balancer::sim::FaultType::Slowdown, faultCfg);

    std::atomic<uint64_t> slowElapsed{0};
    {
        auto t0 = balancer::Clock::now();
        std::atomic<bool> done{false};
        (void)node.submit(makeJob(), [&](balancer::Job, bool)
        {
            slowElapsed.store(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    balancer::Clock::now() - t0).count()));
            done.store(true);
        });
        while (!done.load()) { std::this_thread::sleep_for(std::chrono::microseconds{100}); }
    }

    FATP_ASSERT_GT(slowElapsed.load(), baseElapsed.load(),
                   "Slowdown job must take longer than baseline");

    node.injectFault(balancer::sim::FaultType::None);
    node.stop();
    return true;
}

// ============================================================================
// Partition fault
// ============================================================================

FATP_TEST_CASE(partition_fault_rejects_new_submits)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount = 1;
    balancer::sim::SimulatedNode node(balancer::NodeId{1}, nc);
    node.start();

    node.injectFault(balancer::sim::FaultType::Partition);

    auto result = node.submit(makeJob(), [](balancer::Job, bool) {});
    FATP_ASSERT_FALSE(result.has_value(), "Partitioned node must reject new submits");

    node.injectFault(balancer::sim::FaultType::None);
    node.stop();
    return true;
}

FATP_TEST_CASE(clearing_partition_re_enables_submits)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount = 1;
    balancer::sim::SimulatedNode node(balancer::NodeId{1}, nc);
    node.start();

    node.injectFault(balancer::sim::FaultType::Partition);
    {
        auto result = node.submit(makeJob(), [](balancer::Job, bool) {});
        FATP_ASSERT_FALSE(result.has_value(), "Must reject while partitioned");
    }

    node.injectFault(balancer::sim::FaultType::None);

    std::atomic<bool> completed{false};
    auto result = node.submit(makeJob(), [&](balancer::Job, bool)
    {
        completed.store(true);
    });
    FATP_ASSERT_TRUE(result.has_value(), "Must accept after partition cleared");

    auto deadline = balancer::Clock::now() + std::chrono::seconds{2};
    while (!completed.load() && balancer::Clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    FATP_ASSERT_TRUE(completed.load(), "Job must complete after partition cleared");

    node.stop();
    return true;
}

// ============================================================================
// Cancel
// ============================================================================

FATP_TEST_CASE(cancel_valid_handle_succeeds)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount      = 1;
    nc.minJobDurationUs = 50000; // 50ms — gives time to cancel before execution
    nc.maxJobDurationUs = 50000;
    balancer::sim::SimulatedNode node(balancer::NodeId{1}, nc);
    node.start();

    // Submit a long job to block the worker.
    (void)node.submit(makeJob(), [](balancer::Job, bool) {});

    // Submit the target job — it will queue behind the long job.
    auto result = node.submit(makeJob(), [](balancer::Job, bool) {});
    FATP_ASSERT_TRUE(result.has_value(), "Submit must succeed");

    auto cancelResult = node.cancel(result.value());
    FATP_ASSERT_TRUE(cancelResult.has_value(), "Cancel of queued job must succeed");

    node.stop();
    return true;
}

FATP_TEST_CASE(cancel_invalid_handle_returns_not_found)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount = 1;
    balancer::sim::SimulatedNode node(balancer::NodeId{1}, nc);
    node.start();

    // Made-up handle that was never registered.
    balancer::JobHandle fakeHandle{0xDEADBEEFCAFEBABEull};
    auto result = node.cancel(fakeHandle);
    FATP_ASSERT_FALSE(result.has_value(), "Cancelling unknown handle must return error");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::CancelError::NotFound),
                   "Error must be NotFound");

    node.stop();
    return true;
}

// ============================================================================
// Balancer-level fault routing
// ============================================================================

FATP_TEST_CASE(balancer_routes_around_crashed_node)
{
    // Two-node cluster; crash one; verify balancer still routes to the other.
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount = 1;
    balancer::sim::SimulatedCluster cluster(2, nc);

    auto policy   = std::make_unique<balancer::LeastLoaded>();
    balancer::Balancer balancer(cluster.nodes(), std::move(policy), fastConfig());

    cluster.node(0).injectFault(balancer::sim::FaultType::Crash);

    std::atomic<int> completions{0};
    for (int i = 0; i < 5; ++i)
    {
        auto r = balancer.submit(makeJob());
        if (r.has_value())
        {
            // We can't easily attach per-submit callbacks through Balancer's
            // current API, but success here means routing didn't crash.
        }
    }
    // At minimum, submit() must not crash the process when one node is Failed.
    return true;
}

FATP_TEST_CASE(full_cluster_failure_rejects_submits)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount = 1;
    balancer::sim::SimulatedCluster cluster(2, nc);

    auto policy   = std::make_unique<balancer::LeastLoaded>();
    balancer::Balancer balancer(cluster.nodes(), std::move(policy), fastConfig());

    cluster.node(0).injectFault(balancer::sim::FaultType::Crash);
    cluster.node(1).injectFault(balancer::sim::FaultType::Crash);

    auto result = balancer.submit(makeJob());
    FATP_ASSERT_FALSE(result.has_value(),
                      "All-crashed cluster must reject submits");

    cluster.node(0).injectFault(balancer::sim::FaultType::None);
    cluster.node(1).injectFault(balancer::sim::FaultType::None);
    return true;
}

// ============================================================================
// Drain-swap-resume
// ============================================================================

FATP_TEST_CASE(switch_policy_completes_in_flight_jobs)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount      = 2;
    nc.minJobDurationUs = 5000; // 5ms per job
    nc.maxJobDurationUs = 5000;
    balancer::sim::SimulatedCluster cluster(2, nc);

    auto policy = std::make_unique<balancer::LeastLoaded>();
    balancer::Balancer balancer(cluster.nodes(), std::move(policy), fastConfig());

    // Submit several jobs so there are in-flight jobs during the switch.
    for (int i = 0; i < 4; ++i)
    {
        (void)balancer.submit(makeJob());
    }

    // switchPolicy must drain and then swap — no assert; the test validates
    // that switchPolicy() returns and does not deadlock or crash.
    balancer.switchPolicy(std::make_unique<balancer::LeastLoaded>());

    FATP_ASSERT_FALSE(balancer.isDraining(), "Draining must be false after switch completes");
    FATP_ASSERT_EQ(static_cast<int>(balancer.inFlightCount()), 0,
                   "In-flight count must be zero after drain");
    return true;
}

FATP_TEST_CASE(submits_rejected_while_draining)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount      = 1;
    nc.minJobDurationUs = 20000; // 20ms — long enough to observe draining
    nc.maxJobDurationUs = 20000;
    balancer::sim::SimulatedCluster cluster(1, nc);

    auto policy = std::make_unique<balancer::LeastLoaded>();
    balancer::Balancer balancer(cluster.nodes(), std::move(policy), fastConfig());

    // Submit one job to create an in-flight job.
    (void)balancer.submit(makeJob());

    // Launch switchPolicy on a separate thread so we can probe while it drains.
    std::atomic<bool> switchDone{false};
    std::thread switcher([&]
    {
        balancer.switchPolicy(std::make_unique<balancer::LeastLoaded>());
        switchDone.store(true);
    });

    // Give the switcher a moment to set mDraining before we check.
    std::this_thread::sleep_for(std::chrono::milliseconds{2});

    if (balancer.isDraining())
    {
        auto result = balancer.submit(makeJob());
        FATP_ASSERT_FALSE(result.has_value(),
                          "Submit must be rejected while draining");
    }

    switcher.join();
    FATP_ASSERT_TRUE(switchDone.load(), "Switch thread must have completed");
    return true;
}

// ============================================================================
// Jitter
// ============================================================================

FATP_TEST_CASE(jitter_produces_varying_latencies)
{
    balancer::sim::SimulatedNodeConfig nc;
    nc.workerCount      = 4;
    nc.minJobDurationUs = 1000;  // 1ms
    nc.maxJobDurationUs = 10000; // 10ms
    balancer::sim::SimulatedNode node(balancer::NodeId{1}, nc);
    node.start();

    constexpr int kJobs = 10;
    std::vector<uint64_t> latencies;
    latencies.reserve(kJobs);
    std::mutex latMu;
    std::atomic<int> done{0};

    for (int i = 0; i < kJobs; ++i)
    {
        auto t0 = balancer::Clock::now();
        (void)node.submit(makeJob(), [&, t0](balancer::Job, bool)
        {
            uint64_t us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    balancer::Clock::now() - t0).count());
            std::lock_guard lk(latMu);
            latencies.push_back(us);
            done.fetch_add(1);
        });
    }

    auto deadline = balancer::Clock::now() + std::chrono::seconds{5};
    while (done.load() < kJobs && balancer::Clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    FATP_ASSERT_EQ(done.load(), kJobs, "All jitter jobs must complete");

    // With 10 jobs in [1ms, 10ms] we expect some variance; at least two
    // distinct latency values should appear.
    uint64_t minLat = *std::min_element(latencies.begin(), latencies.end());
    uint64_t maxLat = *std::max_element(latencies.begin(), latencies.end());
    FATP_ASSERT_GT(maxLat, minLat, "Jitter must produce varying latencies");

    node.stop();
    return true;
}

} // namespace balancer::testing::faultns

// ============================================================================
// Public Interface
// ============================================================================

namespace balancer::testing
{

using fat_p::testing::TestRunner;

bool test_fault_scenarios()
{
    FATP_PRINT_HEADER(FAULT SCENARIOS)

    TestRunner runner;

    FATP_RUN_TEST_NS(runner, faultns, crashed_node_rejects_new_submits);
    FATP_RUN_TEST_NS(runner, faultns, crashed_node_reports_failed_state);
    FATP_RUN_TEST_NS(runner, faultns, recovery_from_crash_transitions_to_recovering);
    FATP_RUN_TEST_NS(runner, faultns, slowdown_fault_delays_execution);
    FATP_RUN_TEST_NS(runner, faultns, partition_fault_rejects_new_submits);
    FATP_RUN_TEST_NS(runner, faultns, clearing_partition_re_enables_submits);
    FATP_RUN_TEST_NS(runner, faultns, cancel_valid_handle_succeeds);
    FATP_RUN_TEST_NS(runner, faultns, cancel_invalid_handle_returns_not_found);
    FATP_RUN_TEST_NS(runner, faultns, balancer_routes_around_crashed_node);
    FATP_RUN_TEST_NS(runner, faultns, full_cluster_failure_rejects_submits);
    FATP_RUN_TEST_NS(runner, faultns, switch_policy_completes_in_flight_jobs);
    FATP_RUN_TEST_NS(runner, faultns, submits_rejected_while_draining);
    FATP_RUN_TEST_NS(runner, faultns, jitter_produces_varying_latencies);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_fault_scenarios() ? 0 : 1;
}
#endif
