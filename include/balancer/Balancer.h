#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: Balancer
  file_role: public_header
  path: include/balancer/Balancer.h
  namespace: balancer
  layer: Core
  summary: >\n    Core cluster load balancer — coordinates admission, routing, learning,\n    and optional priority aging. Phase 8: DEBT-001, DEBT-002, DEBT-006.\n    Phase 9: DEBT-005 (HoldingQueue — Critical/High overflow jobs are stored\n    and retried by a background drain loop instead of being silently dropped).
  api_stability: in_work
  related:
    docs_search: "Balancer"
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
 * @file Balancer.h
 * @brief Core cluster load balancer.
 *
 * The Balancer is the central coordinator. It:
 * 1. Evaluates admission (AdmissionControl)
 * 2. Builds a ClusterView snapshot
 * 3. Calls the active policy to select a node
 * 4. Submits the job to the selected INode
 * 5. On completion, updates the CostModel
 *
 * The Balancer holds non-owning references to INode objects. Node lifetimes
 * must outlive the Balancer.
 *
 * Thread-safety: submit() and cancel() are thread-safe. switchPolicy() is
 * thread-safe and performs a drain-swap-resume: it blocks new submissions,
 * waits for all in-flight jobs to complete, swaps the policy atomically, and
 * then re-enables submissions. This ensures no job is routed by two policies.
 *
 * Priority aging (Phase 7):
 * AgingEngine integration is opt-in via setAgingEnabled(true). When enabled:
 * - Every accepted job is tracked in the AgingEngine from the moment it is
 *   dispatched to a node.
 * - A background thread calls AgingEngine::tick() at mConfig.aging.scanInterval.
 * - ExpiredEvent: the job is cancelled via cancel().
 * - AgedEvent: priority promotion is observed but cannot be forwarded to the
 *   node's internal queue — INode does not expose a reprioritise() API. This
 *   is a documented architectural gap; the event is not silently discarded.
 *   Future INode extension should add reprioritise() and wire it here.
 * - AgingEngine is not thread-safe; all track/untrack/tick calls are
 *   serialised under mAgingMutex.
 *
 * Phase 4 changes:
 * - cancel() uses a FastHashMap<uint64_t, NodeId> for O(1) job-to-node lookup.
 * - switchPolicy() performs drain-swap-resume instead of an unsafe lock-and-swap.
 * - mDraining flag gates submit() during policy switches.
 *
 * Phase 7 changes:
 * - setAgingEnabled(bool): starts/stops the background AgingEngine tick thread.
 * - setDegradationEnabled(bool): delegates to CostModel to toggle the
 *   DegradationCurve correction in predict().
 * - admissionControl(): exposes AdmissionControl for FeatureSupervisor to
 *   call setShedBulk() and setAdmissionStrict() directly.
 * - mJobIdToHandleMap: secondary map for expired-job cancellation by JobId.
 *
 * Phase 8 changes (DEBT resolution):
 * - DEBT-001: switchPolicy() race fixed. mSwitchMutex serialises concurrent
 *   switchPolicy() calls. mSubmittedCount is incremented before the mDraining
 *   check and decremented on early-out, so no submit can slip between a
 *   zero-count observation and the policy swap.
 * - DEBT-002: buildClusterMetrics() now populates totalCompleted, totalRejected,
 *   rejectionsByCode, meanP50LatencyUs, maxP99LatencyUs, and
 *   throughputPerSecond from live per-node snapshots and AdmissionControl.
 * - DEBT-006: agingLoop() forwards AgedEvent promotions to INode::reprioritise().
 *   The architectural gap (INode lacked reprioritise()) is resolved in Phase 8.
 *
 * Phase 9 changes (DEBT-005 resolution):
 * - mHoldingQueue: a HoldingQueue instance stores Critical/High overflow jobs
 *   when all nodes are saturated. Jobs are assigned a holding handle (bit 63
 *   set) and returned to the caller as a valid JobHandle.
 * - holdingDrainLoop(): background thread wakes on job-completion events and
 *   on a configurable fallback interval (HoldingQueueConfig::drainInterval).
 *   It retries held jobs in Critical-before-High, FIFO order.
 * - cancel() routes holding-handle cancels to mHoldingQueue.cancel().
 * - Jobs exceeding HoldingQueueConfig::maxRetries (0 = unlimited) are dropped
 *   with succeeded=false on their completion callbacks.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Expected.h"
#include "FastHashMap.h"

#include "balancer/AdmissionControl.h"
#include "balancer/AgingEngine.h"
#include "balancer/HoldingQueue.h"
#include "balancer/BalancerConfig.h"
#include "balancer/ClusterView.h"
#include "balancer/CostModel.h"
#include "balancer/INode.h"
#include "balancer/ISchedulingPolicy.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"

