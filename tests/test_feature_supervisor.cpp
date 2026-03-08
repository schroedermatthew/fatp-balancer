/*
BALANCER_META:
  meta_version: 1
  component: FeatureSupervisor
  file_role: test
  path: tests/test_feature_supervisor.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    Functional tests for FeatureSupervisor. Verifies alert-driven policy
    transitions, BatchObserver wiring to Balancer/AdmissionControl, idempotent
    calls, revert-to-baseline behaviour, and Phase 11 Entails graph semantics
    including shared-Entails ref-counting, alert group state queries, and
    kAlertNone explicit transitions.
  api_stability: internal
  related:
    headers:
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
 * @file test_feature_supervisor.cpp
 * @brief Functional tests for FeatureSupervisor.
 */

#include <memory>
#include <string>

#include "FatPTest.h"

#include "balancer/BalancerConfig.h"
#include "balancer/BalancerFeatures.h"
#include "balancer/FeatureSupervisor.h"

#include "policies/RoundRobin.h"
#include "sim/SimulatedCluster.h"

using fat_p::testing::TestRunner;

namespace balancer::testing::supervisor
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline sim::SimulatedClusterConfig makeClusterCfg()
{
    sim::SimulatedClusterConfig cfg;
    cfg.nodeCount         = 2;
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
};

// ---------------------------------------------------------------------------
// Phase 10 tests (unchanged)
// ---------------------------------------------------------------------------

FATP_TEST_CASE(construction_default_state)
{
    // After construction: RoundRobin enabled, no alert, admission defaults.
    Fixture f;
    f.cluster.start();

    FATP_ASSERT_TRUE(f.supervisor.activeAlert().empty(),
        "No alert active on construction");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "Default policy is RoundRobin");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyRoundRobin),
        "kPolicyRoundRobin must be enabled in manager");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "BulkShed must not be enabled initially");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionStrict),
        "Strict must not be enabled initially");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kFeatureAging),
        "Aging must not be enabled initially");

    return true;
}

FATP_TEST_CASE(overload_alert_switches_to_work_stealing)
{
    Fixture f;
    f.cluster.start();

    auto r = f.supervisor.applyChanges(features::kAlertOverload);
    FATP_ASSERT_TRUE(r.has_value(), "applyChanges(kAlertOverload) must succeed");

    FATP_ASSERT_EQ(std::string(f.supervisor.activeAlert()),
                   std::string(features::kAlertOverload),
                   "Active alert must be kAlertOverload");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyWorkStealing),
                   "Active policy must be WorkStealing under overload");

    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyWorkStealing),
        "kPolicyWorkStealing must be enabled in manager");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kPolicyRoundRobin),
        "kPolicyRoundRobin must be disabled after overload");

    // Implied admission feature
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "kAdmissionBulkShed must be implied by kAlertOverload");

    // Observer must have wired BulkShed to AdmissionControl
    FATP_ASSERT_TRUE(f.balancer.admissionControl().isShedBulk(),
        "Balancer::admissionControl().isShedBulk() must be true after overload");

    // Balancer policy name reflects WorkStealing
    FATP_ASSERT_EQ(std::string(f.balancer.policyName()),
                   std::string("WorkStealing"),
                   "Balancer policy name must be WorkStealing");

    return true;
}

FATP_TEST_CASE(latency_alert_enables_aging_and_bulk_shed)
{
    Fixture f;
    f.cluster.start();

    auto r = f.supervisor.applyChanges(features::kAlertLatency);
    FATP_ASSERT_TRUE(r.has_value(), "applyChanges(kAlertLatency) must succeed");

    FATP_ASSERT_EQ(std::string(f.supervisor.activeAlert()),
                   std::string(features::kAlertLatency),
                   "Active alert must be kAlertLatency");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "Active policy stays RoundRobin under latency alert");

    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kFeatureAging),
        "kFeatureAging must be implied by kAlertLatency");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "kAdmissionBulkShed must be implied by kAlertLatency");

    // Observer wiring
    FATP_ASSERT_TRUE(f.balancer.isAgingEnabled(),
        "Balancer aging must be enabled via observer");
    FATP_ASSERT_TRUE(f.balancer.admissionControl().isShedBulk(),
        "BulkShed must be set via observer");

    return true;
}

