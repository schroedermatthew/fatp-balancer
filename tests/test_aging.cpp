/**
 * @file test_aging.cpp
 * @brief Comprehensive unit tests for AgingEngine.h
 */

/*
BALANCER_META:
  meta_version: 1
  component: AgingEngine
  file_role: test
  path: tests/test_aging.cpp
  namespace: balancer::testing::agingns
  layer: Testing
  summary: Unit tests for AgingEngine — priority aging, ceiling enforcement, deadline expiry, cancel interaction.
  api_stability: in_work
  related:
    headers:
      - include/balancer/AgingEngine.h
*/

#include <chrono>
#include <iostream>

#include "balancer/AgingEngine.h"
#include "FatPTest.h"

using fat_p::testing::TestRunner;
namespace colors = fat_p::testing::colors;

namespace balancer::testing::agingns
{

// ============================================================================
// Helpers
// ============================================================================

// Base point for all deterministic time operations in this suite.
inline const balancer::TimePoint kEpoch = balancer::Clock::now();

inline balancer::TimePoint advance(std::chrono::seconds s)
{
    return kEpoch + s;
}

inline balancer::Job makeJob(uint32_t id, balancer::Priority priority)
{
    balancer::Job j;
    j.id       = balancer::JobId{id};
    j.priority = priority;
    j.submitted = kEpoch;
    return j;
}

inline balancer::AgingConfig fastConfig()
{
    balancer::AgingConfig cfg;
    cfg.bulkToLow             = std::chrono::seconds{10};
    cfg.lowToNormal           = std::chrono::seconds{10};
    cfg.normalToHigh          = std::chrono::seconds{10};
    cfg.highToCriticalEnabled = false;
    cfg.highToCritical        = std::chrono::seconds{10};
    return cfg;
}

// ============================================================================
// Construction and basic tracking
// ============================================================================

FATP_TEST_CASE(initially_empty)
{
    balancer::AgingEngine engine(fastConfig());
    FATP_ASSERT_EQ(engine.trackedCount(), size_t(0),
                   "New engine must track no jobs");
    return true;
}

FATP_TEST_CASE(track_adds_job)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(1, balancer::Priority::Normal);
    engine.track(job, kEpoch);
    FATP_ASSERT_TRUE(engine.isTracked(job.id), "Job must be tracked after track()");
    FATP_ASSERT_EQ(engine.trackedCount(), size_t(1), "Tracked count must be 1");
    return true;
}

FATP_TEST_CASE(track_is_idempotent)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(1, balancer::Priority::Normal);
    engine.track(job, kEpoch);
    engine.track(job, advance(std::chrono::seconds{5}));
    FATP_ASSERT_EQ(engine.trackedCount(), size_t(1),
                   "Track must be idempotent — second call must not add a duplicate");
    return true;
}

FATP_TEST_CASE(untrack_removes_job)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(2, balancer::Priority::Low);
    engine.track(job, kEpoch);
    engine.untrack(job.id);
    FATP_ASSERT_FALSE(engine.isTracked(job.id), "Job must not be tracked after untrack()");
    FATP_ASSERT_EQ(engine.trackedCount(), size_t(0), "Tracked count must drop to 0");
    return true;
}

FATP_TEST_CASE(untrack_unknown_id_is_safe)
{
    balancer::AgingEngine engine(fastConfig());
    engine.untrack(balancer::JobId{999});
    FATP_ASSERT_EQ(engine.trackedCount(), size_t(0),
                   "Untrack of unknown id must not affect tracked count");
    return true;
}

FATP_TEST_CASE(current_priority_returns_initial_priority)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(3, balancer::Priority::Bulk);
    engine.track(job, kEpoch);
    FATP_ASSERT_EQ(engine.currentPriority(job.id), balancer::Priority::Bulk,
                   "currentPriority must return initial priority before aging");
    return true;
}

FATP_TEST_CASE(current_priority_default_for_untracked)
{
    balancer::AgingEngine engine(fastConfig());
    balancer::Priority p = engine.currentPriority(balancer::JobId{77});
    FATP_ASSERT_EQ(p, balancer::Priority::Normal,
                   "currentPriority must return Normal for untracked jobs");
    return true;
}

// ============================================================================
// Tick with no elapsed time
// ============================================================================

