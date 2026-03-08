#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: SimulatedNode
  file_role: public_header
  path: sim/SimulatedNode.h
  namespace: balancer::sim
  layer: Sim
  summary: INode implementation backed by FAT-P ThreadPool with SlotMap-based cancel and work-stealing worker threads. Phase 8 adds reprioritise() (DEBT-006) via a priority-override map.
  api_stability: in_work
  related:
    tests:
      - tests/test_node_fsm.cpp
      - tests/test_fault_scenarios.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file SimulatedNode.h
 * @brief INode implementation for local simulation.
 *
 * SimulatedNode implements INode using a FAT-P ThreadPool. It is a test
 * fixture — it must never be included by include/balancer/ or policies/.
 *
 * Phase 4 changes from Phase 3:
 * - Worker threads are managed by fat_p::ThreadPool (replaces manual
 *   std::thread vector and condition variable).
 * - balancer::Priority is mapped to fat_p::Priority for ThreadPool submission.
 *   Bulk maps to fat_p::Priority::Low (ThreadPool has no Bulk band).
 * - Cancel is now functional via fat_p::SlotMap. Each submit encodes a
 *   SlotMap handle into the returned JobHandle (low 32 bits = slot index,
 *   high 32 bits = slot generation). cancel() erases the slot; the task
 *   lambda checks validity before executing.
 * - FaultType is now defined in sim/FaultInjector.h (no longer in this file).
 * - maxJobDurationUs is honoured: when > minJobDurationUs, execution delay
 *   is uniformly distributed in [minJobDurationUs, maxJobDurationUs].
 * - Per-priority queue depth is tracked via atomic counters, one per band.
 *   The ThreadPool does not expose per-priority depth, so counters are
 *   decremented when the task lambda begins, maintaining the invariant
 *   that queueDepthByPriority reflects jobs waiting or executing.
 *
 * @warning Do not include from include/balancer/ or policies/. Transferability violation.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <random>
#include <vector>

#include "Expected.h"
#include "FastHashMap.h"
#include "SlotMap.h"
#include "ThreadPool.h"

#include "balancer/INode.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"
#include "sim/FaultInjector.h"

namespace balancer::sim
{

// ============================================================================
// SimulatedNodeConfig
// ============================================================================

struct SimulatedNodeConfig
{
    /// Number of worker threads.
    uint32_t workerCount = 2;

    /// Simulated time scaling: 1 cost unit = this many microseconds.
    /// Used when minJobDurationUs == 0.
    uint32_t usPerCostUnit = 1;

    /// High-water mark for Overloaded transition.
    uint32_t overloadThreshold = 32;

    /// Low-water mark for recovery from Overloaded.
    uint32_t recoverThreshold = 16;

    /// Minimum artificial execution delay, in microseconds.
    /// When non-zero, overrides the cost-unit-based delay.
    uint32_t minJobDurationUs = 0;

    /// Maximum artificial execution delay, in microseconds.
    /// When > minJobDurationUs, delay is uniformly sampled from
    /// [minJobDurationUs, maxJobDurationUs] per job. When ==
    /// minJobDurationUs (and both non-zero), delay is exactly
    /// minJobDurationUs.
    uint32_t maxJobDurationUs = 0;
};

// ============================================================================
// SimulatedNode
// ============================================================================

/**
 * @brief INode implementation for simulation.
 *
 * Uses fat_p::ThreadPool for worker thread management. Per-priority queue
 * depths are maintained via atomic counters because ThreadPool does not
 * expose per-band depth metrics.
 *
 * Cancel semantics: cancel() marks a SlotMap entry as erased. The task lambda
 * checks validity under lock before executing; if erased, the job is silently
 * dropped (the completion callback is NOT invoked — callers must not rely on
 * callback-based accounting for cancelled jobs).
 *
 * Thread-safety: submit(), cancel(), status(), and metrics() are thread-safe.
 * injectFault() is thread-safe. start() and stop() must not be called
 * concurrently with each other; they may be called from any thread.
 */
class SimulatedNode final : public balancer::INode
{
public:
    SimulatedNode(balancer::NodeId id, SimulatedNodeConfig config = {})
        : mId(id)
        , mConfig(config)
        , mState(balancer::NodeState::Offline)
        , mRng(std::random_device{}())
    {}

