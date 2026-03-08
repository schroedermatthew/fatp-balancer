#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: INode
  file_role: public_header
  path: include/balancer/INode.h
  namespace: balancer
  layer: Interface
  summary: Transferable node contract — implement this interface to connect the balancer to a real cluster. Phase 8 adds reprioritise() for AgingEngine AgedEvent wiring (DEBT-006).
  api_stability: in_work
  related:
    docs_search: "INode"
    tests:
      - tests/test_INode_HeaderSelfContained.cpp
      - tests/test_node_fsm.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file INode.h
 * @brief Transferable node contract.
 *
 * INode is the architectural boundary between the balancer core and the
 * execution layer. SimulatedNode (in sim/) implements INode with FAT-P
 * ThreadPool. A production deployment implements INode with network I/O,
 * gRPC, or any other transport.
 *
 * The balancer never knows which implementation it is talking to.
 *
 * @see INode_Companion_Guide.md for design rationale.
 */

#include <functional>

#include "Expected.h"

#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"

namespace balancer
{

// ============================================================================
// Completion callback
// ============================================================================

/**
 * @brief Callback invoked exactly once per job when execution completes.
 *
 * The callback receives the completed job (with observedCost and executedBy
 * filled) and an error flag indicating whether execution succeeded.
 *
 * Called from an arbitrary thread. Must be non-blocking. Must not re-enter
 * the balancer.
 *
 * @param job          The completed job with observedCost and executedBy set.
 * @param succeeded    True if execution completed without node error.
 */
using JobCompletionCallback = std::function<void(Job job, bool succeeded)>;

// ============================================================================
// INode
// ============================================================================

/**
 * @brief Abstract node interface — the transferability boundary.
 *
 * Implementors must satisfy these thread-safety contracts:
 * - submit() is thread-safe; callers may submit concurrently from multiple threads.
 * - cancel() is thread-safe.
 * - status() is thread-safe and non-blocking; returns a snapshot.
 * - metrics() is thread-safe and non-blocking; returns a value snapshot.
 * - The completion callback is invoked exactly once per job, from any thread.
 *
 * @note INode instances are owned by SimulatedCluster (in sim/) or by the
 *       production deployment harness. The Balancer holds non-owning NodeId
 *       references. Node lifetimes must outlive the Balancer.
 */
class INode
{
public:
    virtual ~INode() = default;

    // Non-copyable, non-movable — nodes have identity.
    INode(const INode&)            = delete;
    INode& operator=(const INode&) = delete;
    INode(INode&&)                 = delete;
    INode& operator=(INode&&)      = delete;

    /**
     * @brief Returns this node's stable identity.
     * @return NodeId assigned at construction. Never changes.
     */
    [[nodiscard]] virtual NodeId id() const noexcept = 0;

    /**
     * @brief Submit a job for execution.
     *
     * On success, the node accepts ownership of the job and will invoke the
     * completion callback exactly once when the job finishes.
     *
     * On failure, the node does not invoke the callback and the caller retains
     * logical ownership.
     *
     * @param job      Job to execute. payload must be valid.
     * @param onDone   Called exactly once when the job completes or fails.
     * @return         JobHandle on success, SubmitError on rejection.
     */
    [[nodiscard]] virtual fat_p::Expected<JobHandle, SubmitError>
    submit(Job job, JobCompletionCallback onDone) = 0;

    /**
     * @brief Attempt to cancel a queued job.
     *
     * A job may only be cancelled if it has not yet started executing.
     * Cancellation is best-effort; success is not guaranteed even for
     * queued jobs (race between cancel and execution start).
     *
     * @param handle   Handle returned by a prior submit() call.
     * @return         void on success, CancelError on failure.
     */
    virtual fat_p::Expected<void, CancelError>
    cancel(JobHandle handle) = 0;

    /**
     * @brief Current lifecycle state of this node.
     *
     * Non-blocking. Returns a snapshot consistent at the time of the call.
     *
     * @return Current NodeState.
     */
    [[nodiscard]] virtual NodeState status() const noexcept = 0;

    /**
     * @brief Snapshot of this node's current load metrics.
     *
     * Non-blocking. Returns a value snapshot; no reference stability required.
     *
     * @return LoadMetrics snapshot.
     */
    [[nodiscard]] virtual LoadMetrics metrics() const noexcept = 0;

    /**
     * @brief Attempt to promote a queued job to a higher priority.
     *
     * Called by Balancer::agingLoop() when the AgingEngine fires an AgedEvent
     * for a job that has been waiting long enough to earn a priority promotion.
     * Implementations should update the job's execution priority in their
     * internal queue if the job has not yet started executing.
     *
     * **Default implementation:** Returns `CancelError::NotFound` (no-op).
     * Production INode implementations that support reprioritisation should
     * override this method.
     *
     * **Thread-safety contract:** Must be thread-safe. May be called from
     * the AgingEngine background thread concurrently with submit() and cancel().
     *
     * **Race with execution:** If the job has already started, reprioritise()
     * must return `CancelError::NotFound` — it is never an error to call
     * reprioritise() on a completing or completed job.
     *
     * @param handle       Handle returned by the submit() call for this job.
     * @param newPriority  The promoted priority assigned by AgingEngine.
     * @return             void on success (promotion applied),
     *                     CancelError::NotFound if the job is not found or
     *                     has already started executing.
     */
    [[nodiscard]] virtual fat_p::Expected<void, CancelError>
    reprioritise(JobHandle handle, Priority newPriority) noexcept
    {
        (void)handle;
        (void)newPriority;
        return fat_p::unexpected(CancelError::NotFound);
    }

    /**
     * @brief Register a callback invoked whenever this node changes state.
     *
     * Optional — nodes that do not support state change notifications may
     * implement this as a no-op. The balancer polls metrics() regardless.
     *
     * @param cb   Callback invoked with the new state from any thread.
     */
    virtual void onStateChange(std::function<void(NodeState)> cb) = 0;

protected:
    INode() = default;
};

} // namespace balancer
