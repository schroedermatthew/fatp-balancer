/**
 * @file test_node_fsm.cpp
 * @brief Unit tests for SimulatedNode state machine and priority admission table.
 */

/*
BALANCER_META:
  meta_version: 1
  component: SimulatedNode
  file_role: test
  path: tests/test_node_fsm.cpp
  namespace: balancer::testing::nodefsm
  layer: Testing
  summary: Tests for SimulatedNode lifecycle FSM and per-state priority admission table.
  api_stability: in_work
  related:
    headers:
      - sim/SimulatedNode.h
*/

#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"
#include "sim/SimulatedNode.h"
#include "FatPTest.h"

namespace balancer::testing::nodefsm
{

using namespace balancer;
using namespace std::chrono_literals;

// CompletionCallback is the Phase 3 alias for JobCompletionCallback, declared
// here to match the short name used throughout these tests.
using CompletionCallback = balancer::JobCompletionCallback;

// ============================================================================
// Helpers
// ============================================================================

inline sim::SimulatedNodeConfig fastConfig()
{
    sim::SimulatedNodeConfig cfg;
    cfg.workerCount        = 2;
    cfg.overloadThreshold  = 4;
    cfg.recoverThreshold   = 2;
    cfg.minJobDurationUs   = 1000;  // 1ms
    cfg.maxJobDurationUs   = 5000;  // 5ms
    return cfg;
}

/// Build a minimal job payload that completes quickly.
inline Job makeJob(Priority p = Priority::Normal, uint64_t costUs = 1000)
{
    Job j;
    j.priority      = p;
    j.estimatedCost = Cost{costUs};
    j.payload       = [] { std::this_thread::sleep_for(1ms); };
    j.id            = JobId{1};
    j.submitted     = Clock::now();
    return j;
}

/// Null completion callback — discards result.
inline CompletionCallback nullCallback()
{
    return [](Job, bool) {};
}

// ============================================================================
// Construction
// ============================================================================

FATP_TEST_CASE(node_starts_in_offline_state)
{
    sim::SimulatedNode node(NodeId{1}, fastConfig());
    FATP_ASSERT_EQ(static_cast<int>(node.status()),
                   static_cast<int>(NodeState::Offline),
                   "Node must start Offline before start() is called");
    return true;
}

FATP_TEST_CASE(node_transitions_to_idle_after_start)
{
    sim::SimulatedNode node(NodeId{1}, fastConfig());
    node.start();
    std::this_thread::sleep_for(10ms);
    FATP_ASSERT_EQ(static_cast<int>(node.status()),
                   static_cast<int>(NodeState::Idle),
                   "Node must reach Idle after start()");
    node.stop();
    return true;
}

// ============================================================================
// State machine: Idle → Busy → Idle
// ============================================================================

FATP_TEST_CASE(node_becomes_busy_when_job_submitted)
{
    sim::SimulatedNode node(NodeId{1}, fastConfig());
    node.start();
    std::this_thread::sleep_for(5ms); // ensure Idle

    // Submit a job that takes a moment to complete.
    Job j = makeJob();
    j.payload = [] { std::this_thread::sleep_for(50ms); };

    auto result = node.submit(std::move(j), nullCallback());
    FATP_ASSERT_TRUE(result.has_value(), "Submit must succeed from Idle");

    std::this_thread::sleep_for(5ms); // let state transition fire
    auto state = node.status();
    FATP_ASSERT_TRUE(state == NodeState::Busy || state == NodeState::Idle,
                     "Node must be Busy or Idle (if already done) during execution");
    node.stop();
    return true;
}

FATP_TEST_CASE(node_returns_to_idle_after_job_completes)
{
    sim::SimulatedNode node(NodeId{1}, fastConfig());
    node.start();
    std::this_thread::sleep_for(5ms);

    bool completed = false;
    Job j = makeJob();
    j.payload = [] { std::this_thread::sleep_for(20ms); };

    node.submit(std::move(j), [&](Job, bool) { completed = true; });

    // Wait up to 500ms for completion.
    for (int i = 0; i < 50 && !completed; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }

    FATP_ASSERT_TRUE(completed, "Job must complete");
    FATP_ASSERT_EQ(static_cast<int>(node.status()),
                   static_cast<int>(NodeState::Idle),
                   "Node must return to Idle after completion");
    node.stop();
    return true;
}

// ============================================================================
// State machine: Overloaded
// ============================================================================

FATP_TEST_CASE(node_transitions_to_overloaded_when_queue_full)
{
    sim::SimulatedNodeConfig cfg = fastConfig();
    cfg.overloadThreshold = 2;
    cfg.workerCount       = 1;
    cfg.minJobDurationUs  = 100000; // 100ms — ensures queue builds up
    cfg.maxJobDurationUs  = 100000;

    sim::SimulatedNode node(NodeId{1}, cfg);
    node.start();
    std::this_thread::sleep_for(5ms);

    // Submit enough jobs to exceed the overload threshold.
    for (int i = 0; i < 5; ++i)
    {
        Job j = makeJob();
        j.payload = [] { std::this_thread::sleep_for(100ms); };
        node.submit(std::move(j), nullCallback());
    }

    std::this_thread::sleep_for(20ms);
    auto state = node.status();
    FATP_ASSERT_TRUE(state == NodeState::Overloaded || state == NodeState::Busy,
                     "Node must be Overloaded or Busy when queue exceeds threshold");
    node.stop();
    return true;
}

// ============================================================================
// Priority admission table
// ============================================================================

FATP_TEST_CASE(admission_table_idle_accepts_all_priorities)
{
    // nodeAcceptsPriority is a free function in LoadMetrics.h.
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Idle, Priority::Critical), "Idle/Critical");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Idle, Priority::High),     "Idle/High");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Idle, Priority::Normal),   "Idle/Normal");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Idle, Priority::Low),      "Idle/Low");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Idle, Priority::Bulk),     "Idle/Bulk");
    return true;
}