FATP_TEST_CASE(tick_before_threshold_produces_no_events)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(4, balancer::Priority::Bulk);
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{5}));
    FATP_ASSERT_TRUE(result.aged.empty(), "No AgedEvent before aging threshold");
    FATP_ASSERT_TRUE(result.expired.empty(), "No ExpiredEvent when no deadline set");
    return true;
}

// ============================================================================
// Priority aging — each band
// ============================================================================

FATP_TEST_CASE(bulk_ages_to_low_after_threshold)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(5, balancer::Priority::Bulk);
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{10}));
    FATP_ASSERT_EQ(result.aged.size(), size_t(1),  "One AgedEvent for Bulk -> Low");
    FATP_ASSERT_EQ(result.aged[0].oldPriority, balancer::Priority::Bulk,  "Old must be Bulk");
    FATP_ASSERT_EQ(result.aged[0].newPriority, balancer::Priority::Low,   "New must be Low");
    FATP_ASSERT_EQ(result.aged[0].id, job.id, "AgedEvent id must match job");
    FATP_ASSERT_EQ(engine.currentPriority(job.id), balancer::Priority::Low,
                   "Tracked priority must update to Low");
    return true;
}

FATP_TEST_CASE(low_ages_to_normal_after_threshold)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(6, balancer::Priority::Low);
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{10}));
    FATP_ASSERT_EQ(result.aged.size(), size_t(1), "One AgedEvent for Low -> Normal");
    FATP_ASSERT_EQ(result.aged[0].oldPriority, balancer::Priority::Low,    "Old must be Low");
    FATP_ASSERT_EQ(result.aged[0].newPriority, balancer::Priority::Normal, "New must be Normal");
    return true;
}

FATP_TEST_CASE(normal_ages_to_high_after_threshold)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(7, balancer::Priority::Normal);
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{10}));
    FATP_ASSERT_EQ(result.aged.size(), size_t(1), "One AgedEvent for Normal -> High");
    FATP_ASSERT_EQ(result.aged[0].oldPriority, balancer::Priority::Normal, "Old must be Normal");
    FATP_ASSERT_EQ(result.aged[0].newPriority, balancer::Priority::High,   "New must be High");
    return true;
}

FATP_TEST_CASE(bulk_ages_through_two_bands)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(12, balancer::Priority::Bulk);
    engine.track(job, kEpoch);

    auto result1 = engine.tick(advance(std::chrono::seconds{10}));
    FATP_ASSERT_EQ(result1.aged.size(), size_t(1), "First band: Bulk -> Low");
    FATP_ASSERT_EQ(result1.aged[0].newPriority, balancer::Priority::Low, "Must be Low");

    auto result2 = engine.tick(advance(std::chrono::seconds{20}));
    FATP_ASSERT_EQ(result2.aged.size(), size_t(1), "Second band: Low -> Normal");
    FATP_ASSERT_EQ(result2.aged[0].newPriority, balancer::Priority::Normal, "Must be Normal");
    FATP_ASSERT_EQ(engine.currentPriority(job.id), balancer::Priority::Normal,
                   "Tracked priority must be Normal after two bands");
    return true;
}

// ============================================================================
// Ceiling enforcement
// ============================================================================

FATP_TEST_CASE(ceiling_respected_high_does_not_age_to_critical_by_default)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(8, balancer::Priority::High);
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{60}));
    FATP_ASSERT_TRUE(result.aged.empty(),
                     "High job must not age to Critical when highToCriticalEnabled=false");
    FATP_ASSERT_EQ(engine.currentPriority(job.id), balancer::Priority::High,
                   "Priority must remain High at ceiling");
    return true;
}

FATP_TEST_CASE(high_ages_to_critical_when_enabled)
{
    balancer::AgingConfig cfg = fastConfig();
    cfg.highToCriticalEnabled = true;
    cfg.highToCritical        = std::chrono::seconds{10};
    balancer::AgingEngine engine(cfg);
    auto job = makeJob(9, balancer::Priority::High);
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{10}));
    FATP_ASSERT_EQ(result.aged.size(), size_t(1), "One AgedEvent for High -> Critical");
    FATP_ASSERT_EQ(result.aged[0].newPriority, balancer::Priority::Critical,
                   "New priority must be Critical");
    return true;
}