namespace balancer
{

/**
 * @brief Core cluster load balancer.
 *
 * Construction:
 * @code
 * std::vector<INode*> nodes = cluster.nodes();
 * auto policy = std::make_unique<LeastLoaded>();
 * Balancer balancer(nodes, std::move(policy), config);
 * @endcode
 *
 * Submit:
 * @code
 * Job job;
 * job.priority      = Priority::Normal;
 * job.estimatedCost = Cost{100};
 * job.payload       = []{ do_work(); };
 *
 * auto result = balancer.submit(std::move(job));
 * if (result.has_value()) { auto handle = result.value(); }
 * else                   { handle_error(result.error()); }
 * @endcode
 *
 * Policy switch (drain-swap-resume):
 * @code
 * balancer.switchPolicy(std::make_unique<WorkStealing>());
 * // Blocks until all in-flight jobs complete, then swaps atomically.
 * @endcode
 */
class Balancer
{
public:
    /**
     * @brief Construct balancer with a node set, active policy, and config.
     *
     * @param nodes    Non-owning pointers to nodes. Must outlive the Balancer.
     * @param policy   Active scheduling policy. Balancer takes ownership.
     * @param config   Balancer configuration.
     */
    Balancer(std::vector<INode*>                nodes,
             std::unique_ptr<ISchedulingPolicy>  policy,
             BalancerConfig                      config = {})
        : mNodes(std::move(nodes))
        , mPolicy(std::move(policy))
        , mConfig(std::move(config))
        , mAdmission(mConfig.admission)
        , mCostModel(mConfig.costModel)
        , mAgingEngine(mConfig.aging)
    {
        for (auto* node : mNodes)
        {
            node->onStateChange([this](NodeState) { /* triggers metrics refresh */ });
        }
        startHoldingDrainThread();
    }

    /**
     * @brief Destructor. Stops the AgingEngine tick thread if running.
     */
    ~Balancer()
    {
        stopAgingThread();
        stopHoldingDrainThread();
    }

    // Non-copyable — owns unique policy and references node lifetimes.
    Balancer(const Balancer&)            = delete;
    Balancer& operator=(const Balancer&) = delete;
    Balancer(Balancer&&)                 = delete;
    Balancer& operator=(Balancer&&)      = delete;

    // ---- Submission --------------------------------------------------------

    /**
     * @brief Submit a job for execution on the best available node.
     *
     * Evaluates admission, selects a node, and dispatches the job. Returns
     * ClusterSaturated immediately if a policy switch is draining in-flight jobs.
     *
     * Thread-safe.
     *
     * @param job  Job to submit. Balancer fills id and submitted on accept.
     * @return     JobHandle on success, SubmitError on rejection.
     */
    [[nodiscard]] fat_p::Expected<JobHandle, SubmitError>
    submit(Job job)
    {
        // DEBT-001 fix: increment before the draining gate so that once
        // switchPolicy() observes mSubmittedCount == 0, no submit thread
        // can be between the gate check and the subsequent increment.
        // If we are draining we decrement and return ClusterSaturated.
        mSubmittedCount.fetch_add(1, std::memory_order_relaxed);

        // Reject during drain-swap-resume to prevent routing by the old policy.
        if (mDraining.load(std::memory_order_acquire))
        {
            mSubmittedCount.fetch_sub(1, std::memory_order_release);
            return fat_p::unexpected(SubmitError::ClusterSaturated);
        }

        // Pre-flight: deadline check.
        if (job.deadline.has_value() && Clock::now() >= *job.deadline)
        {
            mSubmittedCount.fetch_sub(1, std::memory_order_release);
            return fat_p::unexpected(SubmitError::DeadlineUnachievable);
        }

        auto metrics       = snapshotMetrics();
        bool allOverloaded = areAllEligibleOverloaded(metrics, job.priority);

        auto admitResult = mAdmission.evaluate(job.priority, allOverloaded);
        if (!admitResult.has_value())
        {
            mSubmittedCount.fetch_sub(1, std::memory_order_release);
            return fat_p::unexpected(admitResult.error());
        }

        // DEBT-005: if all eligible nodes are overloaded and the priority is
        // Critical/High, AdmissionControl has already reserved a holding slot.
        // Enqueue in mHoldingQueue rather than dispatching immediately.
        if (allOverloaded &&
            (job.priority == Priority::Critical ||
             job.priority == Priority::High))
        {
            job.submitted = Clock::now();
            job.id        = nextJobId();

            auto heldJob  = job;
            auto holdCb   = makeHoldingTerminalCallback(heldJob);

            auto hResult = mHoldingQueue.enqueue(std::move(heldJob), std::move(holdCb));
            if (!hResult.has_value())
            {
                // enqueue failed despite AdmissionControl reserving a slot —
                // size mismatch; release the AC slot and propagate.
                mAdmission.releaseFromHoldingQueue();
                mSubmittedCount.fetch_sub(1, std::memory_order_release);
                return fat_p::unexpected(SubmitError::HoldingQueueFull);
            }

            // mSubmittedCount stays incremented — the job is logically in flight
            // until it is dispatched+completed or definitively dropped.
            mTotalSubmitted.fetch_add(1, std::memory_order_relaxed);
            notifyHoldingDrainer();
            return hResult.value();
        }

        ClusterMetrics clusterMetrics = buildClusterMetrics(metrics);
        ClusterView view(std::move(metrics), std::move(clusterMetrics), mCostModel);

        INode* targetNode = nullptr;
        {
            std::lock_guard policyLock(mPolicyMutex);
            auto selectResult = mPolicy->selectNode(job, view);
            if (!selectResult.has_value())
            {
                mSubmittedCount.fetch_sub(1, std::memory_order_release);
                return fat_p::unexpected(SubmitError::ClusterSaturated);
            }
            targetNode = findNode(selectResult.value());
        }

        if (!targetNode)
        {
            mSubmittedCount.fetch_sub(1, std::memory_order_release);
            return fat_p::unexpected(SubmitError::NodeNotFound);
        }

        job.submitted = Clock::now();
        job.id        = nextJobId();

        // Snapshot a copy for map registration and aging before move.
        const Job    jobSnapshot   = job;
        auto regState     = std::make_shared<DispatchRegistrationState>();
        auto completionCb = makeDispatchCompletionCallback(jobSnapshot, regState);
        auto nodeResult   = targetNode->submit(std::move(job), std::move(completionCb));
        if (!nodeResult.has_value())
        {
            mSubmittedCount.fetch_sub(1, std::memory_order_release);
            return fat_p::unexpected(SubmitError::ClusterSaturated);
        }

        const JobHandle handle = nodeResult.value();
        mTotalSubmitted.fetch_add(1, std::memory_order_relaxed);
        registerDispatchedJob(jobSnapshot, targetNode->id(), handle, regState);
        return handle;
    }

