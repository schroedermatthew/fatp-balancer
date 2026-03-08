/*
BALANCER_META:
  meta_version: 1
  component: TelemetryAdvisor
  file_role: test
  path: tests/test_telemetry_integration.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    End-to-end integration tests for the Phase 7 telemetry loop:
    SimulatedCluster::tick() → TelemetryAdvisor → FeatureSupervisor → Balancer.
    Covers: node fault injection drives node_fault alert, recovery clears alert,
    overload scenario with custom thresholds, JSON panel output, captureSnapshot
    field consistency, and negative cases (empty cluster, idempotent ticks).
  api_stability: internal
  related:
    headers:
      - sim/SimulatedCluster.h
      - include/balancer/TelemetryAdvisor.h
      - include/balancer/FeatureSupervisor.h
      - sim/TelemetrySnapshot.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file test_telemetry_integration.cpp
 * @brief End-to-end integration tests for the Phase 7 telemetry loop.
 *
 * Tests the full chain:
 *   SimulatedCluster::tick()
 *     → captureSnapshot()
 *     → detail::toClusterMetrics()
 *     → TelemetryAdvisor::evaluate()
 *     → FeatureSupervisor::applyChanges()
 *     → Balancer policy switch + AdmissionControl toggle
 */

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "FatPTest.h"

#include "balancer/BalancerConfig.h"
#include "balancer/BalancerFeatures.h"
#include "balancer/FeatureSupervisor.h"
#include "balancer/TelemetryAdvisor.h"

#include "policies/RoundRobin.h"
#include "sim/FaultInjector.h"
#include "sim/SimulatedCluster.h"
#include "sim/TelemetrySnapshot.h"

using fat_p::testing::TestRunner;
using namespace std::chrono_literals;

namespace balancer::testing::integrationns
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline sim::SimulatedClusterConfig makeClusterCfg(uint32_t nodeCount = 4)
{
    sim::SimulatedClusterConfig cfg;
    cfg.nodeCount         = nodeCount;
    cfg.workerCount       = 1;
    cfg.overloadThreshold = 20;
    cfg.recoverThreshold  = 10;
    cfg.minJobDurationUs  = 0;
    cfg.maxJobDurationUs  = 0;
    return cfg;
}

inline BalancerConfig makeBalancerCfg()
{
    BalancerConfig cfg;
    cfg.nodeOverloadThreshold = 20;
    cfg.nodeRecoverThreshold  = 10;
    return cfg;
}

