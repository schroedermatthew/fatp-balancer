/**
 * @file test_admission.cpp
 * @brief Unit tests for AdmissionControl.h
 */

/*
BALANCER_META:
  meta_version: 1
  component: AdmissionControl
  file_role: test
  path: tests/test_admission.cpp
  namespace: balancer::testing::admission
  layer: Testing
  summary: Unit tests for AdmissionControl — all three admission layers, every error code.
  api_stability: in_work
  related:
    headers:
      - include/balancer/AdmissionControl.h
*/

#include <iostream>
#include <thread>
#include <vector>

#include "balancer/AdmissionControl.h"
#include "FatPTest.h"

namespace balancer::testing::admission
{

// ============================================================================
// Helpers
// ============================================================================

/// Build a config with an explicit global rate limit and no other limits.
inline AdmissionConfig globalRateConfig(uint32_t jps)
{
    AdmissionConfig cfg;
    cfg.globalRateLimitJps = jps;
    return cfg;
}

/// Build a config with explicit per-priority limits and no global limit.
inline AdmissionConfig priorityRateConfig(uint32_t normal, uint32_t low, uint32_t bulk)
{
    AdmissionConfig cfg;
    cfg.normalRateLimitJps = normal;
    cfg.lowRateLimitJps    = low;
    cfg.bulkRateLimitJps   = bulk;
    return cfg;
}

/// Build a config with a holding queue capacity.
inline AdmissionConfig holdingConfig(uint32_t capacity)
{
    AdmissionConfig cfg;
    cfg.holdingQueueCapacity = capacity;
    return cfg;
}

// ============================================================================
// Unlimited pass-through
// ============================================================================

FATP_TEST_CASE(unlimited_admits_all_priorities)
{
    // All limits at 0 = unlimited.
    AdmissionControl ac(AdmissionConfig{});

    for (auto p : {Priority::Critical, Priority::High,
                   Priority::Normal,   Priority::Low, Priority::Bulk})
    {
        auto result = ac.evaluate(p, /*clusterSaturated=*/false);
        FATP_ASSERT_TRUE(result.has_value(),
            "Unlimited config must admit every priority");
    }
    return true;
}

// ============================================================================
// Layer 1 — global rate limit
// ============================================================================

FATP_TEST_CASE(global_rate_limit_triggers_RateLimited)
{
    // 1 token: the first call succeeds, second fails.
    AdmissionControl ac(globalRateConfig(1));

    auto first = ac.evaluate(Priority::Normal, false);
    FATP_ASSERT_TRUE(first.has_value(), "First call must succeed with 1 token");

    auto second = ac.evaluate(Priority::Normal, false);
    FATP_ASSERT_FALSE(second.has_value(), "Second call must fail — token exhausted");
    FATP_ASSERT_EQ(static_cast<int>(second.error()),
                   static_cast<int>(SubmitError::RateLimited),
                   "Error must be RateLimited");

    FATP_ASSERT_EQ(ac.rejectionCount(SubmitError::RateLimited), uint64_t(1),
                   "Rejection counter must increment");
    return true;
}

FATP_TEST_CASE(global_rate_limit_applies_to_all_priorities)
{
    AdmissionControl ac(globalRateConfig(1));

    // Consume the single token with Critical.
    auto first = ac.evaluate(Priority::Critical, false);
    FATP_ASSERT_TRUE(first.has_value(), "First must succeed");

    // Even Critical is blocked when global rate is exhausted.
    auto second = ac.evaluate(Priority::Critical, false);
    FATP_ASSERT_FALSE(second.has_value(), "Critical must also be rate-limited globally");
    FATP_ASSERT_EQ(static_cast<int>(second.error()),
                   static_cast<int>(SubmitError::RateLimited),
                   "Error must be RateLimited even for Critical");
    return true;
}

// ============================================================================
// Layer 2 — per-priority rate limits
// ============================================================================

FATP_TEST_CASE(normal_per_priority_limit_triggers_PriorityRejected)
{
    AdmissionControl ac(priorityRateConfig(/*normal=*/1, 0, 0));

    auto first = ac.evaluate(Priority::Normal, false);
    FATP_ASSERT_TRUE(first.has_value(), "First Normal must succeed");

    auto second = ac.evaluate(Priority::Normal, false);
    FATP_ASSERT_FALSE(second.has_value(), "Second Normal must fail");
    FATP_ASSERT_EQ(static_cast<int>(second.error()),
                   static_cast<int>(SubmitError::PriorityRejected),
                   "Error must be PriorityRejected for Normal throttle");
    return true;
}

FATP_TEST_CASE(low_per_priority_limit_triggers_PriorityRejected)
{
    AdmissionControl ac(priorityRateConfig(0, /*low=*/1, 0));

    auto first = ac.evaluate(Priority::Low, false);
    FATP_ASSERT_TRUE(first.has_value(), "First Low must succeed");

    auto second = ac.evaluate(Priority::Low, false);
    FATP_ASSERT_FALSE(second.has_value(), "Second Low must fail");
    FATP_ASSERT_EQ(static_cast<int>(second.error()),
                   static_cast<int>(SubmitError::PriorityRejected),
                   "Error must be PriorityRejected for Low throttle");
    return true;
}

FATP_TEST_CASE(bulk_per_priority_limit_triggers_PriorityRejected)
{
    AdmissionControl ac(priorityRateConfig(0, 0, /*bulk=*/1));

    auto first = ac.evaluate(Priority::Bulk, false);
    FATP_ASSERT_TRUE(first.has_value(), "First Bulk must succeed");

    auto second = ac.evaluate(Priority::Bulk, false);
    FATP_ASSERT_FALSE(second.has_value(), "Second Bulk must fail");
    FATP_ASSERT_EQ(static_cast<int>(second.error()),
                   static_cast<int>(SubmitError::PriorityRejected),
                   "Error must be PriorityRejected for Bulk throttle");
    return true;
}

FATP_TEST_CASE(critical_bypasses_per_priority_limits)
{
    // Set very tight Normal/Low/Bulk limits — Critical must not be affected.
    AdmissionControl ac(priorityRateConfig(1, 1, 1));

    // Drain per-priority buckets with the affected priorities.
    (void)ac.evaluate(Priority::Normal, false);
    (void)ac.evaluate(Priority::Low,    false);
    (void)ac.evaluate(Priority::Bulk,   false);

    // Critical and High must still pass — they have no per-priority bucket.
    auto cr = ac.evaluate(Priority::Critical, false);
    FATP_ASSERT_TRUE(cr.has_value(), "Critical must bypass per-priority limits");

    auto hi = ac.evaluate(Priority::High, false);
    FATP_ASSERT_TRUE(hi.has_value(), "High must bypass per-priority limits");
    return true;
}

// ============================================================================
// Layer 3 — cluster capacity
// ============================================================================

FATP_TEST_CASE(cluster_saturated_rejects_normal_and_below)
{
    AdmissionConfig cfg;
    cfg.holdingQueueCapacity = 0; // no holding queue
    AdmissionControl ac(cfg);

    for (auto p : {Priority::Normal, Priority::Low, Priority::Bulk})
    {
        auto result = ac.evaluate(p, /*clusterSaturated=*/true);
        FATP_ASSERT_FALSE(result.has_value(), "Saturated cluster must reject Normal and below");
        FATP_ASSERT_EQ(static_cast<int>(result.error()),
                       static_cast<int>(SubmitError::ClusterSaturated),
                       "Error must be ClusterSaturated");
    }
    return true;
}

FATP_TEST_CASE(cluster_saturated_critical_goes_to_holding_queue)
{
    AdmissionControl ac(holdingConfig(/*capacity=*/10));

    auto result = ac.evaluate(Priority::Critical, /*clusterSaturated=*/true);
    FATP_ASSERT_TRUE(result.has_value(),
        "Critical must be admitted to holding queue when cluster saturated");
    FATP_ASSERT_EQ(ac.holdingQueueDepth(), uint32_t(1),
        "Holding queue depth must increment");
    return true;
}

FATP_TEST_CASE(cluster_saturated_high_goes_to_holding_queue)
{
    AdmissionControl ac(holdingConfig(10));

    auto result = ac.evaluate(Priority::High, true);
    FATP_ASSERT_TRUE(result.has_value(), "High must go to holding queue when saturated");
    FATP_ASSERT_EQ(ac.holdingQueueDepth(), uint32_t(1), "Depth must be 1");
    return true;
}

FATP_TEST_CASE(holding_queue_full_produces_HoldingQueueFull)
{
    AdmissionControl ac(holdingConfig(/*capacity=*/2));

    // Fill holding queue to capacity.
    FATP_ASSERT_TRUE(ac.evaluate(Priority::Critical, true).has_value(), "First in");
    FATP_ASSERT_TRUE(ac.evaluate(Priority::Critical, true).has_value(), "Second in");

    // Next must fail with HoldingQueueFull.
    auto result = ac.evaluate(Priority::Critical, true);
    FATP_ASSERT_FALSE(result.has_value(), "Holding queue full must reject");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(SubmitError::HoldingQueueFull),
                   "Error must be HoldingQueueFull");
    return true;
}