    /**
     * @brief Cancel a queued job. O(1) via job-to-node map.
     *
     * Thread-safe.
     *
     * @param handle  Handle returned by submit().
     * @return        void on success, CancelError on failure.
     */
    fat_p::Expected<void, CancelError>
    cancel(JobHandle handle)
    {
        // DEBT-005: holding handles (bit 63 set) route to the HoldingQueue.
        if (isHoldingHandle(handle.value()))
        {
            auto removed = mHoldingQueue.remove(handle.value());
            if (!removed.has_value())
            {
                return fat_p::unexpected(CancelError::NotFound);
            }
            mAdmission.releaseFromHoldingQueue();
            removed->onDone(removed->job, /*succeeded=*/false);
            return {};
        }

        auto routing = lookupRouting(handle);
        if (!routing.has_value())
        {
            return fat_p::unexpected(CancelError::NotFound);
        }

        INode* node = findNode(routing->nodeId);
        if (!node)
        {
            return fat_p::unexpected(CancelError::NotFound);
        }

        auto cancelResult = node->cancel(handle);
        if (!cancelResult.has_value())
        {
            return cancelResult;
        }

        Job cancelledJob{};
        cancelledJob.id = routing->jobId;
        finalizeJobLifecycle(cancelledJob, handle, /*succeeded=*/false);
        return {};
    }

    // ---- Policy switching --------------------------------------------------

    /**
     * @brief Replace the active scheduling policy using drain-swap-resume.
     *
     * Thread-safe. Procedure:
     * 1. Set mDraining — new submit() calls return ClusterSaturated.
     * 2. Spin until mSubmittedCount reaches zero (all in-flight jobs complete).
     * 3. Swap the policy under mPolicyMutex.
     * 4. Clear mDraining — submissions resume under the new policy.
     *
     * @param policy  New policy. Balancer takes ownership.
     */
    void switchPolicy(std::unique_ptr<ISchedulingPolicy> policy)
    {
        // DEBT-001 fix: mSwitchMutex serialises concurrent switchPolicy()
        // calls. Without this, two concurrent callers can both observe a
        // zero mSubmittedCount and proceed to swap simultaneously.
        std::lock_guard switchLock(mSwitchMutex);

        // Phase 1: block new submissions.
        mDraining.store(true, std::memory_order_release);

        // Phase 2: wait for all in-flight jobs to complete.
        // Submit threads that incremented mSubmittedCount before seeing
        // mDraining will eventually decrement it; new submits decrement
        // immediately and return ClusterSaturated (increment-before-drain).
        while (mSubmittedCount.load(std::memory_order_acquire) > 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }

        // Phase 3: swap policy atomically.
        {
            std::lock_guard policyLock(mPolicyMutex);
            mPolicy = std::move(policy);
        }

        // Phase 4: resume submissions.
        mDraining.store(false, std::memory_order_release);
    }

