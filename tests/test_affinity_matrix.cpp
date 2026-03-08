/**
 * @file test_affinity_matrix.cpp
 * @brief Unit tests for AffinityMatrix.h and CostModel Phase 5 features.
 */

/*
BALANCER_META:
  meta_version: 1
  component: AffinityMatrix
  file_role: test
  path: tests/test_affinity_matrix.cpp
  namespace: balancer::testing::affinity
  layer: Testing
  summary: Tests for AffinityMatrix — cold start, EMA convergence, bounds clamping, JSON persistence, and CostModel Phase 5 integration including file-level persistence.
  api_stability: in_work
  related:
    headers:
      - include/balancer/AffinityMatrix.h
      - include/balancer/CostModel.h
*/

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "balancer/AffinityMatrix.h"
#include "balancer/CostModel.h"
#include "FatPTest.h"

namespace balancer::testing::affinity
{

// ============================================================================
// Helpers
// ============================================================================

inline balancer::Job makeJob(uint64_t estimated, uint64_t observed,
                              balancer::NodeId executedBy,
                              balancer::JobClass jobClass = balancer::JobClass{0})
{
    balancer::Job j;
    j.estimatedCost = balancer::Cost{estimated};
    j.observedCost  = balancer::Cost{observed};
    j.executedBy    = executedBy;
    j.jobClass      = jobClass;
    j.priority      = balancer::Priority::Normal;
    j.id            = balancer::JobId{1};
    j.submitted     = balancer::Clock::now();
    return j;
}

inline const balancer::NodeId  kNode1{1};
inline const balancer::NodeId  kNode2{2};
inline const balancer::JobClass kClassA{0};
inline const balancer::JobClass kClassB{1};

// ============================================================================
// AffinityMatrix — cold start
// ============================================================================

FATP_TEST_CASE(cold_cell_returns_neutral)
{
    balancer::AffinityMatrixConfig cfg;
    cfg.warmThreshold = 5;
    balancer::AffinityMatrix mat(cfg);

    FATP_ASSERT_EQ(mat.get(0, 0), 1.0f, "value mismatch");
    FATP_ASSERT_EQ(mat.get(3, 7), 1.0f, "value mismatch");
    FATP_ASSERT_EQ(mat.observations(0, 0), 0u, "value mismatch");
    FATP_SIMPLE_ASSERT(!mat.isCellWarm(0, 0), "assertion failed");
    return true;
}

// ============================================================================
// AffinityMatrix — warm-up and EMA
// ============================================================================

FATP_TEST_CASE(warms_after_threshold_observations)
{
    balancer::AffinityMatrixConfig cfg;
    cfg.warmThreshold = 3;
    cfg.alpha         = 0.1f;
    balancer::AffinityMatrix mat(cfg);

    mat.update(0, 0, 2.0f);
    mat.update(0, 0, 2.0f);
    FATP_SIMPLE_ASSERT(!mat.isCellWarm(0, 0), "assertion failed");
    FATP_ASSERT_EQ(mat.get(0, 0), 1.0f, "value mismatch"); // still cold

    mat.update(0, 0, 2.0f); // third observation meets threshold
    FATP_SIMPLE_ASSERT(mat.isCellWarm(0, 0), "assertion failed");
    FATP_SIMPLE_ASSERT(mat.get(0, 0) > 1.0f, "assertion failed"); // EMA moved toward 2.0
    return true;
}

FATP_TEST_CASE(ema_converges_toward_ratio)
{
    balancer::AffinityMatrixConfig cfg;
    cfg.warmThreshold = 1;
    cfg.alpha         = 0.5f;
    balancer::AffinityMatrix mat(cfg);

    for (int i = 0; i < 30; ++i) { mat.update(0, 0, 2.0f); }

    float v = mat.get(0, 0);
    FATP_SIMPLE_ASSERT(v > 1.9f && v < 2.1f, "assertion failed");
    return true;
}

FATP_TEST_CASE(cells_are_independent)
{
    balancer::AffinityMatrixConfig cfg;
    cfg.warmThreshold = 1;
    cfg.alpha         = 0.5f;
    balancer::AffinityMatrix mat(cfg);

    for (int i = 0; i < 30; ++i) { mat.update(0, 0, 2.0f); }
    for (int i = 0; i < 30; ++i) { mat.update(1, 0, 0.5f); }

    FATP_SIMPLE_ASSERT(mat.get(0, 0) > 1.9f, "assertion failed");   // converged to ~2.0
    FATP_SIMPLE_ASSERT(mat.get(1, 0) < 0.6f, "assertion failed");   // converged to ~0.5
    FATP_ASSERT_EQ(mat.get(0, 1), 1.0f, "value mismatch"); // untouched — neutral
    return true;
}

// ============================================================================
// AffinityMatrix — bounds clamping
// ============================================================================

FATP_TEST_CASE(out_of_bounds_index_clamped_not_crashed)
{
    balancer::AffinityMatrixConfig cfg;
    cfg.maxNodes      = 4;
    cfg.maxJobClasses = 4;
    cfg.warmThreshold = 1;
    cfg.alpha         = 0.5f;
    balancer::AffinityMatrix mat(cfg);

    // OOB write clamped to last row/col [3,3]
    mat.update(100, 200, 3.0f);

    FATP_SIMPLE_ASSERT(mat.isCellWarm(3, 3), "assertion failed");
    FATP_SIMPLE_ASSERT(mat.get(3, 3) > 1.0f, "assertion failed");
    // OOB read must produce same result as the clamped cell
    FATP_ASSERT_EQ(mat.get(100, 200), mat.get(3, 3), "value mismatch");
    return true;
}

// ============================================================================
// AffinityMatrix — reset
// ============================================================================

FATP_TEST_CASE(reset_returns_to_neutral)
{
    balancer::AffinityMatrixConfig cfg;
    cfg.warmThreshold = 1;
    balancer::AffinityMatrix mat(cfg);

    for (int i = 0; i < 10; ++i) { mat.update(0, 0, 2.0f); }
    FATP_SIMPLE_ASSERT(mat.isCellWarm(0, 0), "assertion failed");

    mat.reset();
    FATP_SIMPLE_ASSERT(!mat.isCellWarm(0, 0), "assertion failed");
    FATP_ASSERT_EQ(mat.get(0, 0), 1.0f, "value mismatch");
    return true;
}

// ============================================================================
// AffinityMatrix — JSON round-trip
// ============================================================================

FATP_TEST_CASE(save_load_round_trip)
{
    balancer::AffinityMatrixConfig cfg;
    cfg.maxNodes      = 4;
    cfg.maxJobClasses = 4;
    cfg.warmThreshold = 1;
    cfg.alpha         = 0.5f;
    balancer::AffinityMatrix original(cfg);

    for (int i = 0; i < 10; ++i) { original.update(0, 0, 2.0f); }
    for (int i = 0; i < 10; ++i) { original.update(1, 2, 0.5f); }

    float    origV00   = original.get(0, 0);
    float    origV12   = original.get(1, 2);
    uint32_t origObs00 = original.observations(0, 0);
    uint32_t origObs12 = original.observations(1, 2);

    fat_p::JsonValue snap;
    original.save(snap);

    balancer::AffinityMatrix restored(cfg);
    FATP_SIMPLE_ASSERT(restored.load(snap), "assertion failed");

    FATP_SIMPLE_ASSERT(std::abs(restored.get(0, 0) - origV00) < 0.001f, "assertion failed");
    FATP_SIMPLE_ASSERT(std::abs(restored.get(1, 2) - origV12) < 0.001f, "assertion failed");
    FATP_ASSERT_EQ(restored.observations(0, 0), origObs00, "value mismatch");
    FATP_ASSERT_EQ(restored.observations(1, 2), origObs12, "value mismatch");
    FATP_ASSERT_EQ(restored.get(0, 1), 1.0f, "value mismatch"); // untouched cell
    return true;
}

FATP_TEST_CASE(load_rejects_dimension_mismatch)
{
    balancer::AffinityMatrixConfig cfg4;
    cfg4.maxNodes = 4; cfg4.maxJobClasses = 4;
    balancer::AffinityMatrix mat4(cfg4);
    fat_p::JsonValue snap;
    mat4.save(snap);

    balancer::AffinityMatrixConfig cfg8;
    cfg8.maxNodes = 8; cfg8.maxJobClasses = 8;
    balancer::AffinityMatrix mat8(cfg8);
    FATP_SIMPLE_ASSERT(!mat8.load(snap), "assertion failed");
    return true;
}

FATP_TEST_CASE(load_rejects_malformed_json)
{
    balancer::AffinityMatrixConfig cfg;
    balancer::AffinityMatrix mat(cfg);
    fat_p::JsonValue bad = fat_p::JsonValue{int64_t{42}};
    FATP_SIMPLE_ASSERT(!mat.load(bad), "assertion failed");
    return true;
}

// ============================================================================
// CostModel Phase 5 — affinity integration
// ============================================================================

FATP_TEST_CASE(costmodel_cold_affinity_returns_neutral)
{
    balancer::CostModel model;
    auto job = makeJob(100, 0, kNode1, kClassA);
    FATP_ASSERT_EQ(model.predict(job, kNode1).units, 100u, "value mismatch");
    return true;
}

FATP_TEST_CASE(costmodel_affinity_score_query)
{
    balancer::CostModelConfig cfg;
    cfg.warmThreshold           = 3;
    cfg.nodeAlpha               = 0.5f;
    cfg.affinity.warmThreshold  = 3;
    cfg.affinity.alpha          = 0.5f;
    balancer::CostModel model(cfg);

    for (int i = 0; i < 5; ++i)
    {
        model.update(makeJob(100, 200, kNode1, kClassA));
    }

    FATP_SIMPLE_ASSERT(model.isAffinityCellWarm(kNode1, kClassA), "assertion failed");
    FATP_SIMPLE_ASSERT(model.affinityScore(kNode1, kClassA) > 1.5f, "assertion failed"); // converging toward 2.0

    FATP_SIMPLE_ASSERT(!model.isAffinityCellWarm(kNode1, kClassB), "assertion failed");
    FATP_ASSERT_EQ(model.affinityScore(kNode1, kClassB), 1.0f, "value mismatch");
    return true;
}

FATP_TEST_CASE(costmodel_affinity_observation_count)
{
    balancer::CostModel model;
    FATP_ASSERT_EQ(model.affinityObservations(kNode1, kClassA), 0u, "value mismatch");

    model.update(makeJob(100, 150, kNode1, kClassA));
    FATP_ASSERT_EQ(model.affinityObservations(kNode1, kClassA), 1u, "value mismatch");
    return true;
}

FATP_TEST_CASE(costmodel_reset_clears_affinity)
{
    balancer::CostModelConfig cfg;
    cfg.warmThreshold           = 1;
    cfg.affinity.warmThreshold  = 1;
    balancer::CostModel model(cfg);

    model.update(makeJob(100, 200, kNode1, kClassA));
    FATP_SIMPLE_ASSERT(model.isAffinityCellWarm(kNode1, kClassA), "assertion failed");

    model.reset();
    FATP_SIMPLE_ASSERT(!model.isAffinityCellWarm(kNode1, kClassA), "assertion failed");
    FATP_ASSERT_EQ(model.affinityObservations(kNode1, kClassA), 0u, "value mismatch");
    return true;
}

// ============================================================================
// CostModel — file-level persistence
// ============================================================================

FATP_TEST_CASE(costmodel_save_load_file_round_trip)
{
    const std::string path =
        (std::filesystem::temp_directory_path() / "balancer_model_test.json").string();

    balancer::CostModelConfig cfg;
    cfg.warmThreshold           = 3;
    cfg.nodeAlpha               = 0.5f;
    cfg.affinity.warmThreshold  = 3;
    cfg.affinity.alpha          = 0.5f;

    balancer::CostModel original(cfg);

    for (int i = 0; i < 5; ++i)
    {
        original.update(makeJob(100, 200, kNode1, kClassA));
    }
    for (int i = 0; i < 5; ++i)
    {
        original.update(makeJob(100, 50, kNode2, kClassB));
    }

    float    origN1Mult   = original.nodeMultiplier(kNode1);
    float    origAff1A    = original.affinityScore(kNode1, kClassA);
    uint32_t origObs1A    = original.affinityObservations(kNode1, kClassA);

    auto saveResult = original.save(path);
    FATP_SIMPLE_ASSERT(saveResult.has_value(), "assertion failed");

    balancer::CostModel restored(cfg);
    auto loadResult = restored.load(path);
    FATP_SIMPLE_ASSERT(loadResult.has_value(), "assertion failed");

    FATP_SIMPLE_ASSERT(std::abs(restored.nodeMultiplier(kNode1) - origN1Mult) < 0.01f, "assertion failed");
    FATP_SIMPLE_ASSERT(std::abs(restored.affinityScore(kNode1, kClassA) - origAff1A) < 0.01f, "assertion failed");
    FATP_ASSERT_EQ(restored.affinityObservations(kNode1, kClassA), origObs1A, "value mismatch");

    std::filesystem::remove(path);
    return true;
}

FATP_TEST_CASE(costmodel_load_missing_file_returns_error)
{
    balancer::CostModel model;
    auto r = model.load(
        (std::filesystem::temp_directory_path() / "balancer_does_not_exist_xyz_phase5.json").string());
    FATP_SIMPLE_ASSERT(!r.has_value(), "assertion failed");
    FATP_SIMPLE_ASSERT(r.error() == balancer::PersistError::FileOpenFailed, "assertion failed");
    return true;
}

FATP_TEST_CASE(costmodel_load_malformed_resets_model)
{
    const std::string path =
        (std::filesystem::temp_directory_path() / "balancer_bad_json_phase5.json").string();
    {
        std::ofstream out(path);
        out << "{ not : valid : json !!!";
    }

    balancer::CostModelConfig cfg;
    cfg.warmThreshold           = 1;
    cfg.affinity.warmThreshold  = 1;
    balancer::CostModel model(cfg);

    model.update(makeJob(100, 200, kNode1, kClassA));
    FATP_ASSERT_EQ(model.observationCount(kNode1), 1u, "value mismatch");

    auto r = model.load(path);
    FATP_SIMPLE_ASSERT(!r.has_value(), "assertion failed");
    FATP_ASSERT_EQ(model.observationCount(kNode1), 0u, "value mismatch"); // reset on failure

    std::filesystem::remove(path);
    return true;
}

} // namespace balancer::testing::affinity