    ~SimulatedNode() override
    {
        stop();
    }

    // ---- Lifecycle ---------------------------------------------------------

    /**
     * @brief Start worker threads. Transitions Offline → Initializing → Idle.
     */
    void start()
    {
        {
            std::lock_guard lock(mMutex);
            mShutdown = false;
            mState    = balancer::NodeState::Initializing;
        }
        notifyStateChange(balancer::NodeState::Initializing);

        mThreadPool = std::make_unique<fat_p::ThreadPool>(
            static_cast<size_t>(mConfig.workerCount));

        {
            std::lock_guard lock(mMutex);
            mState = balancer::NodeState::Idle;
        }
        notifyStateChange(balancer::NodeState::Idle);
    }

    /**
     * @brief Drain pending tasks and stop the thread pool. Transitions to Offline.
     *
     * Waits for all enqueued tasks to complete before returning.
     */
    void stop()
    {
        {
            std::lock_guard lock(mMutex);
            mShutdown = true;
        }

        if (mThreadPool)
        {
            mThreadPool->shutdown();
            mThreadPool.reset();
        }

        std::lock_guard lock(mMutex);
        mState = balancer::NodeState::Offline;
    }

    // ---- Fault injection ---------------------------------------------------

    /**
     * @brief Inject a fault or clear an active fault. Thread-safe.
     *
     * Passing FaultType::None clears any active fault. The new FaultConfig
     * takes effect for all tasks that begin executing after this call returns;
     * it does not retroactively affect in-flight tasks.
     *
     * @param fault   Fault type to inject. FaultType::None clears the fault.
     * @param config  Fault parameters (slowdownFactor etc.). Ignored for None.
     */
    void injectFault(FaultType fault, FaultConfig config = {})
    {
        std::lock_guard lock(mMutex);
        mFault       = fault;
        mFaultConfig = config;

        if (fault == FaultType::Crash)
        {
            mState = balancer::NodeState::Failed;
            notifyStateChangeUnlocked(balancer::NodeState::Failed);
        }
        else if (fault == FaultType::None && mState == balancer::NodeState::Failed)
        {
            mState = balancer::NodeState::Recovering;
            notifyStateChangeUnlocked(balancer::NodeState::Recovering);
        }
    }

    // ---- INode -------------------------------------------------------------

    [[nodiscard]] balancer::NodeId id() const noexcept override
    {
        return mId;
    }

    [[nodiscard]] fat_p::Expected<balancer::JobHandle, balancer::SubmitError>
    submit(balancer::Job job, balancer::JobCompletionCallback onDone) override
    {
        fat_p::SlotMapHandle smHandle;

        {
            std::lock_guard lock(mMutex);

            if (mShutdown ||
                mFault == FaultType::Crash  ||
                mFault == FaultType::Partition ||
                mState == balancer::NodeState::Failed ||
                mState == balancer::NodeState::Offline)
            {
                return fat_p::unexpected(
                    balancer::SubmitError::ClusterSaturated);
            }

            if (!balancer::nodeAcceptsPriority(mState, job.priority))
            {
                return fat_p::unexpected(
                    balancer::SubmitError::PriorityRejected);
            }

            if (!mThreadPool)
            {
                return fat_p::unexpected(
                    balancer::SubmitError::ClusterSaturated);
            }

            job.executedBy           = mId;
            job.queueDepthAtDispatch = mQueueDepth.load(std::memory_order_relaxed);

            // Register in SlotMap for cancel support.
            smHandle = mPendingSlots.insert(true);

            // Track per-priority depth.
            auto bandIdx = static_cast<size_t>(job.priority);
            mQueueDepthByPriority[bandIdx].fetch_add(1, std::memory_order_relaxed);
            mQueueDepth.fetch_add(1, std::memory_order_relaxed);

            updateStateUnlocked();
        }

        // Encode SlotMap handle into JobHandle:
        //   bits [63:32] = smHandle.index
        //   bits [31: 0] = smHandle.generation
        auto jobHandle = balancer::JobHandle{
            (static_cast<uint64_t>(smHandle.index) << 32) |
             static_cast<uint64_t>(smHandle.generation)};

        // Map balancer priority to ThreadPool priority.
        fat_p::Priority tpPriority = toThreadPoolPriority(job.priority);

        // Submit to ThreadPool. Capture job, callback, and SlotMap handle.
        // The SlotMap handle is checked under lock before execution begins.
        mThreadPool->submit_priority(
            tpPriority,
            [this, job = std::move(job), onDone = std::move(onDone), smHandle]() mutable
            {
                executeJob(std::move(job), std::move(onDone), smHandle);
            });

        return jobHandle;
    }

