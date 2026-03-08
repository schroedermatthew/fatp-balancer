#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: AgingEngine
  file_role: public_header
  path: include/balancer/AgingEngine.h
  namespace: balancer
  layer: Core
  summary: Priority aging engine — promotes waiting jobs through priority bands and fires deadline expiry events.
  api_stability: in_work
  related:
    docs_search: "AgingEngine"
    tests:
      - tests/test_aging.cpp
      - tests/test_AgingEngine_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file AgingEngine.h
 * @brief Priority aging and deadline expiry for waiting jobs.
 *
 * Jobs waiting in queues accumulate wait time. Left unaddressed, low-priority
 * jobs can starve indefinitely behind a steady stream of higher-priority work.
 * AgingEngine prevents starvation by promoting jobs through priority bands when
 * they have waited beyond the configured threshold for their current priority.
 *
 * Aging model:
 * - Bulk → Low after bulkToLow seconds
 * - Low → Normal after lowToNormal seconds
 * - Normal → High after normalToHigh seconds
 * - High → Critical after highToCritical seconds (if highToCriticalEnabled)
 *
 * Ceiling: if highToCriticalEnabled is false (the default), High is the highest
 * priority aging can reach. Jobs at the ceiling do not generate further events.
 *
 * Deadline expiry: if a job has a deadline and Clock::now() >= deadline,
 * AgingEngine fires an ExpiredEvent. The caller is responsible for removing
 * the job from the queue and optifying the submitter.
 *
 * Design — explicit time injection:
 * AgingEngine::tick() takes an explicit TimePoint rather than calling
 * Clock::now() internally. This makes the engine fully deterministic in tests:
 * callers advance time by constructing TimePoints from a known base and passing
 * them to tick(). Background scheduling loops pass Clock::now() directly.
 *
 * Thread-safety: AgingEngine is NOT thread-safe. Callers must ensure that
 * track(), untrack(), and tick() are called from the same thread (typically
 * a dedicated aging-engine thread or single-threaded test code).
 *
 * @see AgingConfig in BalancerConfig.h for threshold configuration.
 */

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "balancer/BalancerConfig.h"
#include "balancer/Job.h"

namespace balancer
{

// ============================================================================
// AgingEngine
// ============================================================================

/**
 * @brief Priority aging and deadline expiry engine.
 *
 * Invariant: every JobId tracked by AgingEngine corresponds to a job that
 * has been submitted and has not yet completed or been cancelled. Callers
 * must call untrack() on completion or cancellation to maintain this invariant.
 *
 * @note Not thread-safe. See file-level documentation.
 */
class AgingEngine
{
public:
    // ---- Event types -------------------------------------------------------

    /**
     * @brief Fired when a job ages from one priority band to the next.
     *
     * The caller is responsible for locating the job in its queue and
     * moving it to the higher-priority queue.
     */
    struct AgedEvent
    {
        JobId    id;           ///< Job that aged.
        Priority oldPriority;  ///< Priority before aging.
        Priority newPriority;  ///< Priority after aging (always higher than old).
    };

    /**
     * @brief Fired when a job's deadline has passed.
     *
     * The caller is responsible for removing the job from the queue and
     * notifying the submitter. AgingEngine stops tracking the job automatically
     * after firing this event.
     */
    struct ExpiredEvent
    {
        JobId    id;           ///< Job that expired.
        Priority priority;     ///< Priority at expiry time.
    };

    /**
     * @brief Aggregate result of a tick() call.
     */
    struct TickResult
    {
        std::vector<AgedEvent>    aged;    ///< Jobs promoted this tick. May be empty.
        std::vector<ExpiredEvent> expired; ///< Jobs that missed their deadline. May be empty.
    };

    // ---- Construction ------------------------------------------------------

    /**
     * @brief Construct with aging configuration.
     * @param config  Aging thresholds and ceiling settings.
     */
    explicit AgingEngine(AgingConfig config = {}) noexcept
        : mConfig(std::move(config))
    {}

    // Non-copyable, non-movable — tracked state and callbacks are identity-bound.
    AgingEngine(const AgingEngine&)            = delete;
    AgingEngine& operator=(const AgingEngine&) = delete;
    AgingEngine(AgingEngine&&)                 = delete;
    AgingEngine& operator=(AgingEngine&&)      = delete;

    // ---- Tracking ----------------------------------------------------------

    /**
     * @brief Begin tracking a job for priority aging and deadline expiry.
     *
     * The engine records the job's id, current priority, deadline, and the
     * current time as the start of the waiting period. Aging thresholds are
     * measured from the moment track() is called, not from job.submitted.
     *
     * Calling track() for a JobId that is already tracked is a no-op; the
     * existing entry is preserved.
     *
     * @param job  Job to track. id, priority, and optionally deadline must be set.
     * @param now  Current time. Waiting period starts from this point.
     */
    void track(const Job& job, TimePoint now) noexcept
    {
        if (mTracked.count(job.id.value()))
        {
            return; // already tracked
        }
        mTracked.emplace(job.id.value(), TrackedJob{
            job.id,
            job.priority,
            job.deadline,
            now
        });
    }

