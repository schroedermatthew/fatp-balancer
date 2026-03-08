/*
BALANCER_META:
  meta_version: 1
  component: FeatureSupervisor
  file_role: test
  path: tests/test_feature_manager_cascade_probe.cpp
  namespace: balancer::testing
  layer: Testing
  summary: Q2 resolution probe — documents three FeatureManager behaviours that constrain FeatureSupervisor reload() design.
  api_stability: internal
  related:
    headers:
      - include/balancer/BalancerFeatures.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file test_feature_manager_cascade_probe.cpp
 * @brief Q2 resolution: FeatureManager cascade probe.
 *
 * Phase 7 Plan §10 Q2 requires a targeted probe before FeatureSupervisor.h is
 * written. Three questions are answered here:
 *
 * **Q2a — Simultaneous conflicting Implies chains in one batchEnable:**
 * batchEnable({"alert.x", "alert.y"}) where both Imply different
 * MutuallyExclusive members FAILS. FeatureManager's plan/commit model detects
 * the conflict and returns an error without touching live state.
 *
 * **Q2b — Disable of an implying feature does not cascade-disable implied targets:**
 * disable("alert.x") leaves "policy.a" (implied by alert.x) enabled. The Implies
 * relationship is one-directional: enabling A implies B, but disabling A does NOT
 * disable B. Consequently, enable("alert.y") fails because policy.a is still active
 * and policy.b is MutuallyExclusive with it.
 *
 * **Q2c — Preempts + Implies on MutuallyExclusive siblings fails when Preempts
 * target is currently active:**
 * enable("alert.overload") with Preempts(policy.a) + Implies(policy.b) fails when
 * policy.a is currently active. FeatureManager's conflict check runs against
 * pre-plan state — it sees policy.a enabled and rejects policy.b as conflicting,
 * without considering that Preempts is about to disable policy.a. This succeeds
 * only when policy.a is NOT active at time of enable.
 *
 * **FeatureSupervisor design consequence (load-bearing):**
 * FeatureSupervisor::applyChanges() must NOT rely on FeatureManager's Preempts
 * or Implies to manage policy transitions across MutuallyExclusive members.
 * Instead it must:
 *   1. Track the currently-active policy and the currently-active alert (if any).
 *   2. Before enabling a new alert: batchDisable({current_alert, current_policy})
 *      to clear the MutuallyExclusive slot.
 *   3. Then enable the new alert (which will Imply the new policy into the now-
 *      empty slot without conflict).
 * The FeatureManager graph is authoritative for non-policy features (aging,
 * degradation, admission modes). Policy switching is sequenced explicitly by
 * FeatureSupervisor to avoid plan/commit ordering issues.
 */

#include <iostream>
#include <string>
#include <vector>

#include "FeatureManager.h"
#include "FatPTest.h"

using fat_p::testing::TestRunner;
using FM = fat_p::feature::FeatureManager<fat_p::SingleThreadedPolicy>;
using FR = fat_p::feature::FeatureRelationship;