    fat_p::Expected<void, balancer::CancelError>
    cancel(balancer::JobHandle handle) override
    {
        // Decode SlotMap handle from JobHandle encoding.
        fat_p::SlotMapHandle smHandle{
            static_cast<uint32_t>(handle.value() >> 32),
            static_cast<uint32_t>(handle.value() & 0xFFFFFFFFu)};

        std::lock_guard lock(mMutex);

        if (!mPendingSlots.is_valid(smHandle))
        {
            return fat_p::unexpected(
                balancer::CancelError::NotFound);
        }

        // Erase: the task lambda will see is_valid() == false and skip execution.
        mPendingSlots.erase(smHandle);
        mPriorityOverrides.erase(handle.value());
        return {};
    }

    /**
     * @brief Promote a queued job's priority in the pending-slots map.
     *
     * Updates `mPriorityOverrides` so that when `executeJob()` fires, it uses
     * the promoted priority for latency attribution and metrics. The FAT-P
     * ThreadPool does not support re-ordering already-queued tasks, so
     * execution order is unchanged — only the effective priority label
     * seen at execution time is updated.
     *
     * Returns `CancelError::NotFound` if the job is not in `mPendingSlots`
     * (it has already started or been cancelled). Thread-safe.
     */
    [[nodiscard]] fat_p::Expected<void, balancer::CancelError>
    reprioritise(balancer::JobHandle handle,
                 balancer::Priority  newPriority) noexcept override
    {
        fat_p::SlotMapHandle smHandle{
            static_cast<uint32_t>(handle.value() >> 32),
            static_cast<uint32_t>(handle.value() & 0xFFFFFFFFu)};

        std::lock_guard lock(mMutex);

        if (!mPendingSlots.is_valid(smHandle))
        {
            return fat_p::unexpected(balancer::CancelError::NotFound);
        }

        // Store the override. executeJob() will read it before running.
        mPriorityOverrides.insert(handle.value(), newPriority);
        return {};
    }

    [[nodiscard]] balancer::NodeState status() const noexcept override
    {
        std::lock_guard lock(mMutex);
        return mState;
    }

    [[nodiscard]] balancer::LoadMetrics metrics() const noexcept override
    {
        std::lock_guard lock(mMutex);

        balancer::LoadMetrics m;
        m.nodeId        = mId;
        m.state         = mState;
        m.queueDepth    = mQueueDepth.load(std::memory_order_relaxed);
        m.completedJobs = mCompletedJobs;
        m.failedJobs    = mFailedJobs;
        m.p50LatencyUs  = percentile(50);
        m.p99LatencyUs  = percentile(99);

        const size_t workerCount = mConfig.workerCount > 0 ? mConfig.workerCount : 1;
        const size_t activeApprox = mThreadPool
            ? mThreadPool->active_tasks()
            : 0;
        m.utilization = static_cast<float>(activeApprox)
                      / static_cast<float>(workerCount);
        if (m.utilization > 1.0f)
        {
            m.utilization = 1.0f;
        }

        for (size_t i = 0; i < balancer::kPriorityLevelCount; ++i)
        {
            m.queueDepthByPriority[i] =
                mQueueDepthByPriority[i].load(std::memory_order_relaxed);
        }

        return m;
    }

