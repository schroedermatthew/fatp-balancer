#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: BalancerFeatures
  file_role: public_header
  path: include/balancer/BalancerFeatures.h
  namespace: balancer::features
  layer: Core
  summary: Canonical string_view constants for every FeatureManager feature name — no magic strings anywhere else. kAlertNone added as the idle/healthy alert state (Phase 11).
  api_stability: in_work
  related:
    docs_search: "BalancerFeatures"
    tests:
      - tests/test_BalancerFeatures_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file BalancerFeatures.h
 * @brief Canonical feature name constants for FeatureManager integration.
 *
 * Every feature name that FeatureSupervisor, TelemetryAdvisor, or any
 * downstream consumer passes to FeatureManager is defined exactly once here.
 * No literal feature-name strings appear anywhere else in the codebase.
 *
 * The constants are grouped to match the three sections of the Phase 7 feature
 * space:
 *
 * - **Policy group** (§3.1): MutuallyExclusive — exactly one active at a time.
 * - **Behavioral features** (§3.2): independent toggles for learning and
 *   admission subsystems.
 * - **Alert features** (§3.3): set by TelemetryAdvisor; cascade through
 *   Entails edges in the FeatureManager graph. kAlertNone is the idle state
 *   and is the initial enabled alert after FeatureSupervisor construction.
 *
 * Transferability note: this header ships to production unchanged. It has no
 * dependency on any simulation header.
 *
 * @note This header has no includes beyond `<string_view>`. It is intentionally
 *       dependency-free so it may be included anywhere in the layer stack.
 */

#include <string_view>

namespace balancer::features
{

// ============================================================================
// Policy group — MutuallyExclusive
// ============================================================================
//
// Exactly one of these may be enabled at a time. FeatureManager enforces the
// MutuallyExclusive constraint declared in the feature graph.

/// Default cold-start policy. Active when no alert has triggered an override.
inline constexpr std::string_view kPolicyRoundRobin    = "policy.round_robin";

/// Least-loaded policy. Routes each job to the node with the smallest queue.
inline constexpr std::string_view kPolicyLeastLoaded   = "policy.least_loaded";

/// Affinity-routing policy. Requires feature.affinity_matrix_warm to be set.
inline constexpr std::string_view kPolicyAffinity      = "policy.affinity_routing";

/// Work-stealing policy. Preferred under overload; implied by alert.overload.
inline constexpr std::string_view kPolicyWorkStealing  = "policy.work_stealing";

/// Composite policy. Requires feature.aging and feature.affinity_matrix.
inline constexpr std::string_view kPolicyComposite     = "policy.composite";

// ============================================================================
// Behavioral features
// ============================================================================
//
// Independent toggles for learning and admission subsystems. May be combined
// freely; no MutuallyExclusive constraint among this group.

/// Enable AffinityMatrix learning. When disabled, all cells return 1.0.
inline constexpr std::string_view kFeatureAffinityMatrix     = "feature.affinity_matrix";

/// Synthetic readiness flag set by FeatureSupervisor when enough affinity
/// cells have reached warmThreshold observations.
/// Gate: required by policy.affinity_routing before AffinityRouting activates.
inline constexpr std::string_view kFeatureAffinityMatrixWarm = "feature.affinity_matrix_warm";

/// Enable AgingEngine priority inflation for waiting jobs.
inline constexpr std::string_view kFeatureAging              = "feature.aging";

/// Enable CostModel depth-aware DegradationCurve correction.
inline constexpr std::string_view kFeatureDegradationCurve   = "feature.degradation_curve";

// ============================================================================
// Admission features
// ============================================================================
//
// Runtime admission mode overrides. These are orthogonal to per-priority token
// bucket rates and override them as named policy states.

/// Shed Bulk-priority jobs immediately when the cluster is under stress.
/// Implies: Bulk jobs are rejected at Layer 2 of AdmissionControl.
inline constexpr std::string_view kAdmissionBulkShed = "admission.bulk_shed";

/// Strict admission: accept only Critical and High; reject everything else.
/// Higher-severity override than bulk_shed.
inline constexpr std::string_view kAdmissionStrict   = "admission.strict";

// ============================================================================
// ML-triggered alert features
// ============================================================================
//
// TelemetryAdvisor sets exactly these flags. All downstream consequences
// (policy switches, admission changes) are expressed as Entails edges in the
// FeatureManager graph. The advisor has no knowledge of work stealing, aging,
// or admission tiers.
//
// kAlertNone is the idle state: no active alert. It is enabled at construction
// by FeatureSupervisor and is a member of the MutuallyExclusive alert group
// alongside the three active alerts. applyChanges("") is a synonym for
// applyChanges(kAlertNone) — the conversion happens at the top of that
// function; all callers may pass either form.

/// Idle / healthy state — no alert active.
/// Entails: policy.round_robin (default policy when cluster is healthy).
/// Initial state after FeatureSupervisor construction.
inline constexpr std::string_view kAlertNone      = "alert.none";

/// Cluster sustained-overload alert.
/// Entails: policy.work_stealing, admission.bulk_shed
inline constexpr std::string_view kAlertOverload  = "alert.overload";

/// Cluster high-latency alert.
/// Entails: policy.round_robin, admission.bulk_shed, feature.aging
inline constexpr std::string_view kAlertLatency   = "alert.latency";

/// Node-fault alert (one or more nodes in Failed state).
/// Entails: policy.round_robin, admission.strict
inline constexpr std::string_view kAlertNodeFault = "alert.node_fault";

} // namespace balancer::features