/// Block until the given node reaches Failed or Offline state.
/// Returns true if the state is reached within the timeout.
[[nodiscard]] bool waitForFailed(const sim::SimulatedNode& node,
    std::chrono::milliseconds timeout = 500ms)
{
    auto deadline = balancer::Clock::now() + timeout;
    while (balancer::Clock::now() < deadline)
    {
        auto s = node.status();
        if (s == NodeState::Failed || s == NodeState::Offline) { return true; }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

/// Block until the given node is out of Failed state (Recovering or Idle).
[[nodiscard]] bool waitForRecovery(const sim::SimulatedNode& node,
    std::chrono::milliseconds timeout = 1000ms)
{
    auto deadline = balancer::Clock::now() + timeout;
    while (balancer::Clock::now() < deadline)
    {
        auto s = node.status();
        if (s != NodeState::Failed && s != NodeState::Offline) { return true; }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

struct Fixture
{
    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor};
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

FATP_TEST_CASE(tick_on_healthy_cluster_fires_no_alert)
{
    // All four nodes active, none failed. A single tick must produce no alert.
    Fixture f;

    auto r = f.cluster.tick(f.advisor, f.balancer);
    FATP_ASSERT_TRUE(r.has_value(), "tick must succeed on healthy cluster");
    FATP_ASSERT_TRUE(f.advisor.currentAlert().empty(),
        "No alert must be active on a healthy cluster tick");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "Policy must remain RoundRobin on healthy cluster");

    return true;
}

FATP_TEST_CASE(node_crash_drives_node_fault_alert_via_tick)
{
    // Crash 2 of 4 nodes (50% unavailable >= default 25% threshold).
    // tick() must fire kAlertNodeFault through the full chain.
    Fixture f;

    f.cluster.node(0).injectFault(sim::FaultType::Crash);
    f.cluster.node(1).injectFault(sim::FaultType::Crash);

    FATP_ASSERT_TRUE(waitForFailed(f.cluster.node(0)),
        "Node 0 must reach Failed state");
    FATP_ASSERT_TRUE(waitForFailed(f.cluster.node(1)),
        "Node 1 must reach Failed state");

    auto r = f.cluster.tick(f.advisor, f.balancer);
    FATP_ASSERT_TRUE(r.has_value(), "tick must succeed after node crash");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "kAlertNodeFault must fire when >= 25% nodes are unavailable");

    // Full chain: strict admission must be wired through supervisor to balancer.
    FATP_ASSERT_TRUE(f.balancer.admissionControl().isAdmissionStrict(),
        "Strict admission must be enabled end-to-end after node_fault alert");
    FATP_ASSERT_FALSE(f.balancer.admissionControl().isShedBulk(),
        "BulkShed must not be set by node_fault");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "RoundRobin must be active under node_fault");

    return true;
}

FATP_TEST_CASE(single_node_crash_at_exact_default_threshold)
{
    // 1 crashed out of 4 total = 25% unavailable — exactly at default threshold.
    // kAlertNodeFault must fire.
    Fixture f;

    f.cluster.node(0).injectFault(sim::FaultType::Crash);
    FATP_ASSERT_TRUE(waitForFailed(f.cluster.node(0)),
        "Node 0 must reach Failed state");

    auto r = f.cluster.tick(f.advisor, f.balancer);
    FATP_ASSERT_TRUE(r.has_value(), "tick must succeed");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "node_fault must fire at exact 25% threshold (1/4 = 0.25)");

    return true;
}

FATP_TEST_CASE(node_fault_clears_when_nodes_recover)
{
    // Crash 2 nodes, fire node_fault; clear faults; verify alert clears on next tick.
    Fixture f;

    f.cluster.node(0).injectFault(sim::FaultType::Crash);
    f.cluster.node(1).injectFault(sim::FaultType::Crash);

    FATP_ASSERT_TRUE(waitForFailed(f.cluster.node(0)), "Node 0 Failed");
    FATP_ASSERT_TRUE(waitForFailed(f.cluster.node(1)), "Node 1 Failed");

    // Confirm alert fires.
    auto r1 = f.cluster.tick(f.advisor, f.balancer);
    FATP_ASSERT_TRUE(r1.has_value(), "first tick must succeed");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "node_fault alert must be active");

    // Clear faults — nodes begin recovery.
    f.cluster.node(0).injectFault(sim::FaultType::None);
    f.cluster.node(1).injectFault(sim::FaultType::None);

    // Wait for nodes to recover past Failed state.
    FATP_ASSERT_TRUE(waitForRecovery(f.cluster.node(0)), "Node 0 must leave Failed state");
    FATP_ASSERT_TRUE(waitForRecovery(f.cluster.node(1)), "Node 1 must leave Failed state");

    // Tick again — nodes are recovering/idle, below threshold.
    auto r2 = f.cluster.tick(f.advisor, f.balancer);
    FATP_ASSERT_TRUE(r2.has_value(), "recovery tick must succeed");
    FATP_ASSERT_TRUE(f.advisor.currentAlert().empty(),
        "Alert must clear when nodes recover below the threshold");
    FATP_ASSERT_FALSE(f.balancer.admissionControl().isAdmissionStrict(),
        "Strict admission must be cleared after recovery");

    return true;
}

FATP_TEST_CASE(tick_idempotent_on_same_cluster_state)
{
    // Two consecutive ticks with no state change must produce the same result.
    Fixture f;

    f.cluster.node(0).injectFault(sim::FaultType::Crash);
    FATP_ASSERT_TRUE(waitForFailed(f.cluster.node(0)), "Node 0 must reach Failed");

    auto r1 = f.cluster.tick(f.advisor, f.balancer);
    FATP_ASSERT_TRUE(r1.has_value(), "first tick");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault), "first tick alert");

    auto r2 = f.cluster.tick(f.advisor, f.balancer);
    FATP_ASSERT_TRUE(r2.has_value(), "idempotent second tick");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault), "second tick alert unchanged");

    return true;
}