FATP_TEST_CASE(release_from_holding_queue_decrements_depth)
{
    AdmissionControl ac(holdingConfig(2));
    (void)ac.evaluate(Priority::Critical, true); // depth = 1

    ac.releaseFromHoldingQueue(); // depth = 0
    FATP_ASSERT_EQ(ac.holdingQueueDepth(), uint32_t(0),
        "releaseFromHoldingQueue must decrement depth");

    // Now a new Critical job must be admitted.
    auto result = ac.evaluate(Priority::Critical, true);
    FATP_ASSERT_TRUE(result.has_value(), "Must admit after release");
    return true;
}

// ============================================================================
// Stacked layers
// ============================================================================

FATP_TEST_CASE(global_limit_checked_before_per_priority)
{
    AdmissionConfig cfg;
    cfg.globalRateLimitJps = 1;    // 1 global token
    cfg.normalRateLimitJps = 1000; // generous per-priority
    AdmissionControl ac(cfg);

    // Consume the global token.
    auto first = ac.evaluate(Priority::Normal, false);
    FATP_ASSERT_TRUE(first.has_value(), "First must succeed");

    // Second must fail at layer 1 (global), not layer 2 (per-priority).
    auto second = ac.evaluate(Priority::Normal, false);
    FATP_ASSERT_FALSE(second.has_value(), "Second must fail");
    FATP_ASSERT_EQ(static_cast<int>(second.error()),
                   static_cast<int>(SubmitError::RateLimited),
                   "Must fail at global layer, not priority layer");

    // Rejection counters must reflect the global layer.
    FATP_ASSERT_EQ(ac.rejectionCount(SubmitError::RateLimited),    uint64_t(1), "RateLimited=1");
    FATP_ASSERT_EQ(ac.rejectionCount(SubmitError::PriorityRejected), uint64_t(0), "PriorityRejected=0");
    return true;
}

