#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: HoldingQueue
  file_role: public_header
  path: include/balancer/HoldingQueue.h
  namespace: balancer
  layer: Core
  summary: >
    Bounded, priority-ordered holding queue for Critical and High overflow jobs.
    Resolves DEBT-005: AdmissionControl signals "hold this job" but previously
    no implementation existed to store and retry held jobs. HoldingQueue is the
    backing store; Balancer owns one instance and drains it when cluster capacity
    frees up.
  api_stability: in_work
  related:
    docs_search: "HoldingQueue"
    tests:
      - tests/test_HoldingQueue_HeaderSelfContained.cpp
      - tests/test_holding_queue.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file HoldingQueue.h
 * @brief Bounded, priority-ordered holding queue for overflow high-priority jobs.
 *
 * ## Background (DEBT-005)
 *
 * `AdmissionControl::evaluate()` returns success (no error) for Critical and
 * High priority jobs even when the cluster is fully saturated, simultaneously
 * incrementing `mHoldingQueueDepth` to record that a slot is logically
 * occupied. Previously, `Balancer::submit()` would proceed to `selectNode()`
 * which returned `NoneEligible` — the job was silently dropped while the
 * depth counter leaked.
 *
 * `HoldingQueue` is the concrete implementation that fulfils the contract
 * `AdmissionControl` assumed. When a Critical/High job cannot be dispatched
 * because all nodes are overloaded, it is enqueued here. A background drain
 * loop in `Balancer` resubmits held jobs when completion callbacks signal
 * that cluster capacity has been freed.
 *
 * ## Handle encoding
 *
 * Held jobs are assigned a `JobHandle` with bit 63 set. This distinguishes
 * them from node-dispatched handles (which encode a `SlotMapHandle` in the
 * lower 63 bits). `Balancer::cancel()` inspects bit 63 to route cancel calls
 * to the correct path.
 *
 * ```
 * Node handle:    0 | slot_generation(31 bits) | slot_index(32 bits)
 * Holding handle: 1 | holding_sequence_id(63 bits)
 * ```
 *
 * ## Priority ordering
 *
 * Critical jobs are dequeued before High jobs. Within the same priority,
 * order is FIFO (insertion order). This is implemented via two separate
 * `std::deque`s rather than a single sorted container, avoiding per-entry
 * comparator overhead.
 *
 * ## Thread-safety
 *
 * All public methods are thread-safe. `enqueue()` and `tryDequeue()` are the
 * hot path; they acquire `mMutex` for the minimal critical section.
 *
 * ## Transferability
 *
 * This header does not include any `sim/` header and ships unchanged to
 * production deployments.
 */

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

#include "balancer/INode.h"
#include "balancer/Job.h"

namespace balancer
{

// ============================================================================
// Constants
// ============================================================================

/// Bit mask applied to a raw JobHandle value to mark it as a holding handle.
inline constexpr uint64_t kHoldingHandleBit = uint64_t{1} << 63;

/// Test whether a raw JobHandle value encodes a holding-queue handle.
[[nodiscard]] inline constexpr bool isHoldingHandle(uint64_t raw) noexcept
{
    return (raw & kHoldingHandleBit) != 0;
}

// ============================================================================
// HoldingQueue
// ============================================================================

/**
 * @brief Bounded, priority-ordered store for overflow Critical/High jobs.
 *
 * Jobs are dequeued in Critical-before-High, FIFO-within-priority order.
 * The queue rejects enqueue when `size() >= capacity`.
 *
 * Capacity is set at construction and is immutable. It mirrors
 * `AdmissionConfig::holdingQueueCapacity` — the `AdmissionControl` counter
 * and this queue's actual size must stay in agreement; `Balancer` is
 * responsible for calling `AdmissionControl::releaseFromHoldingQueue()` when
 * a held job is successfully dispatched or definitively dropped.
 */
class HoldingQueue
{
public:
    // ---- Entry type --------------------------------------------------------

    /**
     * @brief A single held job and its associated state.
     */
    struct Entry
    {
        Job                    job;           ///< The job to (re)submit.
        JobCompletionCallback  onDone;        ///< Original completion callback.
        uint64_t               holdHandle{0}; ///< Raw holding-handle value (bit 63 set).
    };

    // ---- Construction ------------------------------------------------------