FATP_TEST_CASE(node_fault_alert_enables_strict_admission)
{
    Fixture f;
    f.cluster.start();

    auto r = f.supervisor.applyChanges(features::kAlertNodeFault);
    FATP_ASSERT_TRUE(r.has_value(), "applyChanges(kAlertNodeFault) must succeed");

    FATP_ASSERT_EQ(std::string(f.supervisor.activeAlert()),
                   std::string(features::kAlertNodeFault),
                   "Active alert must be kAlertNodeFault");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "Active policy stays RoundRobin under node fault");

    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionStrict),
        "kAdmissionStrict must be implied by kAlertNodeFault");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "kAdmissionBulkShed must not be set by kAlertNodeFault");

    FATP_ASSERT_TRUE(f.balancer.admissionControl().isAdmissionStrict(),
        "Balancer admissionControl().isAdmissionStrict() must be true");

    return true;
}

FATP_TEST_CASE(revert_to_baseline_clears_alert_and_restores_round_robin)
{
    Fixture f;
    f.cluster.start();

    // Set overload alert first.
    auto r1 = f.supervisor.applyChanges(features::kAlertOverload);
    FATP_ASSERT_TRUE(r1.has_value(), "overload alert must apply");

    // Revert to no alert.
    auto r2 = f.supervisor.applyChanges("");
    FATP_ASSERT_TRUE(r2.has_value(), "revert to baseline must succeed");

    FATP_ASSERT_TRUE(f.supervisor.activeAlert().empty(),
        "Alert must be cleared after revert");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "Policy must revert to RoundRobin");

    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "BulkShed must be disabled after revert");
    FATP_ASSERT_FALSE(f.balancer.admissionControl().isShedBulk(),
        "Balancer BulkShed must be off after revert");

    FATP_ASSERT_EQ(std::string(f.balancer.policyName()),
                   std::string("RoundRobin"),
                   "Balancer policy must be RoundRobin after revert");

    return true;
}

FATP_TEST_CASE(idempotent_same_alert_no_op)
{
    // Calling applyChanges() twice with the same alert is a no-op.
    Fixture f;
    f.cluster.start();

    auto r1 = f.supervisor.applyChanges(features::kAlertOverload);
    FATP_ASSERT_TRUE(r1.has_value(), "first overload apply must succeed");

    auto r2 = f.supervisor.applyChanges(features::kAlertOverload);
    FATP_ASSERT_TRUE(r2.has_value(), "idempotent apply must succeed");

    FATP_ASSERT_EQ(std::string(f.supervisor.activeAlert()),
                   std::string(features::kAlertOverload),
                   "Alert must still be overload");

    return true;
}

FATP_TEST_CASE(alert_transition_overload_to_latency)
{
    // Transition from overload (WorkStealing) to latency (RoundRobin + aging).
    Fixture f;
    f.cluster.start();

    auto r1 = f.supervisor.applyChanges(features::kAlertOverload);
    FATP_ASSERT_TRUE(r1.has_value(), "overload apply must succeed");

    auto r2 = f.supervisor.applyChanges(features::kAlertLatency);
    FATP_ASSERT_TRUE(r2.has_value(), "transition to latency must succeed");

    FATP_ASSERT_EQ(std::string(f.supervisor.activeAlert()),
                   std::string(features::kAlertLatency),
                   "Alert must be latency");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "Policy must be RoundRobin after latency");

    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kPolicyWorkStealing),
        "WorkStealing must be disabled after latency transition");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kFeatureAging),
        "Aging must be implied by latency alert");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "BulkShed must be implied by latency alert");

    return true;
}