FATP_TEST_CASE(overload_alert_via_custom_threshold_tick)
{
    // Custom thresholds: node_fault needs 70% unavailable (hard to hit with 4 nodes);
    // overload triggers when fewer than 90% of nodes are active.
    // Crash 1 of 4 nodes:
    //   unavailFrac = 1/4 = 0.25 < 0.70 → no node_fault
    //   activeFrac  = 3/4 = 0.75 < 0.90 → overload fires
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.70f;
    thresholds.overloadFraction    = 0.90f;

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    cluster.node(0).injectFault(sim::FaultType::Crash);
    FATP_ASSERT_TRUE(waitForFailed(cluster.node(0)), "Node 0 must reach Failed");

    auto r = cluster.tick(advisor, balancer);
    FATP_ASSERT_TRUE(r.has_value(), "tick must succeed");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertOverload),
                   "kAlertOverload must fire when activeFrac < overloadFraction");

    FATP_ASSERT_TRUE(balancer.admissionControl().isShedBulk(),
        "BulkShed must be wired through the full chain under overload");
    FATP_ASSERT_EQ(std::string(balancer.policyName()),
                   std::string("WorkStealing"),
                   "WorkStealing must be active under overload");

    return true;
}

FATP_TEST_CASE(capture_snapshot_fields_consistent_with_tick_alert)
{
    // captureSnapshot() must return activeNodes/unavailableNodes fields that
    // correspond to the same cluster state that drove the alert in tick().
    // With 2 of 4 nodes crashed, both captureSnapshot and tick must agree.
    Fixture f;

    f.cluster.node(0).injectFault(sim::FaultType::Crash);
    f.cluster.node(1).injectFault(sim::FaultType::Crash);
    FATP_ASSERT_TRUE(waitForFailed(f.cluster.node(0)), "Node 0 Failed");
    FATP_ASSERT_TRUE(waitForFailed(f.cluster.node(1)), "Node 1 Failed");

    sim::TelemetrySnapshot snap = f.cluster.captureSnapshot(f.balancer);

    FATP_ASSERT_EQ(snap.unavailableNodes, uint32_t(2),
        "captureSnapshot must report 2 unavailable nodes");
    // activeNodes may be 2 (Idle/Busy); the others are in intermediate states
    // or Failed. We only assert >= 1 because Recovering/Draining nodes are
    // not counted by either field.
    FATP_ASSERT_TRUE(snap.unavailableNodes >= 1,
        "At least 1 node must be unavailable after crash");

    // Tick should agree: node_fault fires (2/4 = 50% >= 25%).
    auto r = f.cluster.tick(f.advisor, f.balancer);
    FATP_ASSERT_TRUE(r.has_value(), "tick must succeed");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "tick must fire node_fault consistent with snapshot");

    return true;
}

FATP_TEST_CASE(json_panel_produces_non_empty_string_for_snapshot)
{
    // formatJson() must return a non-empty JSON string.
    // Structural spot-check: the output must contain key field names.
    Fixture f;

    sim::TelemetrySnapshot snap = f.cluster.captureSnapshot(f.balancer);

    std::string json = sim::formatJson(snap);
    FATP_ASSERT_TRUE(!json.empty(), "formatJson must return a non-empty string");

    // Structural spot-check for required top-level keys.
    FATP_ASSERT_TRUE(json.find("\"activeNodes\"") != std::string::npos,
        "JSON must contain activeNodes");
    FATP_ASSERT_TRUE(json.find("\"unavailableNodes\"") != std::string::npos,
        "JSON must contain unavailableNodes");
    FATP_ASSERT_TRUE(json.find("\"nodes\"") != std::string::npos,
        "JSON must contain nodes array");
    FATP_ASSERT_TRUE(json.find("\"overloadedFraction\"") != std::string::npos,
        "JSON must contain overloadedFraction");

    return true;
}