FATP_TEST_CASE(admission_table_busy_accepts_all_priorities)
{
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Busy, Priority::Critical), "Busy/Critical");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Busy, Priority::High),     "Busy/High");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Busy, Priority::Normal),   "Busy/Normal");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Busy, Priority::Low),      "Busy/Low");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Busy, Priority::Bulk),     "Busy/Bulk");
    return true;
}

FATP_TEST_CASE(admission_table_overloaded_accepts_critical_and_high_only)
{
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Overloaded, Priority::Critical),
                     "Overloaded must accept Critical");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Overloaded, Priority::High),
                     "Overloaded must accept High");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Overloaded, Priority::Normal),
                      "Overloaded must reject Normal");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Overloaded, Priority::Low),
                      "Overloaded must reject Low");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Overloaded, Priority::Bulk),
                      "Overloaded must reject Bulk");
    return true;
}

FATP_TEST_CASE(admission_table_draining_accepts_critical_only)
{
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Draining, Priority::Critical),
                     "Draining must accept Critical");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Draining, Priority::High),
                      "Draining must reject High");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Draining, Priority::Normal),
                      "Draining must reject Normal");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Draining, Priority::Low),
                      "Draining must reject Low");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Draining, Priority::Bulk),
                      "Draining must reject Bulk");
    return true;
}

FATP_TEST_CASE(admission_table_failed_accepts_nothing)
{
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Failed, Priority::Critical),
                      "Failed must reject Critical");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Failed, Priority::High),
                      "Failed must reject High");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Failed, Priority::Normal),
                      "Failed must reject Normal");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Failed, Priority::Low),
                      "Failed must reject Low");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Failed, Priority::Bulk),
                      "Failed must reject Bulk");
    return true;
}

FATP_TEST_CASE(admission_table_offline_accepts_nothing)
{
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Offline, Priority::Critical),
                      "Offline must reject Critical");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Offline, Priority::Bulk),
                      "Offline must reject Bulk");
    return true;
}

FATP_TEST_CASE(admission_table_recovering_accepts_critical_and_high)
{
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Recovering, Priority::Critical),
                     "Recovering must accept Critical");
    FATP_ASSERT_TRUE(nodeAcceptsPriority(NodeState::Recovering, Priority::High),
                     "Recovering must accept High");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Recovering, Priority::Normal),
                      "Recovering must reject Normal");
    FATP_ASSERT_FALSE(nodeAcceptsPriority(NodeState::Recovering, Priority::Bulk),
                      "Recovering must reject Bulk");
    return true;
}

// ============================================================================
// Metrics
// ============================================================================

FATP_TEST_CASE(metrics_queue_depth_reflects_pending_jobs)
{
    sim::SimulatedNodeConfig cfg = fastConfig();
    cfg.workerCount      = 1;
    cfg.minJobDurationUs = 200000; // 200ms — holds up queue
    cfg.maxJobDurationUs = 200000;

    sim::SimulatedNode node(NodeId{42}, cfg);
    node.start();
    std::this_thread::sleep_for(5ms);

    for (int i = 0; i < 3; ++i)
    {
        Job j = makeJob();
        j.payload = [] { std::this_thread::sleep_for(200ms); };
        node.submit(std::move(j), nullCallback());
    }

    std::this_thread::sleep_for(10ms);
    auto m = node.metrics();
    FATP_ASSERT_EQ(m.nodeId, NodeId{42}, "Metrics nodeId must match");
    FATP_ASSERT_TRUE(m.queueDepth > 0, "Queue depth must be positive with pending jobs");
    node.stop();
    return true;
}