FATP_TEST_CASE(set_feature_enabled_degradation_curve)
{
    // setFeatureEnabled() can toggle kFeatureDegradationCurve directly.
    Fixture f;
    f.cluster.start();

    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kFeatureDegradationCurve),
        "Degradation curve must be off initially");

    auto r1 = f.supervisor.setFeatureEnabled(features::kFeatureDegradationCurve, true);
    FATP_ASSERT_TRUE(r1.has_value(), "enable degradation curve must succeed");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kFeatureDegradationCurve),
        "Degradation curve must be on after enable");
    FATP_ASSERT_TRUE(f.balancer.isDegradationEnabled(),
        "Balancer degradation must be enabled via observer");

    auto r2 = f.supervisor.setFeatureEnabled(features::kFeatureDegradationCurve, false);
    FATP_ASSERT_TRUE(r2.has_value(), "disable degradation curve must succeed");
    FATP_ASSERT_FALSE(f.balancer.isDegradationEnabled(),
        "Balancer degradation must be disabled after toggle");

    return true;
}

FATP_TEST_CASE(update_affinity_warm_null_matrix_is_noop)
{
    // When no AffinityMatrix is provided, updateAffinityWarm() must be a no-op.
    Fixture f;
    f.cluster.start();

    f.supervisor.updateAffinityWarm();   // must not crash
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kFeatureAffinityMatrixWarm),
        "Warm flag must remain off when AffinityMatrix is null");

    return true;
}

// ---------------------------------------------------------------------------
// Phase 11 Entails-graph integration tests
// ---------------------------------------------------------------------------

FATP_TEST_CASE(construction_alert_none_enabled_in_manager)
{
    // Phase 11: kAlertNone is enabled at construction. The other three alert
    // features must be disabled. kPolicyRoundRobin is enabled via the Entails
    // edge kAlertNone -> kPolicyRoundRobin.
    Fixture f;
    f.cluster.start();

    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAlertNone),
        "kAlertNone must be enabled in manager at construction");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAlertOverload),
        "kAlertOverload must be disabled at construction");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAlertLatency),
        "kAlertLatency must be disabled at construction");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAlertNodeFault),
        "kAlertNodeFault must be disabled at construction");

    // RoundRobin enabled transitively via Entails, not directly.
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyRoundRobin),
        "kPolicyRoundRobin must be enabled transitively via kAlertNone Entails");

    // Balancer must have had switchPolicy called with RoundRobin via the observer.
    FATP_ASSERT_EQ(std::string(f.balancer.policyName()),
                   std::string("RoundRobin"),
                   "Balancer policy must be RoundRobin at construction");

    return true;
}

FATP_TEST_CASE(apply_changes_to_alert_none_explicit)
{
    // applyChanges(kAlertNone) is equivalent to applyChanges("").
    // From overload, both forms must revert correctly.
    Fixture f;
    f.cluster.start();

    auto r1 = f.supervisor.applyChanges(features::kAlertOverload);
    FATP_ASSERT_TRUE(r1.has_value(), "overload apply must succeed");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "BulkShed must be on under overload");

    // Revert using explicit kAlertNone string instead of "".
    auto r2 = f.supervisor.applyChanges(features::kAlertNone);
    FATP_ASSERT_TRUE(r2.has_value(), "applyChanges(kAlertNone) must succeed");

    // activeAlert() returns "" for idle state.
    FATP_ASSERT_TRUE(f.supervisor.activeAlert().empty(),
        "activeAlert must be empty (idle) after reverting to kAlertNone");

    // kAlertNone must be enabled; kAlertOverload disabled.
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAlertNone),
        "kAlertNone must be enabled after explicit revert");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAlertOverload),
        "kAlertOverload must be disabled after revert");

    // BulkShed cleared; RoundRobin active.
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "BulkShed must be cleared after revert to kAlertNone");
    FATP_ASSERT_FALSE(f.balancer.admissionControl().isShedBulk(),
        "Balancer BulkShed must be off after revert");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyRoundRobin),
        "kPolicyRoundRobin must be active after revert to kAlertNone");

    return true;
}

