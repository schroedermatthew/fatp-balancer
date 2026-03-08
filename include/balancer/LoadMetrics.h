#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: LoadMetrics
  file_role: public_header
  path: include/balancer/LoadMetrics.h
  namespace: balancer
  layer: Interface
  summary: Per-node and cluster-level load metrics snapshots.
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
 * @file LoadMetrics.h
 * @brief Per-node and cluster load metrics.
 *
 * LoadMetrics is a value snapshot — it is cheap to copy and contains no
 * references into node internals. Scheduling policies receive a ClusterView
 * built from a consistent snapshot of all node metrics.
 */

#include <array>
#include <cstdint>
#include <cstddef>

#include "balancer/Job.h"

namespace balancer
{

// ============================================================================
// NodeState
// ============================================================================

/**
 * @brief Lifecycle state of a cluster node.
 *
 * @see NodeStateMachine for valid transitions.
 */
enum class NodeState : uint8_t
{
    Offline,        ///< Not participating. Rejects all jobs.
    Initializing,   ///< Starting up. Rejects all jobs.
    Idle,           ///< Ready and lightly loaded. Accepts all priorities.
    Busy,           ///< Actively processing. Accepts all priorities.
    Overloaded,     ///< Queue depth above high-water mark. Accepts Critical + High only.
    Draining,       ///< Preparing for shutdown. Accepts Critical only.
    Failed,         ///< Unhealthy. Rejects all jobs. Pending recovery.
    Recovering,     ///< Returning from Failed. Accepts Critical + High only.
};

/// Convert NodeState to a display string.
inline const char* nodeStateName(NodeState s) noexcept
{
    switch (s)
    {
    case NodeState::Offline:      return "Offline";
    case NodeState::Initializing: return "Initializing";
    case NodeState::Idle:         return "Idle";
    case NodeState::Busy:         return "Busy";
    case NodeState::Overloaded:   return "Overloaded";
    case NodeState::Draining:     return "Draining";
    case NodeState::Failed:       return "Failed";
    case NodeState::Recovering:   return "Recovering";
    }
    return "Unknown";
}

/**
 * @brief Returns true if the given node state accepts jobs of the given priority.
 *
 * Admission eligibility by state:
 *
 * | State        | Critical | High | Normal | Low | Bulk |
 * |--------------|----------|------|--------|-----|------|
 * | Offline      |   N      |  N   |   N    |  N  |  N   |
 * | Initializing |   N      |  N   |   N    |  N  |  N   |
 * | Idle         |   Y      |  Y   |   Y    |  Y  |  Y   |
 * | Busy         |   Y      |  Y   |   Y    |  Y  |  Y   |
 * | Overloaded   |   Y      |  Y   |   N    |  N  |  N   |
 * | Draining     |   Y      |  N   |   N    |  N  |  N   |
 * | Failed       |   N      |  N   |   N    |  N  |  N   |
 * | Recovering   |   Y      |  Y   |   N    |  N  |  N   |
 */
[[nodiscard]] inline bool nodeAcceptsPriority(NodeState state, Priority priority) noexcept
{
    switch (state)
    {
    case NodeState::Idle:
    case NodeState::Busy:
        return true;

    case NodeState::Overloaded:
    case NodeState::Recovering:
        return priority == Priority::Critical || priority == Priority::High;

    case NodeState::Draining:
        return priority == Priority::Critical;

    case NodeState::Offline:
    case NodeState::Initializing:
    case NodeState::Failed:
        return false;
    }
    return false;
}

// ============================================================================
// LoadMetrics
// ============================================================================

/**
 * @brief Snapshot of a single node's load and performance metrics.
 *
 * LoadMetrics is a value type — copy freely. All fields are set by the node
 * on the metrics() call and are consistent at the time of the snapshot.
 *
 * @note Thread-safety: LoadMetrics itself has no thread-safety requirements
 *       (it is a value). The node's metrics() method must be thread-safe.
 */
struct LoadMetrics
{
    /// Node this snapshot belongs to.
    NodeId      nodeId;

    /// Current lifecycle state.
    NodeState   state      = NodeState::Offline;

    /// Fraction of maximum capacity in use. Range [0.0, 1.0].
    /// 0.0 = idle, 1.0 = fully saturated.
    float       utilization = 0.0f;

    /// Total jobs currently queued or executing across all priority bands.
    uint32_t    queueDepth  = 0;

    /// Queue depth per priority band. Index matches Priority enum value.
    std::array<uint32_t, kPriorityLevelCount> queueDepthByPriority = {};

    /// Rolling p50 job latency in microseconds (0 if no completed jobs yet).
    uint64_t    p50LatencyUs = 0;

    /// Rolling p99 job latency in microseconds (0 if no completed jobs yet).
    uint64_t    p99LatencyUs = 0;

    /// Total jobs completed since node started.
    uint64_t    completedJobs = 0;

    /// Total jobs that failed (node error, not business logic error) since start.
    uint64_t    failedJobs    = 0;

    /// Total jobs whose priority was bumped by AgingEngine while waiting on this node.
    uint64_t    agedJobs      = 0;

    /// Total jobs that expired (missed deadline) while waiting on this node.
    uint64_t    expiredJobs   = 0;

    /// Total jobs stolen from this node by WorkStealing policy.
    uint64_t    stolenJobs    = 0;

    /// Wall time when the node last changed state.
    TimePoint   lastStateChange;
};

// ============================================================================
// ClusterMetrics
// ============================================================================

/**
 * @brief Aggregate metrics across the entire cluster.
 *
 * ClusterMetrics is computed by the Balancer from a consistent snapshot of
 * all NodeMetrics. It is passed to scheduling policies as part of ClusterView.
 */
struct ClusterMetrics
{
    /// Total nodes represented by this snapshot, regardless of state.
    uint32_t knownNodes = 0;

    /// Number of nodes currently in Idle or Busy state.
    uint32_t activeNodes     = 0;

    /// Number of nodes in Failed or Offline state.
    uint32_t unavailableNodes = 0;

    /// Number of nodes currently in Overloaded state.
    uint32_t overloadedNodes = 0;

    /// Total jobs submitted across all nodes since balancer start.
    uint64_t totalSubmitted   = 0;

    /// Total jobs completed across all nodes since balancer start.
    uint64_t totalCompleted   = 0;

    /// Total submit attempts rejected by admission control since balancer start.
    uint64_t totalRejected    = 0;

    /// Rejection breakdown by error code. Indexed by SubmitError enum value.
    std::array<uint64_t, 6> rejectionsByCode = {};

    /// Current depth of the high-priority holding queue (Critical + High overflow).
    uint32_t holdingQueueDepth = 0;

    /// Cluster-wide throughput in jobs per second (rolling 1-second window).
    float    throughputPerSecond = 0.0f;

    /// Mean p50 latency across all active nodes in microseconds.
    uint64_t meanP50LatencyUs = 0;

    /// Maximum p99 latency across all active nodes in microseconds.
    uint64_t maxP99LatencyUs  = 0;
};

} // namespace balancer
