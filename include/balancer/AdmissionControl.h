#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: AdmissionControl
  file_role: public_header
  path: include/balancer/AdmissionControl.h
  namespace: [balancer, balancer::detail]
  layer: Core
  summary: Three-layer admission gate — global rate limit, per-priority rate limit, cluster capacity; runtime bulk-shed and strict-admission mode overrides.
  api_stability: in_work
  related:
    tests:
      - tests/test_admission.cpp
      - tests/test_AdmissionControl_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file AdmissionControl.h
 * @brief Three-layer admission gate for job submission.
 *
 * Admission is evaluated in order:
 *
 * 1. Global rate limit: total jobs/second across all priorities.
 *    Protects the balancer itself from being overwhelmed.
 *
 * 2. Per-priority rate limit: separate token buckets for Normal, Low, Bulk.
 *    Critical and High are always allowed through.
 *    Two runtime override modes supersede token-bucket checks:
 *    - **Bulk-shed mode** (`setShedBulk(true)`): Bulk-priority jobs are
 *      rejected immediately, bypassing the token bucket. Enabled by
 *      FeatureSupervisor on `admission.bulk_shed`.
 *    - **Strict mode** (`setAdmissionStrict(true)`): all priorities below High
 *      are rejected immediately. Enabled by FeatureSupervisor on
 *      `admission.strict`. Strict mode supersedes bulk-shed mode.
 *
 * 3. Cluster capacity: if all eligible nodes are Overloaded, high-priority
 *    jobs go to a bounded holding queue; lower priority jobs are rejected.
 *
 * The admission mode flags are runtime overrides; they do not alter token
 * bucket rates. Setting bucket rates to zero is not a substitute — the mode
 * flags carry named semantics that the observability API exposes.
 *
 * The internal TokenBucket implementation lives in balancer::detail and is
 * not part of the public API.
 *
 * Thread-safety: AdmissionControl is thread-safe.
 *
 * @see AdmissionControl_Companion_Guide.md for the three-layer design rationale.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "Expected.h"

#include "balancer/BalancerConfig.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"

namespace balancer::detail
{

// ============================================================================
// TokenBucket
// ============================================================================

/**
 * @brief Simple token bucket for rate limiting.
 *
 * Refills at a configurable rate. A capacity of 0 means unlimited.
 * Thread-safe via mutex.
 *
 * @note Internal to AdmissionControl. Not part of the public balancer API.
 */
class TokenBucket
{
public:
    /**
     * @param ratePerSecond  Tokens added per second. 0 = unlimited.
     * @param burstCapacity  Maximum token accumulation. Defaults to ratePerSecond.
     */
    explicit TokenBucket(uint32_t ratePerSecond, uint32_t burstCapacity = 0) noexcept
        : mRate(ratePerSecond)
        , mCapacity(burstCapacity > 0 ? burstCapacity : ratePerSecond)
        , mTokens(burstCapacity > 0 ? static_cast<float>(burstCapacity)
                                    : static_cast<float>(ratePerSecond))
        , mLastRefill(std::chrono::steady_clock::now())
    {}

    /**
     * @brief Attempt to consume one token.
     * @return true if a token was available and consumed; false if rate limited.
     */
    [[nodiscard]] bool tryConsume() noexcept
    {
        if (mRate == 0)
        {
            return true; // 0 = unlimited
        }

        std::lock_guard lock(mMutex);
        refill();
        if (mTokens >= 1.0f)
        {
            mTokens -= 1.0f;
            return true;
        }
        return false;
    }

    /// Current token count (snapshot).
    [[nodiscard]] float tokens() const noexcept
    {
        std::lock_guard lock(mMutex);
        return mTokens;
    }

private:
    void refill() noexcept // caller holds mMutex
    {
        auto  now     = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - mLastRefill).count();
        mTokens       = std::min(static_cast<float>(mCapacity),
                                  mTokens + elapsed * static_cast<float>(mRate));
        mLastRefill   = now;
    }

    uint32_t   mRate;
    uint32_t   mCapacity;
    float      mTokens;
    std::chrono::steady_clock::time_point mLastRefill;
    mutable std::mutex mMutex;
};

} // namespace balancer::detail

namespace balancer
{

// ============================================================================
// AdmissionControl
// ============================================================================

/**
 * @brief Three-layer admission gate.
 *
 * Constructed once and shared by all submission paths via the Balancer.
 */
class AdmissionControl
{
public:
    explicit AdmissionControl(const AdmissionConfig& config)
        : mGlobalBucket(config.globalRateLimitJps)
        , mNormalBucket(config.normalRateLimitJps)
        , mLowBucket(config.lowRateLimitJps)
        , mBulkBucket(config.bulkRateLimitJps)
        , mHoldingQueueCapacity(config.holdingQueueCapacity)
    {}

    // Non-copyable — owns rate limit state and is referenced by Balancer.
    AdmissionControl(const AdmissionControl&)            = delete;
    AdmissionControl& operator=(const AdmissionControl&) = delete;

    // ---- Runtime admission mode overrides ----------------------------------

    /**
     * @brief Enable or disable bulk-shed mode.
     *
     * When enabled, Bulk-priority jobs are rejected at Layer 2 immediately,
     * bypassing the token bucket. Other priorities are unaffected. Intended
     * for FeatureSupervisor to activate on `admission.bulk_shed`.
     *
     * Thread-safe.
     *
     * @param enabled  true to activate bulk-shed; false to deactivate.
     */
    void setShedBulk(bool enabled) noexcept
    {
        mShedBulk.store(enabled, std::memory_order_release);
    }