FATP_TEST_CASE(overload_to_latency_bulk_shed_remains_active)
{
    // kAdmissionBulkShed is Entailed by both kAlertOverload and kAlertLatency.
    // When transitioning overload -> latency, the ref-counted Entails cascade
    // must keep BulkShed enabled throughout: it should never go false.
    // We verify that after the transition the Balancer still has BulkShed on.
    Fixture f;
    f.cluster.start();

    auto r1 = f.supervisor.applyChanges(features::kAlertOverload);
    FATP_ASSERT_TRUE(r1.has_value(), "overload apply must succeed");
    FATP_ASSERT_TRUE(f.balancer.admissionControl().isShedBulk(),
        "BulkShed must be on under overload");

    auto r2 = f.supervisor.applyChanges(features::kAlertLatency);
    FATP_ASSERT_TRUE(r2.has_value(), "transition overload -> latency must succeed");

    // BulkShed must remain on after the transition — it was never cleared
    // because kAlertLatency also Entails it.
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "kAdmissionBulkShed must remain enabled after overload->latency");
    FATP_ASSERT_TRUE(f.balancer.admissionControl().isShedBulk(),
        "Balancer isShedBulk must remain true after overload->latency");

    // Verify latency-specific features were also applied.
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kFeatureAging),
        "kFeatureAging must be active after latency");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kPolicyWorkStealing),
        "kPolicyWorkStealing must be off after latency");
    FATP_ASSERT_TRUE(f.balancer.isAgingEnabled(),
        "Balancer aging must be on after latency");

    return true;
}

FATP_TEST_CASE(overload_to_node_fault_transition)
{
    // overload -> node_fault:
    //   Off: kPolicyWorkStealing, kAdmissionBulkShed
    //   On:  kPolicyRoundRobin, kAdmissionStrict
    Fixture f;
    f.cluster.start();

    auto r1 = f.supervisor.applyChanges(features::kAlertOverload);
    FATP_ASSERT_TRUE(r1.has_value(), "overload apply must succeed");

    auto r2 = f.supervisor.applyChanges(features::kAlertNodeFault);
    FATP_ASSERT_TRUE(r2.has_value(), "overload -> node_fault must succeed");

    FATP_ASSERT_EQ(std::string(f.supervisor.activeAlert()),
                   std::string(features::kAlertNodeFault),
                   "Active alert must be kAlertNodeFault");
    FATP_ASSERT_EQ(std::string(f.supervisor.activePolicy()),
                   std::string(features::kPolicyRoundRobin),
                   "Policy must be RoundRobin under node_fault");

    // WorkStealing and BulkShed must be cleared.
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kPolicyWorkStealing),
        "kPolicyWorkStealing must be off after node_fault");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "kAdmissionBulkShed must be off after node_fault");
    FATP_ASSERT_FALSE(f.balancer.admissionControl().isShedBulk(),
        "Balancer BulkShed must be off after node_fault");

    // Strict must be on.
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionStrict),
        "kAdmissionStrict must be on after node_fault");
    FATP_ASSERT_TRUE(f.balancer.admissionControl().isAdmissionStrict(),
        "Balancer isAdmissionStrict must be true after node_fault");

    return true;
}

FATP_TEST_CASE(latency_to_node_fault_transition)
{
    // latency -> node_fault:
    //   Off: kAdmissionBulkShed, kFeatureAging
    //   On:  kAdmissionStrict (kPolicyRoundRobin already active, stays active)
    Fixture f;
    f.cluster.start();

    auto r1 = f.supervisor.applyChanges(features::kAlertLatency);
    FATP_ASSERT_TRUE(r1.has_value(), "latency apply must succeed");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kFeatureAging),
        "Aging must be on under latency");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "BulkShed must be on under latency");

    auto r2 = f.supervisor.applyChanges(features::kAlertNodeFault);
    FATP_ASSERT_TRUE(r2.has_value(), "latency -> node_fault must succeed");

    FATP_ASSERT_EQ(std::string(f.supervisor.activeAlert()),
                   std::string(features::kAlertNodeFault),
                   "Active alert must be kAlertNodeFault");

    // Aging and BulkShed must be cleared.
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kFeatureAging),
        "kFeatureAging must be off after node_fault");
    FATP_ASSERT_FALSE(f.balancer.isAgingEnabled(),
        "Balancer aging must be off after node_fault");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "kAdmissionBulkShed must be off after node_fault");
    FATP_ASSERT_FALSE(f.balancer.admissionControl().isShedBulk(),
        "Balancer BulkShed must be off after node_fault");

    // Strict must be on; RoundRobin still active.
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionStrict),
        "kAdmissionStrict must be on after node_fault");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyRoundRobin),
        "kPolicyRoundRobin must remain active");

    return true;
}