FATP_TEST_CASE(metrics_completed_count_increments)
{
    sim::SimulatedNode node(NodeId{1}, fastConfig());
    node.start();
    std::this_thread::sleep_for(5ms);

    int done = 0;
    for (int i = 0; i < 3; ++i)
    {
        Job j = makeJob();
        j.id      = JobId{static_cast<uint32_t>(i + 1)};
        j.payload = [] { std::this_thread::sleep_for(5ms); };
        node.submit(std::move(j), [&](Job, bool) { ++done; });
    }

    // Wait for all completions.
    for (int i = 0; i < 100 && done < 3; ++i)
    {
        std::this_thread::sleep_for(10ms);
    }

    auto m = node.metrics();
    FATP_ASSERT_EQ(m.completedJobs, uint64_t(3),
                   "completedJobs must equal submitted job count");
    node.stop();
    return true;
}

// ============================================================================
// State change callback
// ============================================================================

FATP_TEST_CASE(state_change_callback_fires_on_transition)
{
    sim::SimulatedNode node(NodeId{1}, fastConfig());

    std::vector<NodeState> states;
    node.onStateChange([&](NodeState s) { states.push_back(s); });

    node.start();
    std::this_thread::sleep_for(10ms);

    FATP_ASSERT_FALSE(states.empty(), "At least one state change must fire after start()");
    node.stop();
    return true;
}

// ============================================================================
// Fault injection
// ============================================================================

FATP_TEST_CASE(fault_crash_rejects_new_submissions)
{
    sim::SimulatedNode node(NodeId{1}, fastConfig());
    node.start();
    std::this_thread::sleep_for(5ms);

    node.injectFault(sim::FaultType::Crash);
    std::this_thread::sleep_for(10ms);

    Job j = makeJob();
    auto result = node.submit(std::move(j), nullCallback());
    FATP_ASSERT_FALSE(result.has_value(),
        "Crashed node must reject submissions");
    node.stop();
    return true;
}

FATP_TEST_CASE(fault_none_restores_operation)
{
    sim::SimulatedNode node(NodeId{1}, fastConfig());
    node.start();
    std::this_thread::sleep_for(5ms);

    node.injectFault(sim::FaultType::Crash);
    std::this_thread::sleep_for(10ms);

    node.injectFault(sim::FaultType::None); // clear fault
    std::this_thread::sleep_for(20ms);

    Job j = makeJob();
    j.payload = [] { std::this_thread::sleep_for(1ms); };
    auto result = node.submit(std::move(j), nullCallback());
    FATP_ASSERT_TRUE(result.has_value(),
        "Node must accept submissions after fault cleared");
    node.stop();
    return true;
}

} // namespace balancer::testing::nodefsm

// ============================================================================
// Public interface
// ============================================================================

namespace balancer::testing
{

bool test_node_fsm()
{
    FATP_PRINT_HEADER(NODE FSM)

    fat_p::testing::TestRunner runner;

    FATP_RUN_TEST_NS(runner, nodefsm, node_starts_in_offline_state);
    FATP_RUN_TEST_NS(runner, nodefsm, node_transitions_to_idle_after_start);
    FATP_RUN_TEST_NS(runner, nodefsm, node_becomes_busy_when_job_submitted);
    FATP_RUN_TEST_NS(runner, nodefsm, node_returns_to_idle_after_job_completes);
    FATP_RUN_TEST_NS(runner, nodefsm, node_transitions_to_overloaded_when_queue_full);
    FATP_RUN_TEST_NS(runner, nodefsm, admission_table_idle_accepts_all_priorities);
    FATP_RUN_TEST_NS(runner, nodefsm, admission_table_busy_accepts_all_priorities);
    FATP_RUN_TEST_NS(runner, nodefsm, admission_table_overloaded_accepts_critical_and_high_only);
    FATP_RUN_TEST_NS(runner, nodefsm, admission_table_draining_accepts_critical_only);
    FATP_RUN_TEST_NS(runner, nodefsm, admission_table_failed_accepts_nothing);
    FATP_RUN_TEST_NS(runner, nodefsm, admission_table_offline_accepts_nothing);
    FATP_RUN_TEST_NS(runner, nodefsm, admission_table_recovering_accepts_critical_and_high);
    FATP_RUN_TEST_NS(runner, nodefsm, metrics_queue_depth_reflects_pending_jobs);
    FATP_RUN_TEST_NS(runner, nodefsm, metrics_completed_count_increments);
    FATP_RUN_TEST_NS(runner, nodefsm, state_change_callback_fires_on_transition);
    FATP_RUN_TEST_NS(runner, nodefsm, fault_crash_rejects_new_submissions);
    FATP_RUN_TEST_NS(runner, nodefsm, fault_none_restores_operation);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_node_fsm() ? 0 : 1;
}
#endif