    // ---- Runtime toggles (Phase 7) -----------------------------------------

    /**
     * @brief Enable or disable the AgingEngine priority-aging subsystem.
     *
     * When enabled, a background thread calls AgingEngine::tick() at
     * BalancerConfig::aging::scanInterval. Two event types are processed:
     *
     * - **ExpiredEvent**: deadline passed — job is cancelled via cancel().
     *   A concurrent completion is benign (CancelError::NotFound is silently
     *   ignored in the aging loop).
     * - **AgedEvent**: job waited beyond its priority threshold. The event is
     *   received but cannot be acted upon — INode does not expose reprioritise().
     *   This is a documented architectural gap; the node's internal queue order
     *   is unchanged. Future INode extension should add reprioritise() and wire
     *   the AgedEvent handler here.
     *
     * Disabling stops the background thread immediately (interruptible sleep).
     * Jobs already dispatched continue to be untracked when their completion
     * callbacks fire; the AgingEngine will not observe them on subsequent ticks.
     *
     * setAgingEnabled(true) is idempotent if already running.
     * setAgingEnabled(false) is idempotent if already stopped.
     *
     * Thread-safe.
     *
     * @param enabled  true to start aging; false to stop it.
     */
    void setAgingEnabled(bool enabled)
    {
        if (enabled)
        {
            startAgingThread();
        }
        else
        {
            stopAgingThread();
        }
    }

    /**
     * @brief Enable or disable the DegradationCurve correction in predict().
     *
     * Delegates to the owned CostModel. When disabled, the depth-aware
     * degradation multiplier is not applied; existing bucket observations are
     * preserved and the curve resumes from its learned state on re-enable.
     *
     * Intended for FeatureSupervisor to activate on `feature.degradation_curve`.
     *
     * Thread-safe (atomic store inside CostModel).
     *
     * @param enabled  true to apply depth-aware correction; false to suppress it.
     */
    void setDegradationEnabled(bool enabled) noexcept
    {
        mCostModel.setDegradationEnabled(enabled);
    }

    // ---- Observability -----------------------------------------------------

    /// Returns true if the AgingEngine tick thread is currently running.
    [[nodiscard]] bool isAgingEnabled() const noexcept
    {
        return mAgingEnabled.load(std::memory_order_acquire);
    }

    /// Returns true if the DegradationCurve correction is currently applied.
    [[nodiscard]] bool isDegradationEnabled() const noexcept
    {
        return mCostModel.isDegradationEnabled();
    }

    /// Read-only access to the shared cost model.
    [[nodiscard]] const CostModel& costModel() const noexcept { return mCostModel; }

    /// Read-only access to the configuration (e.g. for PolicyFactory).
    [[nodiscard]] const BalancerConfig& config() const noexcept { return mConfig; }

    /// Jobs currently in flight (submitted but not yet completed).
    [[nodiscard]] uint32_t inFlightCount() const noexcept
    {
        return mSubmittedCount.load(std::memory_order_relaxed);
    }

    /// Total jobs submitted since construction.
    [[nodiscard]] uint64_t totalSubmitted() const noexcept
    {
        return mTotalSubmitted.load(std::memory_order_relaxed);
    }

    /// Current policy name.
    [[nodiscard]] std::string_view policyName() const noexcept
    {
        std::lock_guard lock(mPolicyMutex);
        return mPolicy ? mPolicy->name() : "<none>";
    }

    /// True while a policy switch is draining in-flight jobs.
    [[nodiscard]] bool isDraining() const noexcept
    {
        return mDraining.load(std::memory_order_acquire);
    }

    /**
     * @brief Non-owning access to the AdmissionControl instance.
     *
     * Exposed for FeatureSupervisor to call setShedBulk() and
     * setAdmissionStrict() in response to FeatureManager events.
     */
    [[nodiscard]] AdmissionControl& admissionControl() noexcept { return mAdmission; }
    [[nodiscard]] const AdmissionControl& admissionControl() const noexcept { return mAdmission; }

    /**
     * @brief Build and return a current ClusterMetrics snapshot.
     *
     * Aggregates per-node `LoadMetrics` into a `ClusterMetrics` value that
     * includes active/unavailable node counts, throughput, p50/p99 latency,
     * total completed/rejected counters, and holding-queue depth. The values
     * are consistent within a single snapshot but are not atomically coherent
     * across nodes — they may lag by one observation window.
     */
    [[nodiscard]] ClusterMetrics clusterMetrics() const
    {
        const std::vector<LoadMetrics> metrics = snapshotMetrics();
        return buildClusterMetrics(metrics);
    }

private:
    // ---- Aging thread management -------------------------------------------

    // ---- Routing and lifecycle helpers -------------------------------------