namespace balancer::testing
{

using fat_p::testing::TestRunner;

bool test_affinity_matrix()
{
    FATP_PRINT_HEADER(AFFINITY MATRIX)

    TestRunner runner;

    FATP_RUN_TEST_NS(runner, affinity, cold_cell_returns_neutral);
    FATP_RUN_TEST_NS(runner, affinity, warms_after_threshold_observations);
    FATP_RUN_TEST_NS(runner, affinity, ema_converges_toward_ratio);
    FATP_RUN_TEST_NS(runner, affinity, cells_are_independent);
    FATP_RUN_TEST_NS(runner, affinity, out_of_bounds_index_clamped_not_crashed);
    FATP_RUN_TEST_NS(runner, affinity, reset_returns_to_neutral);
    FATP_RUN_TEST_NS(runner, affinity, save_load_round_trip);
    FATP_RUN_TEST_NS(runner, affinity, load_rejects_dimension_mismatch);
    FATP_RUN_TEST_NS(runner, affinity, load_rejects_malformed_json);
    FATP_RUN_TEST_NS(runner, affinity, costmodel_cold_affinity_returns_neutral);
    FATP_RUN_TEST_NS(runner, affinity, costmodel_affinity_score_query);
    FATP_RUN_TEST_NS(runner, affinity, costmodel_affinity_observation_count);
    FATP_RUN_TEST_NS(runner, affinity, costmodel_reset_clears_affinity);
    FATP_RUN_TEST_NS(runner, affinity, costmodel_save_load_file_round_trip);
    FATP_RUN_TEST_NS(runner, affinity, costmodel_load_missing_file_returns_error);
    FATP_RUN_TEST_NS(runner, affinity, costmodel_load_malformed_resets_model);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_affinity_matrix() ? 0 : 1;
}
#endif