FATP_TEST_CASE(node_fault_to_latency_transition)
{
    // node_fault -> latency:
    //   Off: kAdmissionStrict
    //   On:  kAdmissionBulkShed, kFeatureAging (kPolicyRoundRobin already active)
    Fixture f;
    f.cluster.start();

    auto r1 = f.supervisor.applyChanges(features::kAlertNodeFault);
    FATP_ASSERT_TRUE(r1.has_value(), "node_fault apply must succeed");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionStrict),
        "Strict must be on under node_fault");

    auto r2 = f.supervisor.applyChanges(features::kAlertLatency);
    FATP_ASSERT_TRUE(r2.has_value(), "node_fault -> latency must succeed");

    FATP_ASSERT_EQ(std::string(f.supervisor.activeAlert()),
                   std::string(features::kAlertLatency),
                   "Active alert must be kAlertLatency");

    // Strict must be cleared.
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionStrict),
        "kAdmissionStrict must be off after latency");
    FATP_ASSERT_FALSE(f.balancer.admissionControl().isAdmissionStrict(),
        "Balancer isAdmissionStrict must be false after latency");

    // Latency features must be on.
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "kAdmissionBulkShed must be on after latency");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kFeatureAging),
        "kFeatureAging must be on after latency");
    FATP_ASSERT_TRUE(f.balancer.isAgingEnabled(),
        "Balancer aging must be on after latency");
    FATP_ASSERT_TRUE(f.balancer.admissionControl().isShedBulk(),
        "Balancer BulkShed must be on after latency");

    return true;
}

FATP_TEST_CASE(full_cycle_all_alerts)
{
    // none -> overload -> latency -> node_fault -> none
    // Verify manager and Balancer state at each step.
    Fixture f;
    f.cluster.start();

    // Step 1: none (construction) — already verified by construction tests.

    // Step 2: overload
    FATP_ASSERT_TRUE(f.supervisor.applyChanges(features::kAlertOverload).has_value(),
        "none -> overload must succeed");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyWorkStealing),
        "WorkStealing on under overload");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "BulkShed on under overload");

    // Step 3: latency
    FATP_ASSERT_TRUE(f.supervisor.applyChanges(features::kAlertLatency).has_value(),
        "overload -> latency must succeed");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kPolicyWorkStealing),
        "WorkStealing off under latency");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyRoundRobin),
        "RoundRobin on under latency");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kFeatureAging),
        "Aging on under latency");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "BulkShed on under latency");

    // Step 4: node_fault
    FATP_ASSERT_TRUE(f.supervisor.applyChanges(features::kAlertNodeFault).has_value(),
        "latency -> node_fault must succeed");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kFeatureAging),
        "Aging off under node_fault");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionBulkShed),
        "BulkShed off under node_fault");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAdmissionStrict),
        "Strict on under node_fault");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyRoundRobin),
        "RoundRobin on under node_fault");

    // Step 5: none (via empty string)
    FATP_ASSERT_TRUE(f.supervisor.applyChanges("").has_value(),
        "node_fault -> none must succeed");
    FATP_ASSERT_TRUE(f.supervisor.activeAlert().empty(),
        "Alert must be idle after full cycle");
    FATP_ASSERT_FALSE(f.supervisor.isEnabled(features::kAdmissionStrict),
        "Strict off after full cycle");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyRoundRobin),
        "RoundRobin on after full cycle");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAlertNone),
        "kAlertNone must be re-enabled after full cycle");
    FATP_ASSERT_EQ(std::string(f.balancer.policyName()),
                   std::string("RoundRobin"),
                   "Balancer policy must be RoundRobin after full cycle");

    return true;
}