    struct JobRoutingInfo
    {
        JobId  jobId;
        NodeId nodeId;
    };

    struct DispatchRegistrationState
    {
        std::mutex               mutex;
        bool                     published = false;
        std::optional<JobHandle> handle;
        std::optional<Job>       earlyCompletedJob;
        bool                     earlySucceeded = false;
    };

    [[nodiscard]] std::optional<JobRoutingInfo>
    lookupRouting(JobHandle handle) const
    {
        std::lock_guard mapLock(mJobMapMutex);
        const JobRoutingInfo* info = mHandleToJobInfo.find(handle.value());
        if (!info)
        {
            return std::nullopt;
        }
        return *info;
    }

    void eraseRoutingState(JobHandle handle)
    {
        std::lock_guard mapLock(mJobMapMutex);
        const JobRoutingInfo* info = mHandleToJobInfo.find(handle.value());
        if (!info)
        {
            return;
        }
        mJobIdToHandleMap.erase(info->jobId.value());
        mHandleToJobInfo.erase(handle.value());
    }

    void finalizeJobLifecycle(const Job&               finishedJob,
                              std::optional<JobHandle> routedHandle,
                              bool                     succeeded)
    {
        if (succeeded)
        {
            mCostModel.update(finishedJob);
            mTotalCompleted.fetch_add(1, std::memory_order_relaxed);
        }

        {
            std::lock_guard agingLock(mAgingMutex);
            mAgingEngine.untrack(finishedJob.id);
        }

        if (routedHandle.has_value())
        {
            eraseRoutingState(*routedHandle);
        }

        mSubmittedCount.fetch_sub(1, std::memory_order_release);
        notifyHoldingDrainer();
    }

    [[nodiscard]] JobCompletionCallback
    makeHoldingTerminalCallback(const Job& capturedJobRef)
    {
        Job capturedJob = capturedJobRef;
        return [this, capturedJob](Job completed, bool succeeded) mutable
        {
            capturedJob.observedCost = completed.observedCost;
            capturedJob.executedBy   = completed.executedBy;
            finalizeJobLifecycle(capturedJob, std::nullopt, succeeded);
        };
    }

    [[nodiscard]] JobCompletionCallback
    makeDispatchCompletionCallback(
        const Job& capturedJobRef,
        const std::shared_ptr<DispatchRegistrationState>& regState)
    {
        Job capturedJob = capturedJobRef;
        return [this, capturedJob, regState](Job completed, bool succeeded) mutable
        {
            capturedJob.observedCost = completed.observedCost;
            capturedJob.executedBy   = completed.executedBy;

            std::optional<JobHandle> routedHandle;
            {
                std::lock_guard stateLock(regState->mutex);
                if (!regState->published)
                {
                    regState->earlyCompletedJob = capturedJob;
                    regState->earlySucceeded    = succeeded;
                    return;
                }
                routedHandle = regState->handle;
            }

            finalizeJobLifecycle(capturedJob, routedHandle, succeeded);
        };
    }

    void registerDispatchedJob(
        const Job&                                         jobSnapshot,
        NodeId                                             nodeId,
        JobHandle                                          handle,
        const std::shared_ptr<DispatchRegistrationState>& regState)
    {
        {
            std::lock_guard mapLock(mJobMapMutex);
            mHandleToJobInfo.insert(handle.value(),
                                    JobRoutingInfo{jobSnapshot.id, nodeId});
            mJobIdToHandleMap.insert(jobSnapshot.id.value(), handle.value());
        }

        if (mAgingEnabled.load(std::memory_order_acquire))
        {
            std::lock_guard agingLock(mAgingMutex);
            mAgingEngine.track(jobSnapshot, Clock::now());
        }

        std::optional<Job> earlyJob;
        bool               earlySucceeded = false;
        {
            std::lock_guard stateLock(regState->mutex);
            regState->handle    = handle;
            regState->published = true;
            if (regState->earlyCompletedJob.has_value())
            {
                earlyJob       = std::move(regState->earlyCompletedJob);
                earlySucceeded = regState->earlySucceeded;
            }
        }

        if (earlyJob.has_value())
        {
            finalizeJobLifecycle(*earlyJob, handle, earlySucceeded);
        }
    }

    // ---- Holding queue drain management ------------------------------------

    void startHoldingDrainThread()
    {
        if (mHoldingDrainRunning.exchange(true, std::memory_order_acq_rel))
            return;
        mHoldingDrainStop.store(false, std::memory_order_release);
        mHoldingDrainThread = std::thread([this] { holdingDrainLoop(); });
    }

    void stopHoldingDrainThread()
    {
        if (!mHoldingDrainRunning.exchange(false, std::memory_order_acq_rel))
            return;
        mHoldingDrainStop.store(true, std::memory_order_release);
        mHoldingDrainCv.notify_all();
        if (mHoldingDrainThread.joinable())
            mHoldingDrainThread.join();
    }

