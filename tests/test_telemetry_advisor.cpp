/*
BALANCER_META:
  meta_version: 1
  component: TelemetryAdvisor
  file_role: test
  path: tests/test_telemetry_advisor.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    Functional tests for TelemetryAdvisor. Covers healthy-cluster no-alert,
    node_fault threshold, overload threshold with custom thresholds, alert
    priority ordering, zero-node edge case, revert to healthy, idempotent
    evaluation, currentAlert accessor, and threshold accessors.
  api_stability: internal
  related:
    headers:
      - include/balancer/TelemetryAdvisor.h
      - include/balancer/FeatureSupervisor.h
      - include/balancer/BalancerFeatures.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file test_telemetry_advisor.cpp
 * @brief Functional tests for TelemetryAdvisor.
 */

#include <memory>
#include <string>

#include "FatPTest.h"

#include "balancer/BalancerConfig.h"
#include "balancer/BalancerFeatures.h"
#include "balancer/FeatureSupervisor.h"
#include "balancer/TelemetryAdvisor.h"

#include "policies/RoundRobin.h"
#include "sim/SimulatedCluster.h"

using fat_p::testing::TestRunner;

namespace balancer::testing::advisorns
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline sim::SimulatedClusterConfig makeClusterCfg()
{
    sim::SimulatedClusterConfig cfg;
    cfg.nodeCount         = 4;
    cfg.workerCount       = 1;
    cfg.overloadThreshold = 20;
    cfg.recoverThreshold  = 10;
    cfg.minJobDurationUs  = 1000;
    cfg.maxJobDurationUs  = 5000;
    return cfg;
}

inline BalancerConfig makeBalancerCfg()
{
    BalancerConfig cfg;
    cfg.nodeOverloadThreshold = 20;
    cfg.nodeRecoverThreshold  = 10;
    return cfg;
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

/// Returns a ClusterMetrics with only the DEBT-002-reliable fields populated.
inline ClusterMetrics makeMetrics(uint32_t activeNodes,
                                  uint32_t unavailableNodes,
                                  uint64_t totalSubmitted = 0)
{
    ClusterMetrics m;
    m.activeNodes      = activeNodes;
    m.unavailableNodes = unavailableNodes;
    m.totalSubmitted   = totalSubmitted;
    // throughputPerSecond, meanP50LatencyUs, maxP99LatencyUs, totalRejected
    // are intentionally left zero (DEBT-002).
    return m;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

FATP_TEST_CASE(construction_exposes_thresholds_and_no_active_alert)
{
    // After construction the supervisor has no active alert.
    // Thresholds and supervisor accessors must be reachable.
    Fixture f;

    FATP_ASSERT_TRUE(f.advisor.currentAlert().empty(),
        "No alert must be active on construction");
    FATP_ASSERT_EQ(f.advisor.thresholds().unavailableFraction, 0.25f,
        "Default unavailableFraction must be 0.25");
    FATP_ASSERT_EQ(f.advisor.thresholds().overloadFraction, 0.75f,
        "Default overloadFraction must be 0.75");

    return true;
}

FATP_TEST_CASE(healthy_cluster_fires_no_alert)
{
    // All nodes active, none unavailable — no threshold is breached.
    Fixture f;

    auto r = f.advisor.evaluate(makeMetrics(4, 0));
    FATP_ASSERT_TRUE(r.has_value(), "evaluate must succeed on healthy cluster");
    FATP_ASSERT_TRUE(f.advisor.currentAlert().empty(),
        "No alert must be active for a healthy cluster");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "Policy must remain RoundRobin when cluster is healthy");

    return true;
}

FATP_TEST_CASE(node_fault_fires_when_unavailable_fraction_reached)
{
    // 3 unavailable out of 4 total (active=1, unavailable=3):
    // unavailFrac = 3/4 = 0.75 >= 0.25 → kAlertNodeFault.
    Fixture f;

    auto r = f.advisor.evaluate(makeMetrics(1, 3));
    FATP_ASSERT_TRUE(r.has_value(), "evaluate must succeed");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "kAlertNodeFault must fire when unavailableFraction is reached");

    // Strict admission must be wired through supervisor → balancer.
    FATP_ASSERT_TRUE(f.balancer.admissionControl().isAdmissionStrict(),
        "Strict admission must be enabled under node_fault");
    FATP_ASSERT_FALSE(f.balancer.admissionControl().isShedBulk(),
        "BulkShed must not be set by node_fault");

    return true;
}