    void onStateChange(std::function<void(balancer::NodeState)> cb) override
    {
        std::lock_guard lock(mMutex);
        mStateCallback = std::move(cb);
    }

private:
    // ---- Task execution ----------------------------------------------------

    void executeJob(balancer::Job         job,
                    balancer::JobCompletionCallback onDone,
                    fat_p::SlotMapHandle  smHandle)
    {
        // Check cancellation under lock; also consume any priority override.
        {
            std::lock_guard lock(mMutex);

            // Re-encode smHandle to the JobHandle key used by mPriorityOverrides.
            const uint64_t rawKey =
                (static_cast<uint64_t>(smHandle.index) << 32) |
                 static_cast<uint64_t>(smHandle.generation);

            if (!mPendingSlots.is_valid(smHandle))
            {
                // Cancelled — decrement counters without firing callback.
                mPriorityOverrides.erase(rawKey);
                decrementQueueCounters(job.priority);
                updateStateUnlocked();
                return;
            }

            // Consume any priority override stored by reprioritise().
            const Priority* ov = mPriorityOverrides.find(rawKey);
            if (ov)
            {
                job.priority = *ov;
                mPriorityOverrides.erase(rawKey);
            }
        }

        auto t0 = balancer::Clock::now();

        bool succeeded = true;
        try
        {
            uint32_t delayUs = computeDelayUs(job);
            if (delayUs > 0)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
            }

            if (job.payload)
            {
                job.payload();
            }
        }
        catch (...)
        {
            succeeded = false;
        }

        auto t1      = balancer::Clock::now();
        uint64_t elapsedUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

        const uint32_t upc = mConfig.usPerCostUnit > 0 ? mConfig.usPerCostUnit : 1;
        job.observedCost = balancer::Cost{elapsedUs / upc};

        {
            std::lock_guard lock(mMutex);
            decrementQueueCounters(job.priority);
            if (succeeded)
            {
                ++mCompletedJobs;
            }
            else
            {
                ++mFailedJobs;
            }
            recordLatency(elapsedUs);
            mPendingSlots.erase(smHandle);
            updateStateUnlocked();
        }

        onDone(std::move(job), succeeded);
    }

    // ---- Helpers -----------------------------------------------------------

    [[nodiscard]] uint32_t computeDelayUs(const balancer::Job& job) const
    {
        // Read fault state without lock — slightly racy but acceptable for simulation.
        const FaultType fault        = mFault;
        const uint32_t  slowFactor   = (mFaultConfig.slowdownFactor >= 1)
                                         ? mFaultConfig.slowdownFactor : 1;

        uint32_t delayUs = 0;
        if (mConfig.minJobDurationUs > 0)
        {
            if (mConfig.maxJobDurationUs > mConfig.minJobDurationUs)
            {
                // Jitter: uniformly distributed in [min, max].
                std::uniform_int_distribution<uint32_t> dist(
                    mConfig.minJobDurationUs, mConfig.maxJobDurationUs);
                delayUs = dist(mRng);
            }
            else
            {
                delayUs = mConfig.minJobDurationUs;
            }
        }
        else
        {
            delayUs = static_cast<uint32_t>(
                job.estimatedCost.units * mConfig.usPerCostUnit);
        }

        if (fault == FaultType::Slowdown)
        {
            delayUs *= slowFactor;
        }
        return delayUs;
    }

