#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: Job
  file_role: public_header
  path: include/balancer/Job.h
  namespace: balancer
  layer: Interface
  summary: Core job model — Priority, Cost, JobClass, Job aggregate, and associated StrongId types.
  api_stability: in_work
  related:
    docs_search: "Job"
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
 * @file Job.h
 * @brief Core job model for fatp-balancer.
 *
 * Defines the Job aggregate and all associated types (Priority, Cost, JobClass,
 * JobId, JobHandle, Deadline). Jobs are the unit of work submitted to the balancer.
 *
 * @see Job_User_Manual.md for usage documentation.
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ostream>

#include "Expected.h"
#include "StrongId.h"

namespace balancer
{

// ============================================================================
// Clock
// ============================================================================

/// Monotonic clock used throughout the balancer for timestamps and deadlines.
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;

// ============================================================================
// Strong ID types
// ============================================================================

/// Opaque identifier for a job. Generational via SlotMap.
using JobId    = fat_p::StrongId<uint32_t, struct JobIdTag>;

/// Opaque identifier for a cluster node.
using NodeId   = fat_p::StrongId<uint32_t, struct NodeIdTag>;

/// Job class tag for cost model grouping. JobClass{0} is the default "unclassified" class.
using JobClass = fat_p::StrongId<uint32_t, struct JobClassTag>;

/// Handle returned to the caller on successful submit. Contains JobId + generational epoch.
using JobHandle = fat_p::StrongId<uint64_t, struct JobHandleTag>;

// ============================================================================
// Priority
// ============================================================================

/**
 * @brief Job priority level.
 *
 * Five levels from Critical to Bulk. Policies use priority to determine
 * admission eligibility, queue selection, and routing decisions.
 *
 * Priority is mutable — the AgingEngine may promote a job's priority
 * after it has waited beyond its configured maxWait threshold.
 */
enum class Priority : uint8_t
{
    Critical = 0, ///< Preempts all other work. Never dropped. Never shed under load.
    High     = 1, ///< High urgency. Not preemptive but preferentially routed.
    Normal   = 2, ///< Default. Subject to standard admission control.
    Low      = 3, ///< Best-effort. Shed before Normal under load.
    Bulk     = 4, ///< Background work. First to be shed. Leaky-bucket rate limited.
};

/// Number of priority levels — used for static array sizing.
inline constexpr size_t kPriorityLevelCount = 5;

/// Convert Priority to a display string.
inline const char* priorityName(Priority p) noexcept
{
    switch (p)
    {
    case Priority::Critical: return "Critical";
    case Priority::High:     return "High";
    case Priority::Normal:   return "Normal";
    case Priority::Low:      return "Low";
    case Priority::Bulk:     return "Bulk";
    }
    return "Unknown";
}

/// Stream operator for Priority — required by FATP_ASSERT_EQ diagnostics.
inline std::ostream& operator<<(std::ostream& os, Priority p)
{
    return os << priorityName(p);
}

// ============================================================================
// Cost
// ============================================================================

/**
 * @brief Estimated or observed work units for a job.
 *
 * Cost is an opaque work-unit quantity. The submitter provides an estimated
 * cost; the node fills observedCost on completion. The CostModel uses the
 * ratio to build a learned correction factor.
 *
 * Cost{0} is valid and means "negligible work" — typically for probe or
 * heartbeat jobs.
 */
struct Cost
{
    uint64_t units = 0;

    explicit constexpr Cost(uint64_t u = 0) noexcept : units(u) {}

    constexpr bool operator==(const Cost& rhs) const noexcept { return units == rhs.units; }
    constexpr bool operator!=(const Cost& rhs) const noexcept { return units != rhs.units; }
    constexpr bool operator< (const Cost& rhs) const noexcept { return units <  rhs.units; }
    constexpr bool operator<=(const Cost& rhs) const noexcept { return units <= rhs.units; }
    constexpr bool operator> (const Cost& rhs) const noexcept { return units >  rhs.units; }
    constexpr bool operator>=(const Cost& rhs) const noexcept { return units >= rhs.units; }