    /**
     * @brief Enable or disable strict-admission mode.
     *
     * When enabled, any priority below High (i.e. Normal, Low, Bulk) is
     * rejected at Layer 2 immediately. Strict mode supersedes bulk-shed mode.
     * Intended for FeatureSupervisor to activate on `admission.strict`.
     *
     * Thread-safe.
     *
     * @param enabled  true to activate strict mode; false to deactivate.
     */
    void setAdmissionStrict(bool enabled) noexcept
    {
        mAdmissionStrict.store(enabled, std::memory_order_release);
    }

    /// Returns true if bulk-shed mode is currently active.
    [[nodiscard]] bool isShedBulk() const noexcept
    {
        return mShedBulk.load(std::memory_order_acquire);
    }

    /// Returns true if strict-admission mode is currently active.
    [[nodiscard]] bool isAdmissionStrict() const noexcept
    {
        return mAdmissionStrict.load(std::memory_order_acquire);
    }

    /**
     * @brief Evaluate admission for a job at the given priority.
     *
     * @param priority          Job priority.
     * @param clusterSaturated  True if all eligible nodes are Overloaded.
     * @return                  void on admit, SubmitError on reject.
     */
    [[nodiscard]] fat_p::Expected<void, SubmitError>
    evaluate(Priority priority, bool clusterSaturated) noexcept
    {
        // Layer 1: global rate limit
        if (!mGlobalBucket.tryConsume())
        {
            ++mRejections[static_cast<size_t>(SubmitError::RateLimited)];
            return fat_p::unexpected(SubmitError::RateLimited);
        }

        // Layer 2: per-priority rate limit
        // Critical and High bypass per-priority limiting entirely.
        // Strict mode rejects Normal, Low, and Bulk immediately (supersedes bulk-shed).
        // Bulk-shed mode rejects Bulk immediately.
        if (priority == Priority::Normal || priority == Priority::Low ||
            priority == Priority::Bulk)
        {
            if (mAdmissionStrict.load(std::memory_order_acquire))
            {
                ++mRejections[static_cast<size_t>(SubmitError::PriorityRejected)];
                return fat_p::unexpected(SubmitError::PriorityRejected);
            }
            if (priority == Priority::Bulk &&
                mShedBulk.load(std::memory_order_acquire))
            {
                ++mRejections[static_cast<size_t>(SubmitError::PriorityRejected)];
                return fat_p::unexpected(SubmitError::PriorityRejected);
            }
        }

        if (priority == Priority::Normal && !mNormalBucket.tryConsume())
        {
            ++mRejections[static_cast<size_t>(SubmitError::PriorityRejected)];
            return fat_p::unexpected(SubmitError::PriorityRejected);
        }
        if (priority == Priority::Low && !mLowBucket.tryConsume())
        {
            ++mRejections[static_cast<size_t>(SubmitError::PriorityRejected)];
            return fat_p::unexpected(SubmitError::PriorityRejected);
        }
        if (priority == Priority::Bulk && !mBulkBucket.tryConsume())
        {
            ++mRejections[static_cast<size_t>(SubmitError::PriorityRejected)];
            return fat_p::unexpected(SubmitError::PriorityRejected);
        }

        // Layer 3: cluster capacity
        if (clusterSaturated)
        {
            if (priority == Priority::Critical || priority == Priority::High)
            {
                return tryReserveHoldingSlot();
            }
            else
            {
                ++mRejections[static_cast<size_t>(SubmitError::ClusterSaturated)];
                return fat_p::unexpected(SubmitError::ClusterSaturated);
            }
        }

        return {};
    }

    /**
     * @brief Decrement holding queue depth when a job is dispatched from it.
     */
    void releaseFromHoldingQueue() noexcept
    {
        mHoldingQueueDepth.fetch_sub(1, std::memory_order_relaxed);
    }

    /// Current holding queue occupancy.
    [[nodiscard]] uint32_t holdingQueueDepth() const noexcept
    {
        return mHoldingQueueDepth.load(std::memory_order_relaxed);
    }

    /// Rejection count for a given error code (snapshot).
    [[nodiscard]] uint64_t rejectionCount(SubmitError error) const noexcept
    {
        return mRejections[static_cast<size_t>(error)].load(std::memory_order_relaxed);
    }

private:
    [[nodiscard]] fat_p::Expected<void, SubmitError>
    tryReserveHoldingSlot() noexcept
    {
        uint32_t current = mHoldingQueueDepth.load(std::memory_order_relaxed);

        while (true)
        {
            if (current >= mHoldingQueueCapacity)
            {
                ++mRejections[static_cast<size_t>(SubmitError::HoldingQueueFull)];
                return fat_p::unexpected(SubmitError::HoldingQueueFull);
            }

            if (mHoldingQueueDepth.compare_exchange_weak(
                    current,
                    current + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed))
            {
                return {};
            }
        }
    }

    detail::TokenBucket mGlobalBucket;
    detail::TokenBucket mNormalBucket;
    detail::TokenBucket mLowBucket;
    detail::TokenBucket mBulkBucket;

    // Runtime admission mode overrides. Atomic so FeatureSupervisor can toggle
    // them from a different thread without acquiring any balancer-level lock.
    std::atomic<bool> mShedBulk{false};
    std::atomic<bool> mAdmissionStrict{false};

    uint32_t              mHoldingQueueCapacity;
    std::atomic<uint32_t> mHoldingQueueDepth{0};

    // Indexed by SubmitError value. 6 possible error codes.
    std::array<std::atomic<uint64_t>, 6> mRejections{};
};

} // namespace balancer
