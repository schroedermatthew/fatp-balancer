#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: TelemetryAdvisor
  file_role: public_header
  path: include/balancer/TelemetryAdvisor.h
  namespace: balancer
  layer: Core
  summary: >
    Reads ClusterMetrics each tick and drives FeatureSupervisor::applyChanges()
    when alert thresholds are crossed. Implements priority-ordered alert
    evaluation (node_fault > overload > latency > none). All three alert tiers
    are fully active: node_fault, overload, and latency (meanP50LatencyUs).
  api_stability: in_work
  related:
    docs_search: "TelemetryAdvisor"
    tests:
      - tests/test_TelemetryAdvisor_HeaderSelfContained.cpp
      - tests/test_telemetry_advisor.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file TelemetryAdvisor.h
 * @brief Threshold-based alert evaluation and FeatureSupervisor driver.
 *
 * TelemetryAdvisor is a thin, stateless evaluator that sits between raw
 * ClusterMetrics and the FeatureSupervisor. It knows only about metric
 * thresholds and alert priority. It does not know about work stealing, aging,
 * admission tiers, or any other consequence of an alert — those are all
 * expressed in the feature graph owned by FeatureSupervisor.
 *
 * ### Alert priority (highest to lowest)
 *
 * | Alert             | Condition                                                   |
 * |-------------------|-------------------------------------------------------------|
 * | `kAlertNodeFault` | unavailableNodes / totalNodes >= unavailableFraction        |
 * | `kAlertOverload`  | activeNodes / totalNodes < overloadFraction                 |
 * | `kAlertLatency`   | meanP50LatencyUs >= latencyThresholdUs                      |
 * | *(none)*          | No threshold breached                                       |
 *
 * totalNodes is computed as `activeNodes + unavailableNodes`. Nodes in
 * transient states (Draining, Recovering, Overloaded, Initializing) do not
 * contribute to either counter. If totalNodes is zero, no alert is fired.
 *
 * ### Usage
 *
 * ```cpp
 * TelemetryAdvisor advisor{supervisor};
 *
 * // In the simulation tick:
 * auto result = advisor.evaluate(cluster.metrics());
 * if (!result) { /* log error *\/ }
 * ```
 *
 * ### Transferability
 *
 * This header does not include any `sim/` header. It may be included from
 * production code and WASM bindings.
 *
 * @see FeatureSupervisor.h for the feature graph and policy transition logic.
 * @see LoadMetrics.h for ClusterMetrics field definitions.
 */

#include <string>
#include <string_view>

#include "Expected.h"

#include "balancer/BalancerFeatures.h"
#include "balancer/FeatureSupervisor.h"
#include "balancer/LoadMetrics.h"

namespace balancer
{

// ============================================================================
// TelemetryThresholds
// ============================================================================

/**
 * @brief Threshold parameters that govern TelemetryAdvisor's alert evaluation.
 *
 * All fractions are in the range [0.0, 1.0]. The defaults are conservative
 * starting points; tune for the deployment's observed traffic patterns.
 *
 * @note `latencyThresholdUs` is reserved for Phase 8. TelemetryAdvisor does
 *       not evaluate it while DEBT-002 is unresolved — the corresponding
 *       ClusterMetrics fields are always zero.
 */
struct TelemetryThresholds
{
    /// Fraction of total nodes (activeNodes + unavailableNodes) that must be
    /// unavailable before `kAlertNodeFault` is fired.
    ///
    /// Range (0.0, 1.0]. Default: 0.25 (25% of nodes unavailable).
    float unavailableFraction = 0.25f;

    /// Fraction of total nodes that must be active for the cluster to be
    /// considered healthy. When `activeNodes / totalNodes` drops below this
    /// value the cluster is considered overloaded and `kAlertOverload` is
    /// fired — unless `kAlertNodeFault` is already triggered, which takes
    /// priority.
    ///
    /// Range (0.0, 1.0]. Default: 0.75 (fewer than 75% of nodes active).
    ///
    /// To prevent `kAlertOverload` from firing before `kAlertNodeFault`,
    /// `overloadFraction` should be >= `(1.0f - unavailableFraction)`.
    /// At the defaults (0.75 and 0.25) both thresholds trip at the same
    /// active/unavailable ratio; `kAlertNodeFault` wins by priority ordering.
    float overloadFraction = 0.75f;