    constexpr Cost operator+(const Cost& rhs) const noexcept { return Cost{units + rhs.units}; }
    constexpr Cost operator-(const Cost& rhs) const noexcept { return Cost{units - rhs.units}; }
};

/// Sentinel representing an unknown or uncalibrated cost.
inline constexpr Cost kUnknownCost{0};

// ============================================================================
// Payload
// ============================================================================

/**
 * @brief Opaque job payload.
 *
 * The balancer never inspects payload content. Payload is passed through to
 * the node's execute() function unchanged. Size is bounded to prevent
 * accidental over-allocation on the submit path.
 */
using PayloadFn = std::function<void()>;

// ============================================================================
// Job
// ============================================================================

/**
 * @brief Core job aggregate.
 *
 * Job is a plain aggregate — brace-initialized, no user-declared constructors.
 * All fields are public. The balancer fills generational fields (id, submitted)
 * on accept; callers fill everything else.
 *
 * @note priority is mutable — the AgingEngine may promote it after submission.
 *       All other fields are immutable after submission.
 */
struct Job
{
    // --- Caller-supplied fields (set before submit) ---

    /// Class tag for cost model grouping. Use JobClass{0} if no grouping needed.
    JobClass    jobClass     = JobClass{0};

    /// Caller's estimate of how much work this job requires.
    /// The node reports observedCost on completion; the CostModel learns the gap.
    Cost        estimatedCost = kUnknownCost;

    /// Hard deadline. If Clock::now() >= deadline at submit time, the job is
    /// rejected immediately with SubmitError::DeadlineUnachievable.
    std::optional<TimePoint> deadline;

    /// Maximum time this job should wait before priority aging begins.
    /// If empty, the global AgingConfig.defaultMaxWait applies.
    std::optional<Duration>  maxWait;

    /// Work to execute on the assigned node.
    PayloadFn   payload;

    // --- Balancer-filled fields (set on accept, immutable after) ---

    /// Assigned by the balancer's SlotMap on accept. Invalid until submit succeeds.
    JobId       id;

    /// Priority at the time of submission. May be mutated by AgingEngine.
    Priority    priority     = Priority::Normal;

    /// Wall time when the job was accepted by the balancer.
    TimePoint   submitted;

    // --- Node-filled fields (set on completion) ---

    /// Actual work units consumed. Filled by the node; zero until completion.
    Cost        observedCost = kUnknownCost;

    /// Node that executed the job. Invalid until completion.
    NodeId      executedBy;

    /// Number of times this job has been re-attempted from the HoldingQueue.
    /// Incremented by Balancer::holdingDrainLoop() on each failed dispatch attempt.
    /// Zero for jobs that were never held. Phase 9 (DEBT-005).
    uint32_t    holdRetries{0};

    /// Total queue depth on the assigned node at the moment this job was dispatched.
    /// Set by the node in submit() from the current mQueueDepth snapshot.
    /// Used by CostModel::DegradationCurve to learn the depth-vs-latency relationship.
    /// Scheduling policies may also write this field before calling predict() to obtain
    /// a depth-aware cost estimate for a candidate node.
    uint32_t    queueDepthAtDispatch = 0;
};

// ============================================================================
// Error types
// ============================================================================

/**
 * @brief Errors returned by Balancer::submit().
 */
enum class SubmitError : uint8_t
{
    /// Global ingestion rate limit exceeded.
    RateLimited,

    /// Per-priority rate limit exceeded (typically Bulk or Low under load).
    PriorityRejected,

    /// All nodes are Overloaded; job priority too low for holding queue admission.
    ClusterSaturated,

    /// Holding queue for high-priority overflow is full.
    HoldingQueueFull,

    /// Job's deadline has already passed or cannot be met given current load.
    DeadlineUnachievable,

    /// A specific target node was requested but does not exist.
    NodeNotFound,
};

/// Convert SubmitError to a display string.
inline const char* submitErrorName(SubmitError e) noexcept
{
    switch (e)
    {
    case SubmitError::RateLimited:          return "RateLimited";
    case SubmitError::PriorityRejected:     return "PriorityRejected";
    case SubmitError::ClusterSaturated:     return "ClusterSaturated";
    case SubmitError::HoldingQueueFull:     return "HoldingQueueFull";
    case SubmitError::DeadlineUnachievable: return "DeadlineUnachievable";
    case SubmitError::NodeNotFound:         return "NodeNotFound";
    }
    return "Unknown";
}

/**
 * @brief Errors returned by Balancer::cancel().
 */
enum class CancelError : uint8_t
{
    /// No job with the given handle exists (already completed or never submitted).
    NotFound,

    /// Job has already started execution and cannot be cancelled.
    AlreadyRunning,

    /// Critical jobs may not be cancelled by policy.
    CriticalNotCancellable,
};

} // namespace balancer