namespace balancer::testing::fmprobe
{

static FM buildProbeFm()
{
    FM mgr;
    (void)mgr.addFeature("policy.a");
    (void)mgr.addFeature("policy.b");
    (void)mgr.addMutuallyExclusiveGroup("policies", {"policy.a", "policy.b"});
    (void)mgr.addFeature("alert.x");
    (void)mgr.addFeature("alert.y");
    (void)mgr.addRelationship("alert.x", FR::Implies, "policy.a");
    (void)mgr.addRelationship("alert.y", FR::Implies, "policy.b");
    return mgr;
}

// ---------------------------------------------------------------------------

FATP_TEST_CASE(single_alert_enables_correct_policy)
{
    // Baseline: one alert fires, its Implies target activates.
    FM mgr = buildProbeFm();
    auto res = mgr.enable("alert.x");
    FATP_ASSERT_TRUE(res.has_value(),            "enable alert.x must succeed");
    FATP_ASSERT_TRUE(mgr.isEnabled("alert.x"),   "alert.x must be enabled");
    FATP_ASSERT_TRUE(mgr.isEnabled("policy.a"),  "policy.a implied by alert.x must be enabled");
    FATP_ASSERT_FALSE(mgr.isEnabled("policy.b"), "policy.b must not be enabled");
    return true;
}

FATP_TEST_CASE(simultaneous_alert_conflict_is_rejected)
{
    // Q2a: batchEnable with two conflicting Implies chains must fail atomically.
    // Live state is untouched after the failed transaction.
    FM mgr = buildProbeFm();

    auto res = mgr.batchEnable({"alert.x", "alert.y"});
    FATP_ASSERT_FALSE(res.has_value(),
        "batchEnable with conflicting Implies chains must fail");
    FATP_ASSERT_FALSE(mgr.isEnabled("alert.x"),  "alert.x must not be enabled after failed batch");
    FATP_ASSERT_FALSE(mgr.isEnabled("alert.y"),  "alert.y must not be enabled after failed batch");
    FATP_ASSERT_FALSE(mgr.isEnabled("policy.a"), "policy.a must not be enabled after failed batch");
    FATP_ASSERT_FALSE(mgr.isEnabled("policy.b"), "policy.b must not be enabled after failed batch");
    return true;
}

FATP_TEST_CASE(implies_does_not_cascade_disable_on_parent_disable)
{
    // Q2b: disable(implicant) does NOT cascade-disable implied targets.
    // This is the root cause of the sequential-transition failure — policy.a
    // remains active after alert.x is disabled, blocking the next enable.
    FM mgr = buildProbeFm();

    (void)mgr.enable("alert.x");
    FATP_ASSERT_TRUE(mgr.isEnabled("policy.a"), "policy.a must be active after enable(alert.x)");

    (void)mgr.disable("alert.x");
    FATP_ASSERT_FALSE(mgr.isEnabled("alert.x"), "alert.x must be disabled");

    // policy.a is NOT cascade-disabled — this is the documented behaviour.
    FATP_ASSERT_TRUE(mgr.isEnabled("policy.a"),
        "policy.a remains enabled after disable(alert.x) — Implies does not cascade-disable");

    // Attempt to enable alert.y fails because policy.a is still in the slot.
    auto r = mgr.enable("alert.y");
    FATP_ASSERT_FALSE(r.has_value(),
        "enable(alert.y) fails while policy.a (implied by alert.x) is still enabled");
    return true;
}

FATP_TEST_CASE(explicit_policy_clear_enables_transition)
{
    // The correct alert-to-alert transition pattern for FeatureSupervisor:
    // batchDisable({alert, policy}) clears the MutuallyExclusive slot, then
    // enable(new_alert) succeeds.
    FM mgr = buildProbeFm();

    (void)mgr.enable("alert.x");
    FATP_ASSERT_TRUE(mgr.isEnabled("policy.a"), "policy.a must be active");

    // FeatureSupervisor must disable both the alert and the implied policy.
    auto r1 = mgr.batchDisable({"alert.x", "policy.a"});
    FATP_ASSERT_TRUE(r1.has_value(), "batchDisable alert.x + policy.a must succeed");
    FATP_ASSERT_FALSE(mgr.isEnabled("policy.a"), "policy.a must be cleared");

    auto r2 = mgr.enable("alert.y");
    FATP_ASSERT_TRUE(r2.has_value(),            "enable(alert.y) must succeed after clearing slot");
    FATP_ASSERT_TRUE(mgr.isEnabled("policy.b"), "policy.b must be active");
    return true;
}

FATP_TEST_CASE(preempts_implies_fails_when_target_is_active)
{
    // Q2c: Preempts + Implies on MutuallyExclusive siblings fails when the
    // Preempts target is currently enabled. FeatureManager's conflict check
    // runs against pre-plan state; it sees policy.a enabled and rejects
    // policy.b as MutuallyExclusive, without accounting for the planned
    // Preempts disable of policy.a in the same transaction.
    FM mgr;
    (void)mgr.addFeature("policy.a");
    (void)mgr.addFeature("policy.b");
    (void)mgr.addMutuallyExclusiveGroup("policies", {"policy.a", "policy.b"});
    (void)mgr.addFeature("alert.overload");
    (void)mgr.addRelationship("alert.overload", FR::Preempts, "policy.a");
    (void)mgr.addRelationship("alert.overload", FR::Implies,  "policy.b");

    (void)mgr.enable("policy.a");
    FATP_ASSERT_TRUE(mgr.isEnabled("policy.a"), "policy.a must be active");

    auto r = mgr.enable("alert.overload");
    // FINDING: this fails — Preempts+Implies combination on MutuallyExclusive
    // siblings does not work when the Preempts target is currently active.
    FATP_ASSERT_FALSE(r.has_value(),
        "enable(alert.overload) fails when policy.a is active — "
        "plan/commit conflict check does not account for planned Preempts disable");
    return true;
}

FATP_TEST_CASE(preempts_implies_succeeds_when_target_is_inactive)
{
    // Q2c continued: the same Preempts+Implies graph succeeds when the Preempts
    // target is NOT currently enabled. The MutuallyExclusive slot is free.
    FM mgr;
    (void)mgr.addFeature("policy.a");
    (void)mgr.addFeature("policy.b");
    (void)mgr.addMutuallyExclusiveGroup("policies", {"policy.a", "policy.b"});
    (void)mgr.addFeature("alert.overload");
    (void)mgr.addRelationship("alert.overload", FR::Preempts, "policy.a");
    (void)mgr.addRelationship("alert.overload", FR::Implies,  "policy.b");

    // policy.a is NOT active.
    auto r = mgr.enable("alert.overload");
    FATP_ASSERT_TRUE(r.has_value(),  "enable(alert.overload) succeeds when policy.a is inactive");
    FATP_ASSERT_TRUE(mgr.isEnabled("policy.b"),  "policy.b must be implied");
    FATP_ASSERT_FALSE(mgr.isEnabled("policy.a"), "policy.a must remain disabled");
    return true;
}

FATP_TEST_CASE(fromJson_does_not_validate_enabled_mutually_exclusive)
{
    // Q2 fromJson finding: fromJson() validates the structural graph (cycle check,
    // relationship consistency) via validateUnlocked(), but does NOT re-run the
    // enable() conflict path for features marked enabled=true. MutuallyExclusive
    // features with enabled=true in JSON are rejected at validateUnlocked().
    //
    // Finding confirmed: fromJson() returns an error when two MutuallyExclusive
    // features are both enabled=true. FeatureSupervisor must NOT use fromJson()
    // to drive live state — it must use addFeature/addRelationship + enable() in
    // priority order.
    const std::string conflicting = R"({
        "features": {
            "policy.a": { "enabled": true,  "MutuallyExclusive": ["policy.b"] },
            "policy.b": { "enabled": true,  "MutuallyExclusive": ["policy.a"] }
        },
        "groups": { "policies": ["policy.a", "policy.b"] }
    })";

    auto res = FM::fromJson(conflicting);
    FATP_ASSERT_FALSE(res.has_value(),
        "fromJson must reject two MutuallyExclusive features both enabled=true");
    return true;
}