    void notifyHoldingDrainer()
    {
        mHoldingDrainCv.notify_one();
    }

    /**
     * @brief Background loop: drain held jobs into the cluster when capacity
     *        frees up.
     *
     * Wakes on:
     * - Completion events (via notifyHoldingDrainer() from the onDone lambda).
     * - Configurable fallback timeout (HoldingQueueConfig::drainInterval).
     *
     * On each wake, attempts to dispatch all queued held jobs in
     * Critical-before-High, FIFO order. Jobs that exceed maxRetries
     * (when > 0) are dropped with succeeded=false.
     */
    void holdingDrainLoop()
    {
        const auto interval  = mConfig.holdingQueue.drainInterval;
        const uint32_t maxR  = mConfig.holdingQueue.maxRetries;

        while (!mHoldingDrainStop.load(std::memory_order_acquire))
        {
            {
                std::unique_lock cvLock(mHoldingDrainCvMutex);
                mHoldingDrainCv.wait_for(cvLock, interval,
                    [this] {
                        return mHoldingDrainStop.load(std::memory_order_acquire)
                            || !mHoldingQueue.empty();
                    });
            }

            if (mHoldingDrainStop.load(std::memory_order_acquire))
                break;

            // Drain: dequeue unconditionally so expired/over-retried jobs are
            // always swept regardless of cluster saturation state.
            while (!mHoldingQueue.empty())
            {
                auto entryOpt = mHoldingQueue.tryDequeue();
                if (!entryOpt.has_value())
                    break;

                auto& entry = *entryOpt;

                // Drop if deadline expired — unconditional, even while saturated.
                if (entry.job.deadline.has_value() &&
                    Clock::now() >= *entry.job.deadline)
                {
                    mAdmission.releaseFromHoldingQueue();
                    entry.onDone(entry.job, /*succeeded=*/false);
                    continue;
                }

                // Drop if maxRetries exceeded — unconditional.
                if (maxR > 0 && entry.job.holdRetries >= maxR)
                {
                    mAdmission.releaseFromHoldingQueue();
                    entry.onDone(entry.job, /*succeeded=*/false);
                    continue;
                }

                // Check cluster capacity for dispatch.
                auto metricsNow = snapshotMetrics();
                const bool stillSaturated =
                    areAllEligibleOverloaded(metricsNow, entry.job.priority);

                if (stillSaturated)
                {
                    // Re-queue in-place. The admission reservation is still held.
                    (void)mHoldingQueue.requeue(std::move(entry));
                    break;
                }

                // Try to dispatch.
                ClusterMetrics cm = buildClusterMetrics(metricsNow);
                ClusterView view(std::move(metricsNow), std::move(cm), mCostModel);

                INode* targetNode = nullptr;
                {
                    std::lock_guard policyLock(mPolicyMutex);
                    auto sel = mPolicy->selectNode(entry.job, view);
                    if (!sel.has_value())
                    {
                        // No eligible node — re-queue and stop this cycle.
                        entry.job.holdRetries++;
                        (void)mHoldingQueue.requeue(std::move(entry));
                        break;
                    }
                    targetNode = findNode(sel.value());
                }

                if (!targetNode)
                {
                    entry.job.holdRetries++;
                    (void)mHoldingQueue.requeue(std::move(entry));
                    break;
                }

                entry.job.submitted = Clock::now();

                const Job   drainSnap = entry.job;
                auto regState         = std::make_shared<DispatchRegistrationState>();
                auto dispatchCb       = makeDispatchCompletionCallback(drainSnap, regState);

                auto nodeResult = targetNode->submit(
                    std::move(entry.job), std::move(dispatchCb));

                if (!nodeResult.has_value())
                {
                    // Node refused — keep the reservation and requeue the same entry.
                    entry.job = drainSnap;
                    entry.job.holdRetries++;
                    (void)mHoldingQueue.requeue(std::move(entry));
                    break;
                }

                // Successfully dispatched — release holding slot and register.
                const JobHandle nodeHandle = nodeResult.value();
                mAdmission.releaseFromHoldingQueue();
                registerDispatchedJob(drainSnap, targetNode->id(), nodeHandle, regState);
            }
        }
    }

    // ---- Aging thread management -------------------------------------------