    /**
     * @brief Construct with a maximum capacity.
     *
     * @param capacity  Maximum number of jobs this queue will hold.
     *                  Matches `AdmissionConfig::holdingQueueCapacity`.
     */
    explicit HoldingQueue(uint32_t capacity) noexcept
        : mCapacity(capacity)
    {}

    // Non-copyable, non-movable — owns job callbacks.
    HoldingQueue(const HoldingQueue&)            = delete;
    HoldingQueue& operator=(const HoldingQueue&) = delete;
    HoldingQueue(HoldingQueue&&)                 = delete;
    HoldingQueue& operator=(HoldingQueue&&)      = delete;

    // ---- Core API ----------------------------------------------------------

    /**
     * @brief Enqueue a held job.
     *
     * Assigns a unique holding handle (bit 63 set) and stores the job.
     * Returns `HoldingQueueFull` if capacity is already reached.
     *
     * Thread-safe.
     *
     * @param job     Job to hold. `job.priority` must be Critical or High.
     * @param onDone  Original completion callback.
     * @return        Holding `JobHandle` on success, `HoldingQueueFull` on failure.
     */
    [[nodiscard]] fat_p::Expected<JobHandle, SubmitError>
    enqueue(Job job, JobCompletionCallback onDone)
    {
        std::lock_guard lock(mMutex);

        if (mSize >= mCapacity)
        {
            return fat_p::unexpected(SubmitError::HoldingQueueFull);
        }

        const uint64_t seq       = mNextSeq.fetch_add(1, std::memory_order_relaxed);
        const uint64_t rawHandle = kHoldingHandleBit | seq;

        Entry entry;
        entry.job        = std::move(job);
        entry.onDone     = std::move(onDone);
        entry.holdHandle = rawHandle;

        if (entry.job.priority == Priority::Critical)
        {
            mCritical.push_back(std::move(entry));
        }
        else
        {
            mHigh.push_back(std::move(entry));
        }

        ++mSize;
        return JobHandle{rawHandle};
    }

    /**
     * @brief Dequeue the highest-priority waiting job.
     *
     * Returns the entry with the earliest-inserted Critical job, or if
     * the critical queue is empty, the earliest High job.
     * Returns `std::nullopt` if empty.
     *
     * Thread-safe.
     */
    [[nodiscard]] std::optional<Entry> tryDequeue()
    {
        std::lock_guard lock(mMutex);

        if (!mCritical.empty())
        {
            Entry e = std::move(mCritical.front());
            mCritical.pop_front();
            --mSize;
            return e;
        }
        if (!mHigh.empty())
        {
            Entry e = std::move(mHigh.front());
            mHigh.pop_front();
            --mSize;
            return e;
        }
        return std::nullopt;
    }

    /**
     * @brief Cancel a held job by its holding handle.
     *
     * Removes the entry whose `holdHandle` matches `rawHandle`.
     * Returns `true` if the job was found and removed, `false` if not found
     * (it may have been dispatched already).
     *
     * Thread-safe.
     *
     * @param rawHandle  Raw value of the JobHandle (bit 63 must be set).
     */
    bool cancel(uint64_t rawHandle)
    {
        std::lock_guard lock(mMutex);
        return cancelFrom(mCritical, rawHandle) ||
               cancelFrom(mHigh,     rawHandle);
    }

    // ---- Observability -----------------------------------------------------

    /// Current number of held jobs. Thread-safe (relaxed read).
    [[nodiscard]] uint32_t size() const noexcept
    {
        std::lock_guard lock(mMutex);
        return mSize;
    }

    /// True when no jobs are held. Thread-safe.
    [[nodiscard]] bool empty() const noexcept
    {
        std::lock_guard lock(mMutex);
        return mSize == 0;
    }

    /// Configured maximum capacity.
    [[nodiscard]] uint32_t capacity() const noexcept { return mCapacity; }

private:
    bool cancelFrom(std::deque<Entry>& q, uint64_t rawHandle)
    {
        for (auto it = q.begin(); it != q.end(); ++it)
        {
            if (it->holdHandle == rawHandle)
            {
                q.erase(it);
                --mSize;
                return true;
            }
        }
        return false;
    }

    const uint32_t               mCapacity;
    uint32_t                     mSize{0};
    std::deque<Entry>            mCritical;
    std::deque<Entry>            mHigh;
    mutable std::mutex           mMutex;
    std::atomic<uint64_t>        mNextSeq{1};
};

} // namespace balancer