FATP_TEST_CASE(critical_never_ages_further)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(10, balancer::Priority::Critical);
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{9999}));
    FATP_ASSERT_TRUE(result.aged.empty(), "Critical jobs must never produce AgedEvents");
    return true;
}

FATP_TEST_CASE(jobs_at_ceiling_do_not_generate_spurious_events)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(11, balancer::Priority::Normal);
    engine.track(job, kEpoch);

    engine.tick(advance(std::chrono::seconds{10})); // Normal -> High
    FATP_ASSERT_EQ(engine.currentPriority(job.id), balancer::Priority::High,
                   "Job should have aged to High");

    auto result2 = engine.tick(advance(std::chrono::seconds{100}));
    FATP_ASSERT_TRUE(result2.aged.empty(),
                     "No further AgedEvents after job reaches the ceiling");
    return true;
}

// ============================================================================
// Deadline expiry
// ============================================================================

FATP_TEST_CASE(deadline_expiry_fires_expired_event)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(13, balancer::Priority::Normal);
    job.deadline = kEpoch + std::chrono::seconds{5};
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{5}));
    FATP_ASSERT_EQ(result.expired.size(), size_t(1), "One ExpiredEvent when deadline passed");
    FATP_ASSERT_EQ(result.expired[0].id, job.id, "ExpiredEvent id must match job");
    FATP_ASSERT_EQ(result.expired[0].priority, balancer::Priority::Normal,
                   "ExpiredEvent must record priority at expiry");
    return true;
}

FATP_TEST_CASE(expired_job_removed_from_tracking)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(14, balancer::Priority::Low);
    job.deadline = kEpoch + std::chrono::seconds{5};
    engine.track(job, kEpoch);
    engine.tick(advance(std::chrono::seconds{5}));
    FATP_ASSERT_FALSE(engine.isTracked(job.id),
                      "Job must be removed from tracking after expiry");
    return true;
}

FATP_TEST_CASE(deadline_expiry_takes_priority_over_aging)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(15, balancer::Priority::Bulk);
    job.deadline = kEpoch + std::chrono::seconds{5};
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{5}));
    FATP_ASSERT_EQ(result.expired.size(), size_t(1), "ExpiredEvent must fire at deadline");
    FATP_ASSERT_TRUE(result.aged.empty(),
                     "No AgedEvent when job expires before aging threshold");
    return true;
}

FATP_TEST_CASE(no_expiry_before_deadline)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(16, balancer::Priority::Normal);
    job.deadline = kEpoch + std::chrono::seconds{20};
    engine.track(job, kEpoch);
    auto result = engine.tick(advance(std::chrono::seconds{10}));
    FATP_ASSERT_TRUE(result.expired.empty(), "No ExpiredEvent before the deadline");
    return true;
}

// ============================================================================
// Cancel interaction
// ============================================================================

FATP_TEST_CASE(aged_job_not_re_queued_after_cancel)
{
    balancer::AgingEngine engine(fastConfig());
    auto job = makeJob(17, balancer::Priority::Bulk);
    engine.track(job, kEpoch);
    engine.tick(advance(std::chrono::seconds{10})); // Bulk -> Low
    FATP_ASSERT_EQ(engine.currentPriority(job.id), balancer::Priority::Low,
                   "Job must have aged to Low");
    engine.untrack(job.id);
    auto result = engine.tick(advance(std::chrono::seconds{30}));
    FATP_ASSERT_TRUE(result.aged.empty(), "Cancelled job must not generate AgedEvents");
    FATP_ASSERT_TRUE(result.expired.empty(), "Cancelled job must not generate ExpiredEvents");
    return true;
}

// ============================================================================
// Multiple jobs
// ============================================================================

FATP_TEST_CASE(multiple_jobs_age_independently)
{
    balancer::AgingEngine engine(fastConfig());
    auto jobA = makeJob(20, balancer::Priority::Bulk);
    auto jobB = makeJob(21, balancer::Priority::Normal);
    auto jobC = makeJob(22, balancer::Priority::High);
    engine.track(jobA, kEpoch);
    engine.track(jobB, kEpoch);
    engine.track(jobC, kEpoch);

    auto result = engine.tick(advance(std::chrono::seconds{10}));

    FATP_ASSERT_EQ(result.aged.size(), size_t(2),
                   "Two jobs should age — Bulk->Low and Normal->High");
    FATP_ASSERT_TRUE(result.expired.empty(), "No expirations");
    FATP_ASSERT_EQ(engine.currentPriority(jobA.id), balancer::Priority::Low,
                   "Job A must be Low");
    FATP_ASSERT_EQ(engine.currentPriority(jobB.id), balancer::Priority::High,
                   "Job B must be High");
    FATP_ASSERT_EQ(engine.currentPriority(jobC.id), balancer::Priority::High,
                   "Job C must remain High (ceiling)");
    return true;
}