    void startAgingThread()
    {
        // Idempotent: no-op if already running.
        if (mAgingEnabled.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        mAgingThreadStop.store(false, std::memory_order_release);
        mAgingThread = std::thread([this] { agingLoop(); });
    }

    void stopAgingThread()
    {
        // Idempotent: no-op if not running.
        if (!mAgingEnabled.exchange(false, std::memory_order_acq_rel))
        {
            return;
        }

        mAgingThreadStop.store(true, std::memory_order_release);
        mAgingCv.notify_all();

        if (mAgingThread.joinable())
        {
            mAgingThread.join();
        }
    }

    /**
     * @brief Background loop: tick AgingEngine and process expired jobs.
     *
     * Sleeps for aging.scanInterval between ticks. Wakes immediately when
     * mAgingThreadStop is set by stopAgingThread().
     */
    void agingLoop()
    {
        const auto interval = mConfig.aging.scanInterval;

        while (!mAgingThreadStop.load(std::memory_order_acquire))
        {
            // Interruptible sleep: wakes early on stopAgingThread().
            {
                std::unique_lock cvLock(mAgingCvMutex);
                mAgingCv.wait_for(cvLock, interval,
                    [this] { return mAgingThreadStop.load(std::memory_order_acquire); });
            }

            if (mAgingThreadStop.load(std::memory_order_acquire))
            {
                break;
            }

            AgingEngine::TickResult result;
            {
                std::lock_guard agingLock(mAgingMutex);
                result = mAgingEngine.tick(Clock::now());
            }

            // DEBT-006: forward AgedEvent promotions to INode::reprioritise().
            // A race with concurrent completion is benign — reprioritise()
            // returns CancelError::NotFound when the job has already started.
            for (const auto& ev : result.aged)
            {
                uint64_t rawHandle = 0;
                NodeId   nodeId{0};
                {
                    std::lock_guard mapLock(mJobMapMutex);
                    const uint64_t* hFound =
                        mJobIdToHandleMap.find(ev.id.value());
                    if (!hFound) { continue; }
                    rawHandle = *hFound;
                    const JobRoutingInfo* info = mHandleToJobInfo.find(rawHandle);
                    if (!info) { continue; }
                    nodeId = info->nodeId;
                }
                INode* node = findNode(nodeId);
                if (node)
                {
                    (void)node->reprioritise(
                        JobHandle{rawHandle}, ev.newPriority);
                }
            }

            // ExpiredEvent: cancel the job. A concurrent completion that wins
            // the race returns CancelError::NotFound; this is benign.
            for (const auto& ev : result.expired)
            {
                uint64_t rawHandle = 0;
                {
                    std::lock_guard mapLock(mJobMapMutex);
                    const uint64_t* found = mJobIdToHandleMap.find(ev.id.value());
                    if (!found) { continue; }
                    rawHandle = *found;
                }
                (void)cancel(JobHandle{rawHandle});
            }
        }
    }

    // ---- Helpers -----------------------------------------------------------

    [[nodiscard]] std::vector<LoadMetrics> snapshotMetrics() const
    {
        std::vector<LoadMetrics> result;
        result.reserve(mNodes.size());
        for (const auto* node : mNodes)
        {
            result.push_back(node->metrics());
        }
        return result;
    }

    [[nodiscard]] ClusterMetrics buildClusterMetrics(
        const std::vector<LoadMetrics>& nodeMetrics) const
    {
        ClusterMetrics cm;

        // Node state counters + latency aggregation from the snapshot passed
        // in by submit(). Using the already-captured snapshot avoids a second
        // round of node->status() calls in the same critical path.
        uint64_t p50Sum       = 0;
        uint32_t p50Count     = 0;
        uint64_t p99Max       = 0;

        for (const auto& m : nodeMetrics)
        {
            if (m.state == NodeState::Idle || m.state == NodeState::Busy)
            {
                ++cm.activeNodes;
                if (m.completedJobs > 0)
                {
                    p50Sum += m.p50LatencyUs;
                    ++p50Count;
                }
            }
            else if (m.state == NodeState::Failed ||
                     m.state == NodeState::Offline)
            {
                ++cm.unavailableNodes;
            }

            if (m.state == NodeState::Overloaded)
            {
                ++cm.overloadedNodes;
            }

            if (m.p99LatencyUs > p99Max)
            {
                p99Max = m.p99LatencyUs;
            }
        }

        // knownNodes counts only nodes in stable health states: active, unavailable,
        // or overloaded. Transitional states (Recovering, Initializing, Draining) are
        // excluded so that a recovering node does not inflate the denominator and
        // cause false overload alerts. This matches the old activeNodes+unavailableNodes
        // denominator semantics while adding overloaded to prevent a zero denominator
        // when all nodes are overloaded.
        cm.knownNodes = cm.activeNodes + cm.unavailableNodes + cm.overloadedNodes;

        cm.meanP50LatencyUs = (p50Count > 0) ? (p50Sum / p50Count) : 0;
        cm.maxP99LatencyUs  = p99Max;

        cm.totalSubmitted    = mTotalSubmitted.load(std::memory_order_relaxed);
        cm.totalCompleted    = mTotalCompleted.load(std::memory_order_relaxed);
        cm.holdingQueueDepth = mAdmission.holdingQueueDepth();

        // Rejection counts — sum all error-code buckets.
        for (size_t i = 0; i < cm.rejectionsByCode.size(); ++i)
        {
            uint64_t rc = mAdmission.rejectionCount(
                static_cast<SubmitError>(i));
            cm.rejectionsByCode[i]  = rc;
            cm.totalRejected       += rc;
        }

        // Throughput — rolling 1-second window using last-sample cache.
        {
            std::lock_guard tpLock(mThroughputMutex);
            auto now            = Clock::now();
            float elapsedSec    = std::chrono::duration<float>(
                                      now - mThroughputSampleTime).count();

            if (elapsedSec >= 0.5f)
            {
                uint64_t completed   = mTotalCompleted.load(
                                           std::memory_order_relaxed);
                uint64_t delta       = completed - mThroughputSampleCompleted;
                mCachedThroughput    = (elapsedSec > 0.0f)
                                       ? static_cast<float>(delta) / elapsedSec
                                       : 0.0f;
                mThroughputSampleCompleted = completed;
                mThroughputSampleTime      = now;
            }
            cm.throughputPerSecond = mCachedThroughput;
        }

        return cm;
    }

    [[nodiscard]] bool areAllEligibleOverloaded(
        const std::vector<LoadMetrics>& metrics,
        Priority priority) const noexcept
    {
        bool anyEligible = false;
        for (const auto& m : metrics)
        {
            if (nodeAcceptsPriority(m.state, priority))
            {
                anyEligible = true;
                if (m.state != NodeState::Overloaded)
                {
                    return false;
                }
            }
        }
        return anyEligible;
    }

    [[nodiscard]] INode* findNode(NodeId id) const noexcept
    {
        for (auto* node : mNodes)
        {
            if (node->id() == id)
            {
                return node;
            }
        }
        return nullptr;
    }

    [[nodiscard]] JobId nextJobId() noexcept
    {
        return JobId{mNextJobId.fetch_add(1, std::memory_order_relaxed)};
    }

    // ---- Members -----------------------------------------------------------

    std::vector<INode*>                mNodes;
    std::unique_ptr<ISchedulingPolicy> mPolicy;
    mutable std::mutex                 mPolicyMutex;
    BalancerConfig                     mConfig;
    AdmissionControl                   mAdmission;
    CostModel                          mCostModel;

    // Primary job routing map: JobHandle (uint64_t) → JobRoutingInfo.
    fat_p::FastHashMap<uint64_t, JobRoutingInfo>   mHandleToJobInfo;
    // Secondary map: JobId (uint32_t) → JobHandle (uint64_t) for AgingEngine
    // expired-job cancellation, where only JobId is available.
    fat_p::FastHashMap<uint32_t, uint64_t> mJobIdToHandleMap;
    mutable std::mutex                     mJobMapMutex;

    std::atomic<bool>     mDraining{false};
    std::atomic<uint32_t> mSubmittedCount{0};
    std::atomic<uint64_t> mTotalSubmitted{0};
    std::atomic<uint64_t> mTotalCompleted{0};  ///< Successful completions (DEBT-002).
    std::atomic<uint32_t> mNextJobId{1};

    /// DEBT-001: serialises concurrent switchPolicy() calls.
    std::mutex            mSwitchMutex;

    /// DEBT-002: throughput rolling-window state. Mutable — updated in the
    /// const buildClusterMetrics() path via a mutex-protected cache.
    mutable std::mutex    mThroughputMutex;
    mutable uint64_t      mThroughputSampleCompleted{0};
    mutable TimePoint     mThroughputSampleTime{Clock::now()};
    mutable float         mCachedThroughput{0.0f};

    // ---- HoldingQueue subsystem (DEBT-005) ---------------------------------
    //
    // mHoldingQueue stores Critical/High overflow jobs. The drain thread
    // retries them when completion events free up cluster capacity.

    HoldingQueue       mHoldingQueue{mConfig.admission.holdingQueueCapacity};
    std::thread        mHoldingDrainThread;
    std::atomic<bool>  mHoldingDrainRunning{false};
    std::atomic<bool>  mHoldingDrainStop{false};
    std::condition_variable mHoldingDrainCv;
    std::mutex              mHoldingDrainCvMutex;

    // ---- AgingEngine subsystem ---------------------------------------------
    //
    // AgingEngine is NOT thread-safe. All calls to track(), untrack(), and
    // tick() are serialised under mAgingMutex regardless of calling thread.

    AgingEngine        mAgingEngine;
    mutable std::mutex mAgingMutex;
    std::atomic<bool>  mAgingEnabled{false};

    std::thread             mAgingThread;
    std::atomic<bool>       mAgingThreadStop{false};
    std::condition_variable mAgingCv;
    std::mutex              mAgingCvMutex;
};

} // namespace balancer
