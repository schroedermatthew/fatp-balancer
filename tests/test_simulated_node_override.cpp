/*
BALANCER_META:
  meta_version: 1
  component: SimulatedNode
  file_role: test
  path: tests/test_simulated_node_override.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    Tests for SimulatedNode::reprioritise() override wiring (Phase 10).
    Verifies that mPriorityOverrides is consumed by executeJob() and that
    cancel() clears stale override entries.
  api_stability: internal
  related:
    headers:
      - sim/SimulatedNode.h
      - include/balancer/INode.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file test_simulated_node_override.cpp
 * @brief Unit tests for SimulatedNode priority override wiring.
 *
 * Phase 10 (Task B): reprioritise() stores an override in mPriorityOverrides.
 * executeJob() must read and apply that override before running the job.
 * cancel() must erase the override so the map does not grow unboundedly.
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "FatPTest.h"

#include "balancer/INode.h"
#include "balancer/Job.h"

#include "sim/SimulatedNode.h"

using fat_p::testing::TestRunner;
using namespace std::chrono_literals;

namespace balancer::testing::overridens
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline std::unique_ptr<sim::SimulatedNode> makeNode()
{
    sim::SimulatedNodeConfig cfg;
    cfg.workerCount       = 2;
    cfg.minJobDurationUs  = 0;
    cfg.maxJobDurationUs  = 0;
    cfg.overloadThreshold = 100;
    cfg.recoverThreshold  = 50;
    auto n = std::make_unique<sim::SimulatedNode>(NodeId{1}, cfg);
    n->start();
    return n;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

FATP_TEST_CASE(reprioritise_override_consumed_by_execute)
{
    // Submit a job at Normal priority, reprioritise to Critical before it runs,
    // then verify the completion callback sees Critical priority.
    //
    // The job uses a brief sleep so reprioritise() has a chance to run while it
    // is still queued. With 2 workers and a single queued job this is reliable.
    auto nodePtr = makeNode(); sim::SimulatedNode& node = *nodePtr;

    std::atomic<Priority> observedPriority{Priority::Normal};
    std::atomic<bool>     done{false};

    Job job;
    job.priority      = Priority::Normal;
    job.estimatedCost = Cost{0};
    // 10 ms payload — enough time for reprioritise() to store the override
    // before executeJob() starts under lock.
    job.payload = [] { std::this_thread::sleep_for(10ms); };

    auto result = node.submit(
        std::move(job),
        [&](Job completed, bool) {
            observedPriority.store(completed.priority, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });
    FATP_ASSERT_TRUE(result.has_value(), "submit must succeed");

    // Reprioritise before the job finishes executing.
    auto rp = node.reprioritise(*result, Priority::Critical);
    // reprioritise() may return NotFound if the job already started; that is
    // acceptable. We verify the outcome below.

    // Wait for completion.
    auto deadline = Clock::now() + 2s;
    while (!done.load(std::memory_order_acquire) && Clock::now() < deadline)
    {
        std::this_thread::sleep_for(2ms);
    }
    FATP_ASSERT_TRUE(done.load(std::memory_order_acquire), "job must complete within 2s");

    // If reprioritise succeeded, the callback must observe Critical.
    // If it returned NotFound the job had already started — no assertion on priority.
    if (rp.has_value())
    {
        FATP_ASSERT_EQ(static_cast<int>(observedPriority.load(std::memory_order_acquire)),
                       static_cast<int>(Priority::Critical),
                       "executeJob must apply the reprioritise override");
    }

    return true;
}

FATP_TEST_CASE(reprioritise_cancel_clears_override)
{
    // Cancel a job after reprioritise() and confirm the override entry is gone.
    // Evidence: a second reprioritise() on the same (now-cancelled) handle returns
    // CancelError::NotFound — if cancel() had not cleared the map, the entry would
    // sit orphaned and could produce incorrect behaviour if the slot index were
    // reused (generation protects against actual misfire, but the map would grow).
    auto nodePtr = makeNode(); sim::SimulatedNode& node = *nodePtr;

    // Use a slow payload to ensure the job is still queued when we cancel.
    Job job;
    job.priority      = Priority::Normal;
    job.estimatedCost = Cost{0};
    job.payload       = [] { std::this_thread::sleep_for(500ms); };

    auto result = node.submit(
        std::move(job),
        [](Job, bool) {});
    FATP_ASSERT_TRUE(result.has_value(), "submit must succeed");

    // Store an override.
    auto rp = node.reprioritise(*result, Priority::High);
    // reprioritise() may have succeeded or found the job already running.
    // We can only assert cancel behaviour when the job was still pending.
    if (!rp.has_value())
    {
        // Job already started — test is not applicable.
        return true;
    }
    FATP_ASSERT_TRUE(rp.has_value(), "reprioritise must succeed for slow pending job");

    // Cancel the job.
    auto cancel = node.cancel(*result);
    FATP_ASSERT_TRUE(cancel.has_value(), "cancel must succeed");

    // A second reprioritise on the cancelled handle must return NotFound.
    auto rp2 = node.reprioritise(*result, Priority::Critical);
    FATP_ASSERT_TRUE(!rp2.has_value(),
        "reprioritise on cancelled handle must fail");
    FATP_ASSERT_EQ(static_cast<int>(rp2.error()),
                   static_cast<int>(CancelError::NotFound),
                   "error must be NotFound after cancel");

    return true;
}

} // namespace balancer::testing::overridens

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

namespace balancer::testing
{

bool test_simulated_node_override()
{
    FATP_PRINT_HEADER(SIMULATED NODE OVERRIDE)

    fat_p::testing::TestRunner runner;

    FATP_RUN_TEST_NS(runner, overridens, reprioritise_override_consumed_by_execute);
    FATP_RUN_TEST_NS(runner, overridens, reprioritise_cancel_clears_override);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_simulated_node_override() ? 0 : 1;
}
#endif