FATP_TEST_CASE(correct_supervisor_transition_pattern)
{
    // Full correct pattern for FeatureSupervisor policy transitions:
    //   1. Start: no alert, no policy active.
    //   2. Tick 1: alert.x fires → enable(alert.x) → policy.a implied.
    //   3. Tick 2: alert.x clears, alert.y fires →
    //              batchDisable({alert.x, policy.a}) then enable(alert.y).
    //   4. Tick 3: alert.y clears → batchDisable({alert.y, policy.b}).
    FM mgr = buildProbeFm();

    // Tick 1
    auto r1 = mgr.enable("alert.x");
    FATP_ASSERT_TRUE(r1.has_value(),            "tick1: enable must succeed");
    FATP_ASSERT_TRUE(mgr.isEnabled("policy.a"), "tick1: policy.a must be active");

    // Tick 2
    auto r2 = mgr.batchDisable({"alert.x", "policy.a"});
    FATP_ASSERT_TRUE(r2.has_value(), "tick2: batchDisable must succeed");

    auto r3 = mgr.enable("alert.y");
    FATP_ASSERT_TRUE(r3.has_value(),             "tick2: enable alert.y must succeed");
    FATP_ASSERT_TRUE(mgr.isEnabled("policy.b"),  "tick2: policy.b must be active");
    FATP_ASSERT_FALSE(mgr.isEnabled("policy.a"), "tick2: policy.a must be cleared");

    // Tick 3
    auto r4 = mgr.batchDisable({"alert.y", "policy.b"});
    FATP_ASSERT_TRUE(r4.has_value(),              "tick3: batchDisable must succeed");
    FATP_ASSERT_FALSE(mgr.isEnabled("alert.y"),   "tick3: alert.y must be cleared");
    FATP_ASSERT_FALSE(mgr.isEnabled("policy.b"),  "tick3: policy.b must be cleared");
    return true;
}

} // namespace balancer::testing::fmprobe

namespace balancer::testing
{

bool test_feature_manager_cascade_probe()
{
    FATP_PRINT_HEADER(FEATURE MANAGER CASCADE PROBE - Q2 RESOLUTION)

    TestRunner runner;

    FATP_RUN_TEST_NS(runner, fmprobe, single_alert_enables_correct_policy);
    FATP_RUN_TEST_NS(runner, fmprobe, simultaneous_alert_conflict_is_rejected);
    FATP_RUN_TEST_NS(runner, fmprobe, implies_does_not_cascade_disable_on_parent_disable);
    FATP_RUN_TEST_NS(runner, fmprobe, explicit_policy_clear_enables_transition);
    FATP_RUN_TEST_NS(runner, fmprobe, preempts_implies_fails_when_target_is_active);
    FATP_RUN_TEST_NS(runner, fmprobe, preempts_implies_succeeds_when_target_is_inactive);
    FATP_RUN_TEST_NS(runner, fmprobe, fromJson_does_not_validate_enabled_mutually_exclusive);
    FATP_RUN_TEST_NS(runner, fmprobe, correct_supervisor_transition_pattern);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_feature_manager_cascade_probe() ? 0 : 1;
}
#endif