FATP_TEST_CASE(tracked_count_consistent_across_operations)
{
    balancer::AgingEngine engine(fastConfig());
    auto j1 = makeJob(30, balancer::Priority::Normal);
    auto j2 = makeJob(31, balancer::Priority::Low);
    auto j3 = makeJob(32, balancer::Priority::Bulk);
    j3.deadline = kEpoch + std::chrono::seconds{5};

    engine.track(j1, kEpoch);
    engine.track(j2, kEpoch);
    engine.track(j3, kEpoch);
    FATP_ASSERT_EQ(engine.trackedCount(), size_t(3), "Three jobs tracked");

    engine.tick(advance(std::chrono::seconds{5})); // j3 expires
    FATP_ASSERT_EQ(engine.trackedCount(), size_t(2), "Two after j3 expires");

    engine.untrack(j1.id);
    FATP_ASSERT_EQ(engine.trackedCount(), size_t(1), "One after j1 cancelled");
    FATP_ASSERT_TRUE(engine.isTracked(j2.id), "j2 must still be tracked");
    return true;
}

} // namespace balancer::testing::agingns

// ============================================================================
// Public interface
// ============================================================================

namespace balancer::testing
{

bool test_aging()
{
    FATP_PRINT_HEADER(AGING ENGINE)

    TestRunner runner;

    auto& out = *fat_p::testing::get_test_config().output;

    out << colors::blue() << "--- Construction and Tracking ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, agingns, initially_empty);
    FATP_RUN_TEST_NS(runner, agingns, track_adds_job);
    FATP_RUN_TEST_NS(runner, agingns, track_is_idempotent);
    FATP_RUN_TEST_NS(runner, agingns, untrack_removes_job);
    FATP_RUN_TEST_NS(runner, agingns, untrack_unknown_id_is_safe);
    FATP_RUN_TEST_NS(runner, agingns, current_priority_returns_initial_priority);
    FATP_RUN_TEST_NS(runner, agingns, current_priority_default_for_untracked);

    out << "\n" << colors::blue() << "--- Tick: No Events ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, agingns, tick_before_threshold_produces_no_events);

    out << "\n" << colors::blue() << "--- Priority Aging ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, agingns, bulk_ages_to_low_after_threshold);
    FATP_RUN_TEST_NS(runner, agingns, low_ages_to_normal_after_threshold);
    FATP_RUN_TEST_NS(runner, agingns, normal_ages_to_high_after_threshold);
    FATP_RUN_TEST_NS(runner, agingns, bulk_ages_through_two_bands);

    out << "\n" << colors::blue() << "--- Ceiling Enforcement ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, agingns, ceiling_respected_high_does_not_age_to_critical_by_default);
    FATP_RUN_TEST_NS(runner, agingns, high_ages_to_critical_when_enabled);
    FATP_RUN_TEST_NS(runner, agingns, critical_never_ages_further);
    FATP_RUN_TEST_NS(runner, agingns, jobs_at_ceiling_do_not_generate_spurious_events);

    out << "\n" << colors::blue() << "--- Deadline Expiry ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, agingns, deadline_expiry_fires_expired_event);
    FATP_RUN_TEST_NS(runner, agingns, expired_job_removed_from_tracking);
    FATP_RUN_TEST_NS(runner, agingns, deadline_expiry_takes_priority_over_aging);
    FATP_RUN_TEST_NS(runner, agingns, no_expiry_before_deadline);

    out << "\n" << colors::blue() << "--- Cancel Interaction ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, agingns, aged_job_not_re_queued_after_cancel);

    out << "\n" << colors::blue() << "--- Multiple Jobs ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, agingns, multiple_jobs_age_independently);
    FATP_RUN_TEST_NS(runner, agingns, tracked_count_consistent_across_operations);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_aging() ? 0 : 1;
}
#endif