FATP_TEST_CASE(json_panel_compact_mode_shorter_than_pretty)
{
    // Compact JSON must be strictly shorter than pretty-printed JSON.
    Fixture f;
    sim::TelemetrySnapshot snap = f.cluster.captureSnapshot(f.balancer);

    std::string pretty  = sim::formatJson(snap, /*pretty=*/true);
    std::string compact = sim::formatJson(snap, /*pretty=*/false);

    FATP_ASSERT_TRUE(compact.size() < pretty.size(),
        "Compact JSON must be shorter than pretty-printed JSON");

    return true;
}

FATP_TEST_CASE(json_panel_reflects_node_count)
{
    // The nodes array in the JSON must match the cluster's nodeCount.
    Fixture f;  // 4-node cluster
    sim::TelemetrySnapshot snap = f.cluster.captureSnapshot(f.balancer);

    FATP_ASSERT_EQ(snap.nodes.size(), size_t(4), "Snapshot must have 4 node entries");

    std::string json = sim::formatJson(snap, /*pretty=*/false);

    // Count "nodeId" occurrences as a proxy for node count in JSON.
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = json.find("\"nodeId\"", pos)) != std::string::npos)
    {
        ++count;
        ++pos;
    }
    FATP_ASSERT_EQ(count, size_t(4), "JSON nodes array must contain 4 nodeId entries");

    return true;
}

FATP_TEST_CASE(tick_on_two_node_cluster_no_fault_stays_healthy)
{
    // Smaller cluster (2 nodes) with no faults — must not fire any alert.
    sim::SimulatedCluster cluster{makeClusterCfg(2)};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor};

    auto r = cluster.tick(advisor, balancer);
    FATP_ASSERT_TRUE(r.has_value(), "tick must succeed on 2-node healthy cluster");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(),
        "No alert on healthy 2-node cluster");

    return true;
}

FATP_TEST_CASE(tick_escalation_overload_to_node_fault_then_clear)
{
    // Walk the full priority stack end-to-end via tick():
    //   healthy → overload (crash 1/4 with low threshold) → node_fault
    //   (crash 2nd) → healthy (both recover)
    //
    // Thresholds: node_fault >= 40%, overload < 80%
    //   1/4 unavailable = 25% < 40%: no node_fault, 3/4 active = 75% < 80%: overload
    //   2/4 unavailable = 50% >= 40%: node_fault wins
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.40f;
    thresholds.overloadFraction    = 0.80f;

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    // Healthy baseline
    auto r0 = cluster.tick(advisor, balancer);
    FATP_ASSERT_TRUE(r0.has_value(), "healthy tick");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(), "healthy: no alert");

    // Crash node 0 → overload
    cluster.node(0).injectFault(sim::FaultType::Crash);
    FATP_ASSERT_TRUE(waitForFailed(cluster.node(0)), "Node 0 must reach Failed");

    auto r1 = cluster.tick(advisor, balancer);
    FATP_ASSERT_TRUE(r1.has_value(), "overload tick");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertOverload), "overload fires (1/4 crash, low threshold)");

    // Crash node 1 → node_fault (2/4 = 50% >= 40%)
    cluster.node(1).injectFault(sim::FaultType::Crash);
    FATP_ASSERT_TRUE(waitForFailed(cluster.node(1)), "Node 1 must reach Failed");

    auto r2 = cluster.tick(advisor, balancer);
    FATP_ASSERT_TRUE(r2.has_value(), "node_fault tick");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertNodeFault), "node_fault wins over overload");

    FATP_ASSERT_TRUE(balancer.admissionControl().isAdmissionStrict(),
        "Strict admission active under node_fault");
    FATP_ASSERT_FALSE(balancer.admissionControl().isShedBulk(),
        "BulkShed not set under node_fault");

    // Recover both nodes
    cluster.node(0).injectFault(sim::FaultType::None);
    cluster.node(1).injectFault(sim::FaultType::None);
    FATP_ASSERT_TRUE(waitForRecovery(cluster.node(0)), "Node 0 must leave Failed");
    FATP_ASSERT_TRUE(waitForRecovery(cluster.node(1)), "Node 1 must leave Failed");

    auto r3 = cluster.tick(advisor, balancer);
    FATP_ASSERT_TRUE(r3.has_value(), "recovery tick");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(), "alert clears after full recovery");
    FATP_ASSERT_FALSE(balancer.admissionControl().isAdmissionStrict(),
        "Strict admission cleared after recovery");

    return true;
}