FATP_TEST_CASE(alert_group_state_after_construction)
{
    // The alert_group should be Partial after construction (kAlertNone enabled,
    // 1 of 4 members active). Policy_group is also Partial (kPolicyRoundRobin
    // enabled, 1 of 5 members active).
    Fixture f;
    f.cluster.start();

    auto alertState = f.supervisor.manager().getGroupState("alert_group");
    FATP_ASSERT_TRUE(alertState.has_value(),
        "alert_group state query must succeed");
    FATP_ASSERT_EQ(static_cast<int>(*alertState),
                   static_cast<int>(fat_p::feature::FeatureGroupState::Partial),
                   "alert_group must be Partial at construction (1 of 4 members active)");

    auto policyState = f.supervisor.manager().getGroupState("policy_group");
    FATP_ASSERT_TRUE(policyState.has_value(),
        "policy_group state query must succeed");
    FATP_ASSERT_EQ(static_cast<int>(*policyState),
                   static_cast<int>(fat_p::feature::FeatureGroupState::Partial),
                   "policy_group must be Partial at construction (1 of 5 members active)");

    return true;
}

FATP_TEST_CASE(unknown_alert_returns_error)
{
    // Requesting an unregistered alert name must return an error from
    // FeatureManager::replace() (feature not found). State must be unchanged.
    Fixture f;
    f.cluster.start();

    auto r = f.supervisor.applyChanges("alert.does_not_exist");
    FATP_ASSERT_FALSE(r.has_value(),
        "applyChanges with unknown alert name must return error");

    // State must be unchanged: still idle.
    FATP_ASSERT_TRUE(f.supervisor.activeAlert().empty(),
        "activeAlert must remain idle after failed applyChanges");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kAlertNone),
        "kAlertNone must still be enabled after failed applyChanges");
    FATP_ASSERT_TRUE(f.supervisor.isEnabled(features::kPolicyRoundRobin),
        "kPolicyRoundRobin must still be enabled after failed applyChanges");

    return true;
}

} // namespace balancer::testing::supervisor

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    namespace supervisor = balancer::testing::supervisor;

    TestRunner runner;

    // Phase 10 tests
    FATP_RUN_TEST_NS(runner, supervisor, construction_default_state);
    FATP_RUN_TEST_NS(runner, supervisor, overload_alert_switches_to_work_stealing);
    FATP_RUN_TEST_NS(runner, supervisor, latency_alert_enables_aging_and_bulk_shed);
    FATP_RUN_TEST_NS(runner, supervisor, node_fault_alert_enables_strict_admission);
    FATP_RUN_TEST_NS(runner, supervisor, revert_to_baseline_clears_alert_and_restores_round_robin);
    FATP_RUN_TEST_NS(runner, supervisor, idempotent_same_alert_no_op);
    FATP_RUN_TEST_NS(runner, supervisor, alert_transition_overload_to_latency);
    FATP_RUN_TEST_NS(runner, supervisor, set_feature_enabled_degradation_curve);
    FATP_RUN_TEST_NS(runner, supervisor, update_affinity_warm_null_matrix_is_noop);

    // Phase 11 Entails-graph integration tests
    FATP_RUN_TEST_NS(runner, supervisor, construction_alert_none_enabled_in_manager);
    FATP_RUN_TEST_NS(runner, supervisor, apply_changes_to_alert_none_explicit);
    FATP_RUN_TEST_NS(runner, supervisor, overload_to_latency_bulk_shed_remains_active);
    FATP_RUN_TEST_NS(runner, supervisor, overload_to_node_fault_transition);
    FATP_RUN_TEST_NS(runner, supervisor, latency_to_node_fault_transition);
    FATP_RUN_TEST_NS(runner, supervisor, node_fault_to_latency_transition);
    FATP_RUN_TEST_NS(runner, supervisor, full_cycle_all_alerts);
    FATP_RUN_TEST_NS(runner, supervisor, alert_group_state_after_construction);
    FATP_RUN_TEST_NS(runner, supervisor, unknown_alert_returns_error);

    return runner.print_summary(); // 0 = all passed; >0 = failure count
}