FATP_TEST_CASE(rejection_counters_are_independent_per_error)
{
    AdmissionConfig cfg;
    cfg.globalRateLimitJps = 2;
    cfg.normalRateLimitJps = 1;
    cfg.holdingQueueCapacity = 1;
    AdmissionControl ac(cfg);

    // Produce RateLimited: drain global, then one more.
    (void)ac.evaluate(Priority::Normal, false);
    (void)ac.evaluate(Priority::Normal, false);
    (void)ac.evaluate(Priority::Normal, false); // RateLimited

    // Reset by making a fresh object, then produce PriorityRejected.
    AdmissionControl ac2(cfg);
    (void)ac2.evaluate(Priority::Normal, false);
    (void)ac2.evaluate(Priority::Normal, false); // PriorityRejected (per-priority drained)

    FATP_ASSERT_EQ(ac2.rejectionCount(SubmitError::RateLimited), uint64_t(0),
        "RateLimited counter must be 0");
    FATP_ASSERT_EQ(ac2.rejectionCount(SubmitError::PriorityRejected), uint64_t(1),
        "PriorityRejected counter must be 1");
    return true;
}

// ============================================================================
// Thread safety smoke test
// ============================================================================

FATP_TEST_CASE(concurrent_submits_do_not_crash)
{
    AdmissionConfig cfg;
    cfg.globalRateLimitJps = 1000;
    AdmissionControl ac(cfg);

    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i)
    {
        threads.emplace_back([&ac]
        {
            for (int j = 0; j < 100; ++j)
            {
                (void)ac.evaluate(Priority::Normal, false);
            }
        });
    }
    for (auto& t : threads) { t.join(); }
    return true;
}

} // namespace balancer::testing::admission

// ============================================================================
// Public interface
// ============================================================================

namespace balancer::testing
{

bool test_admission()
{
    FATP_PRINT_HEADER(ADMISSION CONTROL)

    fat_p::testing::TestRunner runner;

    FATP_RUN_TEST_NS(runner, admission, unlimited_admits_all_priorities);
    FATP_RUN_TEST_NS(runner, admission, global_rate_limit_triggers_RateLimited);
    FATP_RUN_TEST_NS(runner, admission, global_rate_limit_applies_to_all_priorities);
    FATP_RUN_TEST_NS(runner, admission, normal_per_priority_limit_triggers_PriorityRejected);
    FATP_RUN_TEST_NS(runner, admission, low_per_priority_limit_triggers_PriorityRejected);
    FATP_RUN_TEST_NS(runner, admission, bulk_per_priority_limit_triggers_PriorityRejected);
    FATP_RUN_TEST_NS(runner, admission, critical_bypasses_per_priority_limits);
    FATP_RUN_TEST_NS(runner, admission, cluster_saturated_rejects_normal_and_below);
    FATP_RUN_TEST_NS(runner, admission, cluster_saturated_critical_goes_to_holding_queue);
    FATP_RUN_TEST_NS(runner, admission, cluster_saturated_high_goes_to_holding_queue);
    FATP_RUN_TEST_NS(runner, admission, holding_queue_full_produces_HoldingQueueFull);
    FATP_RUN_TEST_NS(runner, admission, release_from_holding_queue_decrements_depth);
    FATP_RUN_TEST_NS(runner, admission, global_limit_checked_before_per_priority);
    FATP_RUN_TEST_NS(runner, admission, rejection_counters_are_independent_per_error);
    FATP_RUN_TEST_NS(runner, admission, concurrent_submits_do_not_crash);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_admission() ? 0 : 1;
}
#endif