FATP_TEST_CASE(node_fault_at_exact_threshold)
{
    // 1 unavailable out of 4 total (active=3, unavailable=1):
    // unavailFrac = 1/4 = 0.25 >= 0.25 → kAlertNodeFault fires exactly at threshold.
    Fixture f;

    auto r = f.advisor.evaluate(makeMetrics(3, 1));
    FATP_ASSERT_TRUE(r.has_value(), "evaluate at exact threshold must succeed");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "kAlertNodeFault must fire at exact unavailableFraction boundary");

    return true;
}

FATP_TEST_CASE(overload_fires_with_custom_thresholds)
{
    // Custom thresholds that permit overload to fire independently of node_fault:
    //   unavailableFraction = 0.5  (node_fault needs >= 50% unavailable)
    //   overloadFraction    = 0.9  (overload needs < 90% active)
    //
    // metrics: activeNodes=8, unavailableNodes=1
    //   total = 9; unavailFrac = 1/9 ≈ 0.111 < 0.5 → no node_fault
    //   activeFrac = 8/9 ≈ 0.889 < 0.9 → overload fires
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.50f;
    thresholds.overloadFraction    = 0.90f;

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    auto r = advisor.evaluate(makeMetrics(8, 1));
    FATP_ASSERT_TRUE(r.has_value(), "evaluate must succeed");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertOverload),
                   "kAlertOverload must fire when activeFrac < overloadFraction");

    FATP_ASSERT_TRUE(balancer.admissionControl().isShedBulk(),
        "BulkShed must be wired through supervisor under overload");
    FATP_ASSERT_EQ(std::string(supervisor.activePolicy()),
                   std::string(features::kPolicyWorkStealing),
                   "WorkStealing must be active under overload");

    return true;
}

FATP_TEST_CASE(node_fault_takes_priority_over_overload)
{
    // Both conditions breached simultaneously — node_fault must win.
    // Custom thresholds: unavailableFraction=0.1 (very sensitive), overloadFraction=0.9
    // metrics: activeNodes=5, unavailableNodes=2
    //   total = 7; unavailFrac = 2/7 ≈ 0.286 >= 0.1 → node_fault
    //   activeFrac = 5/7 ≈ 0.714 < 0.9 → overload also triggered
    //   node_fault wins by priority ordering.
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.10f;
    thresholds.overloadFraction    = 0.90f;

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    auto r = advisor.evaluate(makeMetrics(5, 2));
    FATP_ASSERT_TRUE(r.has_value(), "evaluate must succeed");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "kAlertNodeFault must take priority over kAlertOverload");

    return true;
}

FATP_TEST_CASE(zero_total_nodes_fires_no_alert)
{
    // activeNodes=0, unavailableNodes=0 — empty cluster, no alert.
    Fixture f;

    auto r = f.advisor.evaluate(makeMetrics(0, 0));
    FATP_ASSERT_TRUE(r.has_value(), "evaluate on empty cluster must succeed");
    FATP_ASSERT_TRUE(f.advisor.currentAlert().empty(),
        "No alert must fire for an empty cluster (totalNodes == 0)");

    return true;
}

FATP_TEST_CASE(revert_to_healthy_clears_alert)
{
    // Apply node_fault, then revert to healthy cluster — alert must clear.
    Fixture f;

    auto r1 = f.advisor.evaluate(makeMetrics(1, 3));
    FATP_ASSERT_TRUE(r1.has_value(), "node_fault evaluate must succeed");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "node_fault must be active after first evaluate");

    // Healthy snapshot: all nodes active.
    auto r2 = f.advisor.evaluate(makeMetrics(4, 0));
    FATP_ASSERT_TRUE(r2.has_value(), "healthy revert evaluate must succeed");
    FATP_ASSERT_TRUE(f.advisor.currentAlert().empty(),
        "Alert must clear when cluster returns to healthy");

    FATP_ASSERT_FALSE(f.balancer.admissionControl().isAdmissionStrict(),
        "Strict admission must be cleared after revert to healthy");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "Policy must revert to RoundRobin when cluster is healthy");

    return true;
}