    /**
     * @brief Stop tracking a job.
     *
     * Must be called when a job completes, is cancelled, or its ExpiredEvent
     * has been processed. Safe to call for unknown JobIds (no-op).
     *
     * @param id  Job to stop tracking.
     */
    void untrack(JobId id) noexcept
    {
        mTracked.erase(id.value());
    }

    /**
     * @brief Process aging and expiry up to `now`.
     *
     * For each tracked job:
     * - If deadline is set and now >= deadline: fires ExpiredEvent and removes
     *   the job from tracking. Expired jobs do NOT also fire AgedEvent.
     * - If the job has waited at its current priority beyond the configured
     *   threshold, and the ceiling has not been reached: ages the job to the
     *   next higher priority, fires AgedEvent, resets the waiting clock.
     *
     * Jobs at the ceiling (High when highToCriticalEnabled=false, or Critical)
     * are not aged further and do not generate spurious AgedEvents.
     *
     * @param now  Current time. Must be monotonically non-decreasing across calls.
     * @return     All events that fired during this tick.
     */
    [[nodiscard]] TickResult tick(TimePoint now) noexcept
    {
        TickResult result;

        std::vector<uint32_t> toExpire;

        for (auto& [key, entry] : mTracked)
        {
            // Deadline expiry takes priority over aging.
            if (entry.deadline.has_value() && now >= *entry.deadline)
            {
                result.expired.push_back({entry.id, entry.currentPriority});
                toExpire.push_back(key);
                continue;
            }

            // Check aging threshold for current priority.
            auto threshold = agingThreshold(entry.currentPriority);
            if (!threshold.has_value())
            {
                continue; // at ceiling or at Critical — no further aging
            }

            auto waited = now - entry.lastBumpAt;
            if (waited >= *threshold)
            {
                Priority newPriority = promotedPriority(entry.currentPriority);
                result.aged.push_back({entry.id, entry.currentPriority, newPriority});
                entry.currentPriority = newPriority;
                entry.lastBumpAt      = now;
            }
        }

        for (uint32_t key : toExpire)
        {
            mTracked.erase(key);
        }

        return result;
    }

    // ---- Queries -----------------------------------------------------------

    /**
     * @brief Returns true if the given job is currently tracked.
     * @param id  Job to query.
     * @return    true if tracked, false otherwise.
     */
    [[nodiscard]] bool isTracked(JobId id) const noexcept
    {
        return mTracked.count(id.value()) > 0;
    }

    /**
     * @brief Number of jobs currently tracked.
     * @return Tracked job count.
     */
    [[nodiscard]] size_t trackedCount() const noexcept
    {
        return mTracked.size();
    }

    /**
     * @brief Current tracked priority for a job.
     *
     * @param id  Job to query.
     * @return    Tracked priority, or Priority::Normal if the job is not tracked.
     */
    [[nodiscard]] Priority currentPriority(JobId id) const noexcept
    {
        auto it = mTracked.find(id.value());
        return it != mTracked.end() ? it->second.currentPriority : Priority::Normal;
    }

    // ---- Config ------------------------------------------------------------

    [[nodiscard]] const AgingConfig& config() const noexcept { return mConfig; }

private:
    struct TrackedJob
    {
        JobId                    id;
        Priority                 currentPriority;
        std::optional<TimePoint> deadline;
        TimePoint                lastBumpAt;   ///< When aging clock last reset.
    };

    // Returns the threshold Duration before a job at this priority ages to the
    // next band, or nullopt if the job is at or above the configured ceiling.
    [[nodiscard]] std::optional<Duration> agingThreshold(Priority p) const noexcept
    {
        switch (p)
        {
        case Priority::Bulk:
            return mConfig.bulkToLow;

        case Priority::Low:
            return mConfig.lowToNormal;

        case Priority::Normal:
            return mConfig.normalToHigh;

        case Priority::High:
            if (mConfig.highToCriticalEnabled)
            {
                return mConfig.highToCritical;
            }
            return std::nullopt; // ceiling: High stays at High

        case Priority::Critical:
            return std::nullopt; // no aging above Critical
        }
        return std::nullopt;
    }

    // Returns the next higher priority (lower enum value). Assumes p is not Critical.
    [[nodiscard]] static Priority promotedPriority(Priority p) noexcept
    {
        return static_cast<Priority>(static_cast<uint8_t>(p) - 1);
    }

    AgingConfig mConfig;

    // JobId uint32_t value → tracking state.
    std::unordered_map<uint32_t, TrackedJob> mTracked;
};

} // namespace balancer