    /// Mean p50 latency in microseconds above which `kAlertLatency` fires.
    ///
    /// When `ClusterMetrics::meanP50LatencyUs` reaches or exceeds this value
    /// and neither `kAlertNodeFault` nor `kAlertOverload` is active, the
    /// advisor fires `kAlertLatency`. Default: 10 ms (10,000 µs).
    uint64_t latencyThresholdUs = 10'000;
};

// ============================================================================
// TelemetryAdvisor
// ============================================================================

/**
 * @brief Translates ClusterMetrics into alert transitions on a FeatureSupervisor.
 *
 * Call `evaluate()` once per cluster tick. TelemetryAdvisor computes the
 * highest-priority alert whose threshold is breached (or no alert if the
 * cluster is healthy) and forwards the result to `FeatureSupervisor::applyChanges()`.
 *
 * TelemetryAdvisor does **not** own the FeatureSupervisor — the caller is
 * responsible for ensuring the supervisor outlives the advisor.
 *
 * ## Intent
 *
 * Scheduling policy selection and behavioral feature toggling are the
 * responsibility of FeatureSupervisor. TelemetryAdvisor concerns itself only
 * with the question: *"given these raw numbers, which alert name should be
 * active right now?"*
 *
 * ## Invariant
 *
 * After a successful `evaluate()` call, the supervisor's active alert matches
 * the alert determined by TelemetryAdvisor's threshold logic for the snapshot
 * that was passed in.
 *
 * ## Complexity model
 *
 * `evaluate()` is O(1): three floating-point comparisons plus one
 * `applyChanges()` call (which is O(F) in the number of features, a small
 * constant in Phase 7).
 *
 * ## Concurrency model
 *
 * TelemetryAdvisor has no internal mutable state; all mutable state lives in
 * the FeatureSupervisor (which uses `MutexSynchronizationPolicy`). Concurrent
 * `evaluate()` calls are therefore safe as long as the supervisor is
 * thread-safe, but callers should avoid concurrent evaluation of the same
 * snapshot — the second call is redundant and adds contention.
 *
 * ## Error model
 *
 * `evaluate()` returns the `Expected` produced by `applyChanges()`. A failure
 * means the feature-graph transition was rejected; the supervisor remains in
 * its prior state. Callers should log and continue rather than treating this
 * as fatal — a rejected transition leaves the cluster in its last known good
 * alert state.
 */
class TelemetryAdvisor
{
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /**
     * @brief Constructs a TelemetryAdvisor wired to @p supervisor.
     *
     * @param supervisor   The FeatureSupervisor to drive. Must outlive this object.
     * @param thresholds   Alert thresholds. Defaults are safe for most clusters.
     */
    explicit TelemetryAdvisor(FeatureSupervisor&       supervisor,
                              TelemetryThresholds      thresholds = {}) noexcept
        : mSupervisor(supervisor)
        , mThresholds(thresholds)
    {
    }

    TelemetryAdvisor(const TelemetryAdvisor&)            = delete;
    TelemetryAdvisor& operator=(const TelemetryAdvisor&) = delete;
    TelemetryAdvisor(TelemetryAdvisor&&)                 = delete;
    TelemetryAdvisor& operator=(TelemetryAdvisor&&)      = delete;

    // -------------------------------------------------------------------------
    // Primary API
    // -------------------------------------------------------------------------

    /**
     * @brief Evaluate @p metrics and apply the appropriate alert to the supervisor.
     *
     * Computes the highest-priority alert whose threshold is currently breached,
     * then calls `supervisor.applyChanges()` with that alert name (or `""` if
     * the cluster is healthy). The call is idempotent — if the determined alert
     * matches the supervisor's current active alert, `applyChanges()` returns
     * immediately.
     *
     * @param metrics  A ClusterMetrics snapshot from the current tick.
     * @return `{}` on success. An error string if `applyChanges()` rejected
     *         the transition (the supervisor stays in its prior state).
     */
    [[nodiscard]] fat_p::Expected<void, std::string>
    evaluate(const ClusterMetrics& metrics)
    {
        const std::string_view alert = determineAlert(metrics);
        return mSupervisor.applyChanges(alert);
    }

    // -------------------------------------------------------------------------
    // Query
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the alert name that was active after the most recent
     *        successful `evaluate()` call, or `""` if no alert is active.
     *
     * Delegates to the supervisor — this is the ground-truth alert state.
     */
    [[nodiscard]] std::string_view currentAlert() const noexcept
    {
        return mSupervisor.activeAlert();
    }

    /// Returns the thresholds in use by this advisor.
    [[nodiscard]] const TelemetryThresholds& thresholds() const noexcept
    {
        return mThresholds;
    }

    /// Returns a reference to the supervised FeatureSupervisor.
    [[nodiscard]] const FeatureSupervisor& supervisor() const noexcept
    {
        return mSupervisor;
    }

private:
    // -------------------------------------------------------------------------
    // Alert evaluation
    // -------------------------------------------------------------------------

    /**
     * @brief Compute the highest-priority alert for the given metrics snapshot.
     *
     * Priority order: `kAlertNodeFault` > `kAlertOverload` > `kAlertLatency` > `""`.
     *
     * `total` is `knownNodes` — all nodes represented by the snapshot regardless
     * of state. This prevents a zero denominator when all nodes are overloaded.
     * Returns `""` when `knownNodes` is zero (empty cluster).
     */
    [[nodiscard]] std::string_view determineAlert(const ClusterMetrics& m) const noexcept
    {
        const uint32_t total = m.knownNodes;

        // Empty cluster — no meaningful alert can be derived.
        if (total == 0)
        {
            return "";
        }

        const float unavailFrac =
            static_cast<float>(m.unavailableNodes) / static_cast<float>(total);
        const float activeFrac =
            static_cast<float>(m.activeNodes) / static_cast<float>(total);

        // node_fault: enough nodes down that routing must be altered.
        if (unavailFrac >= mThresholds.unavailableFraction)
        {
            return features::kAlertNodeFault;
        }

        // overload: too few active nodes to serve load without policy intervention.
        if (activeFrac < mThresholds.overloadFraction)
        {
            return features::kAlertOverload;
        }

        // latency: cluster topology is healthy but response times are elevated.
        if (m.meanP50LatencyUs >= mThresholds.latencyThresholdUs)
        {
            return features::kAlertLatency;
        }

        return "";
    }

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------

    FeatureSupervisor&   mSupervisor;
    TelemetryThresholds  mThresholds;
};

} // namespace balancer