FATP_TEST_CASE(idempotent_evaluate_same_snapshot)
{
    // Calling evaluate() twice with the same condition is idempotent.
    Fixture f;
    const auto metrics = makeMetrics(1, 3);

    auto r1 = f.advisor.evaluate(metrics);
    FATP_ASSERT_TRUE(r1.has_value(), "first evaluate must succeed");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "node_fault must be active after first evaluate");

    auto r2 = f.advisor.evaluate(metrics);
    FATP_ASSERT_TRUE(r2.has_value(), "idempotent second evaluate must succeed");
    FATP_ASSERT_EQ(std::string(f.advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "Alert must remain node_fault after idempotent evaluate");

    return true;
}

FATP_TEST_CASE(alert_escalation_and_de_escalation_sequence)
{
    // Walk through the full priority stack:
    //   healthy → node_fault → healthy → overload (custom) → healthy
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.50f;
    thresholds.overloadFraction    = 0.90f;

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    // Healthy
    FATP_ASSERT_TRUE(advisor.evaluate(makeMetrics(8, 0)).has_value(), "healthy");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(), "No alert when healthy");

    // node_fault: 5 unavailable out of 9 total (unavailFrac ≈ 0.556 >= 0.5)
    FATP_ASSERT_TRUE(advisor.evaluate(makeMetrics(4, 5)).has_value(), "node_fault");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertNodeFault),
                   "node_fault must fire");

    // Back to healthy
    FATP_ASSERT_TRUE(advisor.evaluate(makeMetrics(8, 0)).has_value(), "revert");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(), "Alert cleared");

    // overload: 8 active / 9 total (activeFrac ≈ 0.889 < 0.9), 1 unavailable (< 0.5)
    FATP_ASSERT_TRUE(advisor.evaluate(makeMetrics(8, 1)).has_value(), "overload");
    FATP_ASSERT_EQ(std::string(advisor.currentAlert()),
                   std::string(features::kAlertOverload),
                   "Overload must fire");

    // Back to healthy
    FATP_ASSERT_TRUE(advisor.evaluate(makeMetrics(8, 0)).has_value(), "final revert");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(), "Final revert clears alert");

    return true;
}

FATP_TEST_CASE(latency_alert_fires_when_p50_exceeds_threshold)
{
    // kAlertLatency fires when meanP50LatencyUs >= latencyThresholdUs and
    // neither node_fault nor overload conditions are met.
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.99f; // far above current unavail ratio
    thresholds.overloadFraction    = 0.01f; // far below current active ratio
    thresholds.latencyThresholdUs  = 5'000; // 5 ms

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    ClusterMetrics m = makeMetrics(4, 0);
    m.meanP50LatencyUs = 5'000; // exactly at threshold

    auto r = advisor.evaluate(m);
    FATP_ASSERT_TRUE(r.has_value(), "evaluate must succeed");
    FATP_ASSERT_TRUE(advisor.currentAlert() == features::kAlertLatency,
        "kAlertLatency must fire when p50 >= latencyThresholdUs");

    return true;
}

FATP_TEST_CASE(latency_alert_not_fired_below_threshold)
{
    // kAlertLatency does not fire when meanP50LatencyUs is strictly below threshold.
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.99f;
    thresholds.overloadFraction    = 0.01f;
    thresholds.latencyThresholdUs  = 5'000;

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    ClusterMetrics m = makeMetrics(4, 0);
    m.meanP50LatencyUs = 4'999; // one microsecond below threshold

    auto r = advisor.evaluate(m);
    FATP_ASSERT_TRUE(r.has_value(), "evaluate must succeed");
    FATP_ASSERT_TRUE(advisor.currentAlert().empty(),
        "kAlertLatency must not fire when p50 < latencyThresholdUs");

    return true;
}