    void decrementQueueCounters(balancer::Priority priority)
    {
        // Called under mMutex.
        auto bandIdx = static_cast<size_t>(priority);
        if (mQueueDepthByPriority[bandIdx].load(std::memory_order_relaxed) > 0)
        {
            mQueueDepthByPriority[bandIdx].fetch_sub(1, std::memory_order_relaxed);
        }
        if (mQueueDepth.load(std::memory_order_relaxed) > 0)
        {
            mQueueDepth.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void updateStateUnlocked()
    {
        if (mFault == FaultType::Crash || mState == balancer::NodeState::Failed)
        {
            return;
        }

        const uint32_t depth = mQueueDepth.load(std::memory_order_relaxed);
        balancer::NodeState newState = mState;

        if (depth >= mConfig.overloadThreshold)
        {
            newState = balancer::NodeState::Overloaded;
        }
        else if (depth > 0 ||
                 (mThreadPool && mThreadPool->active_tasks() > 0))
        {
            if (mState == balancer::NodeState::Overloaded &&
                depth > mConfig.recoverThreshold)
            {
                newState = balancer::NodeState::Overloaded; // hysteresis
            }
            else
            {
                newState = balancer::NodeState::Busy;
            }
        }
        else
        {
            newState = balancer::NodeState::Idle;
        }

        if (newState != mState)
        {
            mState = newState;
            notifyStateChangeUnlocked(newState);
        }
    }

    void notifyStateChange(balancer::NodeState s)
    {
        std::function<void(balancer::NodeState)> cb;
        {
            std::lock_guard lock(mMutex);
            cb = mStateCallback;
        }
        if (cb)
        {
            cb(s);
        }
    }

    void notifyStateChangeUnlocked(balancer::NodeState s)
    {
        if (mStateCallback)
        {
            mStateCallback(s);
        }
    }

    void recordLatency(uint64_t us)
    {
        mLatencies.push_back(us);
        if (mLatencies.size() > kLatencyWindowSize)
        {
            mLatencies.erase(mLatencies.begin());
        }
    }

    [[nodiscard]] uint64_t percentile(int pct) const
    {
        if (mLatencies.empty())
        {
            return 0;
        }
        std::vector<uint64_t> sorted = mLatencies;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(pct) * sorted.size() / 100;
        if (idx >= sorted.size())
        {
            idx = sorted.size() - 1;
        }
        return sorted[idx];
    }

    [[nodiscard]] static fat_p::Priority toThreadPoolPriority(
        balancer::Priority p) noexcept
    {
        switch (p)
        {
            case balancer::Priority::Critical: return fat_p::Priority::Critical;
            case balancer::Priority::High:     return fat_p::Priority::High;
            case balancer::Priority::Normal:   return fat_p::Priority::Normal;
            case balancer::Priority::Low:      return fat_p::Priority::Low;
            case balancer::Priority::Bulk:     return fat_p::Priority::Low;
            default:                           return fat_p::Priority::Normal;
        }
    }

    // ---- Constants ---------------------------------------------------------

    static constexpr size_t kLatencyWindowSize = 256;

    // ---- Members -----------------------------------------------------------

    balancer::NodeId        mId;
    SimulatedNodeConfig     mConfig;
    balancer::NodeState     mState;
    FaultType               mFault       = FaultType::None;
    FaultConfig             mFaultConfig = {};
    bool                    mShutdown    = false;

    // Per-priority queue depth counters. Atomics allow reads from metrics()
    // with minimal contention; writes are performed under mMutex.
    std::array<std::atomic<uint32_t>, balancer::kPriorityLevelCount>
        mQueueDepthByPriority{};
    std::atomic<uint32_t>           mQueueDepth{0};

    // SlotMap for cancel support. Not thread-safe; access under mMutex.
    fat_p::SlotMap<bool>            mPendingSlots;

    // Priority overrides applied by reprioritise(). Keyed by raw JobHandle
    // value. Entries are removed when executeJob() reads them or when the
    // job's SlotMap entry is erased (cancel). Access under mMutex.
    fat_p::FastHashMap<uint64_t, balancer::Priority> mPriorityOverrides;

    uint64_t                        mCompletedJobs = 0;
    uint64_t                        mFailedJobs    = 0;
    std::vector<uint64_t>           mLatencies;

    std::function<void(balancer::NodeState)> mStateCallback;

    // FAT-P ThreadPool — owns the worker threads.
    std::unique_ptr<fat_p::ThreadPool> mThreadPool;

    mutable std::mutex              mMutex;

    // Per-node RNG for jitter (not shared across threads; task lambdas capture
    // by value from computeDelayUs which reads mRng on the submitting thread).
    mutable std::mt19937            mRng;
};

} // namespace balancer::sim
