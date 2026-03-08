#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: BalancerConfig
  file_role: public_header
  path: include/balancer/BalancerConfig.h
  namespace: balancer
  layer: Core
  summary: Balancer configuration aggregate with typed defaults for all subsystems, including Phase 5 affinity matrix config.
  api_stability: in_work
  related:
    tests:
      - tests/test_balancer_core.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file BalancerConfig.h
 * @brief Balancer configuration aggregate.
 *
 * All configuration lives here. Fields have typed defaults that produce a
 * reasonable out-of-the-box balancer. Override specific fields before
 * constructing the Balancer.
 *
 * CostModelConfig is defined here (not in CostModel.h) so that including
 * BalancerConfig.h does not transitively pull in the full learning model.
 * CostModel.h includes BalancerConfig.h for CostModelConfig.
 *
 * Phase 5: Added AffinityMatrixConfig. The per-JobClass correction (formerly
 * classAlpha/classWarmThreshold in CostModelConfig) now lives in
 * AffinityMatrixConfig, which configures the Tensor-backed affinity model.
 */

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace balancer
{

// ============================================================================
// AdmissionConfig
// ============================================================================

/**
 * @brief Admission control configuration.
 */
struct AdmissionConfig
{
    /// Global jobs-per-second rate limit. 0 = unlimited.
    uint32_t globalRateLimitJps = 0;

    /// Per-second limit for Normal priority. 0 = unlimited.
    uint32_t normalRateLimitJps = 0;

    /// Per-second limit for Low priority. 0 = unlimited.
    uint32_t lowRateLimitJps = 0;

    /// Per-second limit for Bulk priority. 0 = unlimited.
    uint32_t bulkRateLimitJps = 0;

    /// Maximum depth of the holding queue for Critical + High overflow
    /// when the cluster is fully saturated.
    uint32_t holdingQueueCapacity = 256;
};

// ============================================================================
// AgingConfig
// ============================================================================

/**
 * @brief Priority aging configuration.
 *
 * Phase 3 will fully activate AgingEngine. These fields are reserved.
 */
struct AgingConfig
{
    using Seconds = std::chrono::seconds;

    /// Time a Bulk job waits before aging to Low.
    Seconds bulkToLow    = Seconds{60};

    /// Time a Low job waits before aging to Normal.
    Seconds lowToNormal  = Seconds{45};

    /// Time a Normal job waits before aging to High.
    Seconds normalToHigh = Seconds{30};

    /// Whether High jobs may age to Critical. Disabled by default.
    bool    highToCriticalEnabled = false;

    /// Time a High job waits before aging to Critical (if enabled).
    Seconds highToCritical = Seconds{15};

    /// How often the aging engine scans the waiting queues.
    std::chrono::milliseconds scanInterval{500};
};

// ============================================================================
// DegradationCurveConfig
// ============================================================================

/**
 * @brief Configuration for the per-node DegradationCurve (Phase 3).
 *
 * The DegradationCurve partitions queue depth into fixed-width buckets and
 * maintains an independent EMA multiplier per bucket. When a node's queue is
 * deep, jobs experience resource contention — the curve learns this effect
 * per depth band and compounds it into predict().
 *
 * Bucket i covers queue depths in the half-open interval
 * [i × bucketSize, (i+1) × bucketSize). The last bucket is open-ended: any
 * depth at or beyond (bucketCount-1) × bucketSize maps to the final bucket.
 */
struct DegradationCurveConfig
{
    /// Number of depth buckets. Depths beyond the last bucket map to bucket[bucketCount-1].
    uint32_t bucketCount = 8;

    /// Queue depth units per bucket. Bucket i covers [i*bucketSize, (i+1)*bucketSize).
    uint32_t bucketSize = 4;

    /// EMA learning rate per bucket. Range (0, 1]. Higher = faster adaptation.
    float alpha = 0.1f;

    /// Observations per bucket required before the bucket's multiplier is applied.
    /// Below this threshold, the bucket returns 1.0 (neutral, no correction).
    uint32_t warmThreshold = 5;
};

// ============================================================================
// AffinityMatrixConfig
// ============================================================================

/**
 * @brief Configuration for the node × JobClass affinity Tensor (Phase 5).
 *
 * The AffinityMatrix is a 2-D `fat_p::RowMajorTensor<float>` of shape
 * `[maxNodes × maxJobClasses]`. Each cell holds an EMA-learned multiplier
 * representing how much the actual cost deviates from the estimate when a
 * specific job class runs on a specific node. It replaces the nested
 * `std::unordered_map` per-class correction used in Phase 2.
 *
 * Tensor-backed storage gives O(1) cell access (single multiply-add index
 * arithmetic) and enables block persistence — the entire affinity state
 * serialises as a flat JSON array rather than a recursive object tree.
 *
 * Cells with fewer than warmThreshold observations return 1.0 (neutral;
 * no correction applied). This prevents early noisy samples from biasing
 * routing when a node has only handled a handful of jobs from a given class.
 *
 * maxNodes and maxJobClasses are fixed at model construction. NodeId values
 * are used directly as row indices; JobClass values as column indices. Both
 * must fit within the configured bounds — values that exceed the bounds are
 * clamped to the last valid index rather than rejected, because the learning
 * model must never reject a completion callback.
 */
struct AffinityMatrixConfig
{
    /// Number of node rows in the tensor. NodeId.value() is used as the row
    /// index directly; IDs at or above this limit map to the last row.
    uint32_t maxNodes = 64;

    /// Number of job-class columns in the tensor. JobClass.value() is used
    /// as the column index directly; values at or above this limit map to
    /// the last column.
    uint32_t maxJobClasses = 16;

    /// EMA learning rate per (node, class) cell.
    float alpha = 0.1f;

    /// Observations per cell required before its multiplier is applied.
    /// Below this threshold the cell returns 1.0 (neutral prediction).
    uint32_t warmThreshold = 5;
};

// ============================================================================
// CostModelConfig
// ============================================================================

/**
 * @brief Configuration for the CostModel learning model.
 *
 * Defined here so BalancerConfig.h is self-contained without pulling in
 * the full CostModel implementation. CostModel.h includes this header.
 *
 * Phase 5 replaces the per-class correction fields (classAlpha,
 * classWarmThreshold) with AffinityMatrixConfig. The Tensor-backed affinity
 * matrix captures the same per-(node, class) estimation bias but in a
 * structure that supports O(1) access and block JSON persistence.
 */
struct CostModelConfig
{
    /// Jobs observed before a node's model is considered warm.
    /// During cold start, predict() returns the raw estimatedCost.
    uint32_t warmThreshold = 10;

    /// Learning rate for node throughput multiplier EMA.
    float nodeAlpha = 0.05f;

    /// Maximum throughput multiplier the model will assign.
    /// Prevents runaway predictions from corrupted observations.
    float maxMultiplier = 10.0f;

    /// Minimum throughput multiplier. Prevents the model from predicting
    /// zero cost (which would cause all work to route to one node).
    float minMultiplier = 0.1f;

    /// Queue-depth-aware degradation curve (Phase 3).
    /// Learns per-node, per-depth-bucket multipliers to account for the
    /// resource contention cost of a deep queue.
    DegradationCurveConfig degradation;

    /// Node × JobClass affinity matrix (Phase 5).
    /// Learns per-(node, class) estimation bias as an EMA-updated Tensor.
    AffinityMatrixConfig affinity;
};

// ============================================================================
// HoldingQueueConfig
// ============================================================================

/**
 * @brief Configuration for the Balancer's internal holding queue (Phase 9 / DEBT-005).
 *
 * The holding queue stores Critical and High priority jobs that arrived when
 * the cluster was fully saturated. A background drain loop in Balancer retries
 * them when a job completion frees up capacity.
 *
 * `maxRetries` limits how many times a held job may be re-attempted. A job
 * that exceeds `maxRetries` is dropped and its completion callback is invoked
 * with `succeeded = false`. Set to 0 for unlimited retries (jobs are held
 * until they dispatch or their deadline expires).
 */
struct HoldingQueueConfig
{
    /// How often the drain loop wakes if no completion event is received.
    /// Shorter intervals reduce latency under sustained overload at the cost
    /// of more lock contention.
    std::chrono::milliseconds drainInterval{50};

    /// Maximum re-dispatch attempts per held job. 0 = unlimited.
    uint32_t maxRetries = 10;
};

// ============================================================================
// CompositePolicyConfig
// ============================================================================

/**
 * @brief Child-chain configuration for the Composite scheduling policy.
 *
 * Specifies the ordered list of child policy names that PolicyFactory::makePolicy()
 * assembles when `kPolicyComposite` is requested. Names are matched using the
 * same lookup table as makePolicy() — both BalancerFeatures constants and the
 * plain-string aliases for policies that have no feature-graph entry are accepted.
 *
 * The default chain (EarliestDeadlineFirst → LeastLoaded) is a sensible
 * production starting point: deadline-aware jobs are routed first; non-deadline
 * jobs fall through to cost-model-guided least-loaded routing.
 *
 * @see PolicyFactory.h for the full set of recognised policy names.
 */
struct CompositePolicyConfig
{
    /// Ordered list of child policy names resolved by makePolicy().
    /// Valid entries: "round_robin", "least_loaded", "work_stealing",
    /// "affinity_routing", "earliest_deadline_first",
    /// "shortest_job_first", "weighted_capacity".
    /// (Use BalancerFeatures constants for the first four.)
    std::vector<std::string> chain = {"earliest_deadline_first", "least_loaded"};
};

// ============================================================================
// BalancerConfig
// ============================================================================

/**
 * @brief Top-level balancer configuration.
 */
struct BalancerConfig
{
    /// Admission control settings.
    AdmissionConfig admission;

    /// Priority aging settings (Phase 3).
    AgingConfig aging;

    /// Learning model settings.
    CostModelConfig costModel;

    /// High-water queue depth per node — transition to Overloaded when exceeded.
    uint32_t nodeOverloadThreshold = 32;

    /// Low-water queue depth per node — transition from Overloaded to Busy.
    uint32_t nodeRecoverThreshold  = 16;

    /// Enable structured diagnostic logging.
    bool loggingEnabled = true;

    /// Child-chain configuration for the Composite scheduling policy.
    /// Used by PolicyFactory::makePolicy(kPolicyComposite, ...).
    CompositePolicyConfig composite;

    /// Holding queue drain configuration (Phase 9 / DEBT-005).
    HoldingQueueConfig holdingQueue;
};

} // namespace balancer