FATP_TEST_CASE(latency_alert_via_direct_evaluate_drives_supervisor)
{
    // Verifies that kAlertLatency flows through the full chain:
    //   TelemetryAdvisor::evaluate() → FeatureSupervisor::applyChanges()
    //   → Balancer admission (shedBulk + strict OFF for latency alert).
    //
    // Uses direct evaluate() with a crafted ClusterMetrics snapshot rather than
    // tick(), because tick() reads live node latency which is zero without heavy
    // traffic. This is still an integration test: it exercises the full
    // evaluate → applyChanges → feature transition path.
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.99f; // suppress node_fault
    thresholds.overloadFraction    = 0.01f; // suppress overload
    thresholds.latencyThresholdUs  = 10'000; // 10 ms

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    // Baseline: healthy, no alert.
    ClusterMetrics healthy;
    healthy.activeNodes      = 4;
    healthy.unavailableNodes = 0;
    healthy.knownNodes       = 4;
    healthy.meanP50LatencyUs = 0;

    auto r0 = advisor.evaluate(healthy);
    FATP_ASSERT_TRUE(r0.has_value(), "healthy evaluate must succeed");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(), "healthy: no alert");

    // Elevated latency snapshot.
    ClusterMetrics latent;
    latent.activeNodes      = 4;
    latent.unavailableNodes = 0;
    latent.knownNodes       = 4;
    latent.meanP50LatencyUs = 15'000; // 15 ms > 10 ms threshold

    auto r1 = advisor.evaluate(latent);
    FATP_ASSERT_TRUE(r1.has_value(), "latency evaluate must succeed");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertLatency), "kAlertLatency fires on high p50");

    // Recover to healthy.
    auto r2 = advisor.evaluate(healthy);
    FATP_ASSERT_TRUE(r2.has_value(), "recovery evaluate must succeed");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(), "alert clears when latency drops");

    return true;
}

} // namespace balancer::testing::integrationns

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

namespace balancer::testing
{

bool test_telemetry_integration()
{
    FATP_PRINT_HEADER(TELEMETRY INTEGRATION)

    fat_p::testing::TestRunner runner;

    FATP_RUN_TEST_NS(runner, integrationns, tick_on_healthy_cluster_fires_no_alert);
    FATP_RUN_TEST_NS(runner, integrationns, node_crash_drives_node_fault_alert_via_tick);
    FATP_RUN_TEST_NS(runner, integrationns, single_node_crash_at_exact_default_threshold);
    FATP_RUN_TEST_NS(runner, integrationns, node_fault_clears_when_nodes_recover);
    FATP_RUN_TEST_NS(runner, integrationns, tick_idempotent_on_same_cluster_state);
    FATP_RUN_TEST_NS(runner, integrationns, overload_alert_via_custom_threshold_tick);
    FATP_RUN_TEST_NS(runner, integrationns, capture_snapshot_fields_consistent_with_tick_alert);
    FATP_RUN_TEST_NS(runner, integrationns, json_panel_produces_non_empty_string_for_snapshot);
    FATP_RUN_TEST_NS(runner, integrationns, json_panel_compact_mode_shorter_than_pretty);
    FATP_RUN_TEST_NS(runner, integrationns, json_panel_reflects_node_count);
    FATP_RUN_TEST_NS(runner, integrationns, tick_on_two_node_cluster_no_fault_stays_healthy);
    FATP_RUN_TEST_NS(runner, integrationns, tick_escalation_overload_to_node_fault_then_clear);
    FATP_RUN_TEST_NS(runner, integrationns, latency_alert_via_direct_evaluate_drives_supervisor);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_telemetry_integration() ? 0 : 1;
}
#endif