FATP_TEST_CASE(latency_alert_lower_priority_than_node_fault)
{
    // When both node_fault and latency conditions are true, kAlertNodeFault wins.
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.25f; // default — fires at 1/4 unavailable
    thresholds.overloadFraction    = 0.01f; // suppress overload
    thresholds.latencyThresholdUs  = 5'000;

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    // 1 of 4 nodes unavailable = 25% → matches unavailableFraction exactly.
    ClusterMetrics m = makeMetrics(3, 1);
    m.meanP50LatencyUs = 100'000; // well above latency threshold

    auto r = advisor.evaluate(m);
    FATP_ASSERT_TRUE(r.has_value(), "evaluate must succeed");
    FATP_ASSERT_TRUE(advisor.currentAlert() == features::kAlertNodeFault,
        "kAlertNodeFault must outrank kAlertLatency");

    return true;
}

FATP_TEST_CASE(latency_alert_lower_priority_than_overload)
{
    // When both overload and latency conditions are true, kAlertOverload wins.
    TelemetryThresholds thresholds;
    thresholds.unavailableFraction = 0.99f; // suppress node_fault
    thresholds.overloadFraction    = 0.75f; // default — fires when <75% active
    thresholds.latencyThresholdUs  = 5'000;

    sim::SimulatedCluster cluster{makeClusterCfg()};
    Balancer              balancer{cluster.nodes(),
                                   std::make_unique<RoundRobin>(),
                                   makeBalancerCfg()};
    FeatureSupervisor     supervisor{balancer};
    TelemetryAdvisor      advisor{supervisor, thresholds};

    // 2 active out of 4 total = 50% active < 75% threshold.
    ClusterMetrics m = makeMetrics(2, 2);
    m.meanP50LatencyUs = 100'000; // well above latency threshold

    auto r = advisor.evaluate(m);
    FATP_ASSERT_TRUE(r.has_value(), "evaluate must succeed");
    FATP_ASSERT_TRUE(advisor.currentAlert() == features::kAlertOverload,
        "kAlertOverload must outrank kAlertLatency");

    return true;
}

} // namespace balancer::testing::advisorns

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

namespace balancer::testing
{

bool test_TelemetryAdvisor()
{
    FATP_PRINT_HEADER(TELEMETRY ADVISOR)

    fat_p::testing::TestRunner runner;

    FATP_RUN_TEST_NS(runner, advisorns, construction_exposes_thresholds_and_no_active_alert);
    FATP_RUN_TEST_NS(runner, advisorns, healthy_cluster_fires_no_alert);
    FATP_RUN_TEST_NS(runner, advisorns, node_fault_fires_when_unavailable_fraction_reached);
    FATP_RUN_TEST_NS(runner, advisorns, node_fault_at_exact_threshold);
    FATP_RUN_TEST_NS(runner, advisorns, overload_fires_with_custom_thresholds);
    FATP_RUN_TEST_NS(runner, advisorns, node_fault_takes_priority_over_overload);
    FATP_RUN_TEST_NS(runner, advisorns, zero_total_nodes_fires_no_alert);
    FATP_RUN_TEST_NS(runner, advisorns, revert_to_healthy_clears_alert);
    FATP_RUN_TEST_NS(runner, advisorns, idempotent_evaluate_same_snapshot);
    FATP_RUN_TEST_NS(runner, advisorns, alert_escalation_and_de_escalation_sequence);
    FATP_RUN_TEST_NS(runner, advisorns, latency_alert_fires_when_p50_exceeds_threshold);
    FATP_RUN_TEST_NS(runner, advisorns, latency_alert_not_fired_below_threshold);
    FATP_RUN_TEST_NS(runner, advisorns, latency_alert_lower_priority_than_node_fault);
    FATP_RUN_TEST_NS(runner, advisorns, latency_alert_lower_priority_than_overload);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_TelemetryAdvisor() ? 0 : 1;
}
#endif
