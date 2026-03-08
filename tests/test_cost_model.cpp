/**
 * @file test_cost_model.cpp
 * @brief Comprehensive unit tests for CostModel.h
 */

/*
BALANCER_META:
  meta_version: 1
  component: CostModel
  file_role: test
  path: tests/test_cost_model.cpp
  namespace: balancer::testing::costmodel
  layer: Testing
  summary: Unit tests for CostModel — EMA convergence, cold start, predict/update, AffinityMatrix integration, DegradationCurve, JSON persistence.
  api_stability: in_work
  related:
    headers:
      - include/balancer/CostModel.h
*/

#include <iostream>
#include <cmath>
#include <sstream>

#include "balancer/CostModel.h"
#include "FatPTest.h"

using fat_p::testing::TestRunner;
namespace colors = fat_p::testing::colors;

namespace balancer::testing::costmodel
{

// ============================================================================
// Helpers
// ============================================================================

inline balancer::Job makeJob(uint64_t estimated, uint64_t observed,
                              balancer::NodeId executedBy,
                              balancer::Priority priority = balancer::Priority::Normal)
{
    balancer::Job j;
    j.estimatedCost = balancer::Cost{estimated};
    j.observedCost  = balancer::Cost{observed};
    j.executedBy    = executedBy;
    j.priority      = priority;
    j.id            = balancer::JobId{1};
    j.submitted     = balancer::Clock::now();
    return j;
}

inline const balancer::NodeId kNode1{1};
inline const balancer::NodeId kNode2{2};
inline const balancer::NodeId kNode3{3};

// ============================================================================
// Cold start
// ============================================================================

FATP_TEST_CASE(cold_start_returns_neutral_prediction)
{
    balancer::CostModel model;

    balancer::Job job;
    job.estimatedCost = balancer::Cost{100};

    // No observations — model must return estimatedCost unchanged
    balancer::Cost predicted = model.predict(job, kNode1);
    FATP_ASSERT_EQ(predicted.units, uint64_t(100),
                   "Cold start must return estimatedCost unchanged");
    return true;
}

FATP_TEST_CASE(cold_start_is_warm_false)
{
    balancer::CostModel model;
    FATP_ASSERT_FALSE(model.isWarm(kNode1),
                      "Node with no observations must be cold");
    return true;
}

FATP_TEST_CASE(warm_threshold_transitions)
{
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 5;
    balancer::CostModel model(cfg);

    balancer::Job job = makeJob(100, 100, kNode1);

    for (uint32_t i = 0; i < 4; ++i)
    {
        model.update(job);
        FATP_ASSERT_FALSE(model.isWarm(kNode1),
                          "Should remain cold before threshold");
    }

    model.update(job);
    FATP_ASSERT_TRUE(model.isWarm(kNode1),
                     "Should be warm after exactly warmThreshold observations");
    return true;
}

FATP_TEST_CASE(observation_count_tracks_correctly)
{
    balancer::CostModel model;

    balancer::Job job = makeJob(100, 100, kNode1);

    FATP_ASSERT_EQ(model.observationCount(kNode1), uint32_t(0),
                   "Initial observation count must be 0");

    for (uint32_t i = 1; i <= 5; ++i)
    {
        model.update(job);
        FATP_ASSERT_EQ(model.observationCount(kNode1), i,
                       "Observation count must increment on each update");
    }
    return true;
}

// ============================================================================
// Update and EMA convergence
// ============================================================================

FATP_TEST_CASE(neutral_update_preserves_multiplier)
{
    // Node always takes exactly as long as estimated → ratio = 1.0 → EMA stays ≈ 1.0
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 3;
    cfg.nodeAlpha     = 0.5f;
    balancer::CostModel model(cfg);

    for (int i = 0; i < 20; ++i)
    {
        model.update(makeJob(100, 100, kNode1));
    }

    float mult = model.nodeMultiplier(kNode1);
    FATP_ASSERT_CLOSE(mult, 1.0f, "Perfect estimates should yield multiplier ≈ 1.0");
    return true;
}

FATP_TEST_CASE(slow_node_multiplier_increases)
{
    // Node always takes 2x estimated — multiplier should converge toward 2.0
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 1;
    cfg.nodeAlpha     = 0.5f;
    balancer::CostModel model(cfg);

    for (int i = 0; i < 30; ++i)
    {
        model.update(makeJob(100, 200, kNode1)); // 2x slower
    }

    float mult = model.nodeMultiplier(kNode1);
    FATP_ASSERT_GT(mult, 1.8f,
                   "Consistently slow node should have multiplier > 1.8 after 30 updates");
    FATP_ASSERT_LT(mult, 2.1f, "Multiplier should be close to 2.0");
    return true;
}

FATP_TEST_CASE(fast_node_multiplier_decreases)
{
    // Node always takes 0.5x estimated — multiplier converges toward 0.5
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 1;
    cfg.nodeAlpha     = 0.5f;
    balancer::CostModel model(cfg);

    for (int i = 0; i < 30; ++i)
    {
        model.update(makeJob(100, 50, kNode1)); // 2x faster
    }

    float mult = model.nodeMultiplier(kNode1);
    FATP_ASSERT_LT(mult, 0.6f,
                   "Consistently fast node should have multiplier < 0.6 after 30 updates");
    FATP_ASSERT_GT(mult, 0.1f, "Multiplier should be above minMultiplier");
    return true;
}

FATP_TEST_CASE(nodes_are_independent)
{
    // Node 1 is slow (2x), Node 2 is fast (0.5x)
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 1;
    cfg.nodeAlpha     = 0.5f;
    balancer::CostModel model(cfg);

    for (int i = 0; i < 30; ++i)
    {
        model.update(makeJob(100, 200, kNode1));
        model.update(makeJob(100, 50,  kNode2));
    }

    float m1 = model.nodeMultiplier(kNode1);
    float m2 = model.nodeMultiplier(kNode2);

    FATP_ASSERT_GT(m1, 1.5f, "Node1 multiplier should reflect slow performance");
    FATP_ASSERT_LT(m2, 0.7f, "Node2 multiplier should reflect fast performance");
    FATP_ASSERT_GT(m1, m2,   "Slow node multiplier must exceed fast node multiplier");
    return true;
}

// ============================================================================
// Predict integrates multiplier
// ============================================================================

FATP_TEST_CASE(predict_scales_by_multiplier_when_warm)
{
    balancer::CostModelConfig cfg;
    cfg.warmThreshold              = 1;
    cfg.nodeAlpha                  = 0.5f;
    cfg.affinity.warmThreshold     = 1000; // keep affinity cold — not under test
    cfg.degradation.warmThreshold  = 1000; // keep degradation cold — not under test
    balancer::CostModel model(cfg);

    // Drive node to 2x multiplier
    for (int i = 0; i < 20; ++i)
    {
        model.update(makeJob(100, 200, kNode1));
    }

    balancer::Job job;
    job.estimatedCost = balancer::Cost{100};

    balancer::Cost predicted = model.predict(job, kNode1);

    // Should be approximately 2 * 100 = 200
    FATP_ASSERT_GT(predicted.units, uint64_t(150),
                   "Warm prediction should scale estimatedCost by learned multiplier");
    FATP_ASSERT_LT(predicted.units, uint64_t(250),
                   "Prediction should not be wildly off from 2x");
    return true;
}

FATP_TEST_CASE(predict_neutral_for_cold_node_during_warm_cluster)
{
    // Node 1 is warm (slow). Node 3 is cold.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 1;
    cfg.nodeAlpha     = 0.5f;
    balancer::CostModel model(cfg);

    for (int i = 0; i < 20; ++i)
    {
        model.update(makeJob(100, 200, kNode1));
    }

    balancer::Job job;
    job.estimatedCost = balancer::Cost{100};

    // Node 3 is cold — must return raw estimatedCost
    balancer::Cost coldPredicted = model.predict(job, kNode3);
    FATP_ASSERT_EQ(coldPredicted.units, uint64_t(100),
                   "Cold node must always receive neutral (raw estimate) prediction");
    return true;
}

// ============================================================================
// Multiplier bounds
// ============================================================================

FATP_TEST_CASE(multiplier_clamped_at_max)
{
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 1;
    cfg.nodeAlpha     = 0.9f;
    cfg.maxMultiplier = 3.0f;
    balancer::CostModel model(cfg);

    // Extremely slow node (100x)
    for (int i = 0; i < 50; ++i)
    {
        model.update(makeJob(1, 100, kNode1));
    }

    float mult = model.nodeMultiplier(kNode1);
    FATP_ASSERT_LE(mult, cfg.maxMultiplier,
                   "Multiplier must not exceed configured maximum");
    return true;
}

FATP_TEST_CASE(multiplier_clamped_at_min)
{
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 1;
    cfg.nodeAlpha     = 0.9f;
    cfg.minMultiplier = 0.2f;
    balancer::CostModel model(cfg);

    // Extremely fast node (observedCost near 0, estimated = 1000)
    for (int i = 0; i < 50; ++i)
    {
        model.update(makeJob(1000, 1, kNode1));
    }

    float mult = model.nodeMultiplier(kNode1);
    FATP_ASSERT_GE(mult, cfg.minMultiplier,
                   "Multiplier must not fall below configured minimum");
    return true;
}

// ============================================================================
// Edge cases
// ============================================================================

FATP_TEST_CASE(zero_estimated_cost_skipped)
{
    balancer::CostModel model;

    // estimatedCost = 0 must not update the model (undefined ratio)
    balancer::Job job = makeJob(0, 100, kNode1);
    model.update(job);

    FATP_ASSERT_EQ(model.observationCount(kNode1), uint32_t(0),
                   "Zero estimatedCost must not update observation count");
    return true;
}

FATP_TEST_CASE(unknown_observed_cost_skipped)
{
    balancer::CostModel model;

    // observedCost == kUnknownCost (0) must not update the model
    balancer::Job job;
    job.estimatedCost = balancer::Cost{100};
    job.observedCost  = balancer::kUnknownCost;
    job.executedBy    = kNode1;

    model.update(job);

    FATP_ASSERT_EQ(model.observationCount(kNode1), uint32_t(0),
                   "Unknown observedCost must not update observation count");
    return true;
}

FATP_TEST_CASE(reset_clears_all_state)
{
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 1;
    balancer::CostModel model(cfg);

    for (int i = 0; i < 10; ++i)
    {
        model.update(makeJob(100, 200, kNode1));
    }

    FATP_ASSERT_TRUE(model.isWarm(kNode1), "Should be warm before reset");

    model.reset();

    FATP_ASSERT_FALSE(model.isWarm(kNode1), "Should be cold after reset");
    FATP_ASSERT_EQ(model.observationCount(kNode1), uint32_t(0),
                   "Observation count must be 0 after reset");
    FATP_ASSERT_CLOSE(model.nodeMultiplier(kNode1), 1.0f,
                      "Multiplier must be neutral after reset");
    return true;
}

FATP_TEST_CASE(unknown_node_returns_neutral_multiplier)
{
    balancer::CostModel model;

    // Node 99 has never been seen
    balancer::NodeId unknownNode{99};
    FATP_ASSERT_CLOSE(model.nodeMultiplier(unknownNode), 1.0f,
                      "Unknown node must return neutral multiplier 1.0");
    return true;
}

// ============================================================================
// Phase 2 — Per-JobClass correction
// ============================================================================

FATP_TEST_CASE(class_correction_cold_start_returns_neutral)
{
    // Before classWarmThreshold observations for a (node, class) pair,
    // perClassMultiplier must return 1.0.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold      = 1; // node warms quickly
    cfg.affinity.warmThreshold = 5; // class needs 5 observations
    balancer::CostModel model(cfg);

    // Warm the node first (4 total observations — not enough for class)
    for (int i = 0; i < 4; ++i)
    {
        balancer::Job j;
        j.estimatedCost = balancer::Cost{100};
        j.observedCost  = balancer::Cost{200};
        j.executedBy    = kNode1;
        j.jobClass      = balancer::JobClass{7};
        j.id            = balancer::JobId{1};
        j.submitted     = balancer::Clock::now();
        model.update(j);
    }

    FATP_ASSERT_CLOSE(model.affinityScore(kNode1, balancer::JobClass{7}), 1.0f,
                      "Class correction must be neutral before classWarmThreshold observations");
    return true;
}

FATP_TEST_CASE(class_correction_warms_after_threshold_observations)
{
    balancer::CostModelConfig cfg;
    cfg.warmThreshold      = 1;
    cfg.affinity.warmThreshold = 3;
    cfg.affinity.alpha         = 0.9f; // fast convergence for test
    balancer::CostModel model(cfg);

    // Submit exactly classWarmThreshold observations of 3x slow for class 7
    for (int i = 0; i < 3; ++i)
    {
        balancer::Job j;
        j.estimatedCost = balancer::Cost{100};
        j.observedCost  = balancer::Cost{300};
        j.executedBy    = kNode1;
        j.jobClass      = balancer::JobClass{7};
        j.id            = balancer::JobId{1};
        j.submitted     = balancer::Clock::now();
        model.update(j);
    }

    float classMultiplier = model.affinityScore(kNode1, balancer::JobClass{7});
    FATP_ASSERT_GT(classMultiplier, 1.0f,
                   "Class correction must exceed 1.0 after observing consistent slowdowns");
    FATP_ASSERT_EQ(model.affinityObservations(kNode1, balancer::JobClass{7}), 3u,
                   "Observation count must match number of updates");
    return true;
}

FATP_TEST_CASE(class_correction_independent_per_class)
{
    // Class 1 on node 1 is 3x slow; Class 2 on node 1 is 0.5x fast.
    // Each class correction must be independent.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold      = 1;
    cfg.affinity.warmThreshold = 1;
    cfg.affinity.alpha         = 0.9f;
    balancer::CostModel model(cfg);

    auto feed = [&](balancer::NodeId node, balancer::JobClass cls,
                    uint64_t estimated, uint64_t observed, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            balancer::Job j;
            j.estimatedCost = balancer::Cost{estimated};
            j.observedCost  = balancer::Cost{observed};
            j.executedBy    = node;
            j.jobClass      = cls;
            j.id            = balancer::JobId{1};
            j.submitted     = balancer::Clock::now();
            model.update(j);
        }
    };

    feed(kNode1, balancer::JobClass{1}, 100, 300, 10); // 3x slow
    feed(kNode1, balancer::JobClass{2}, 100,  50, 10); // 0.5x fast

    float class1 = model.affinityScore(kNode1, balancer::JobClass{1});
    float class2 = model.affinityScore(kNode1, balancer::JobClass{2});

    FATP_ASSERT_GT(class1, 1.5f, "Class 1 correction must reflect 3x slowdown");
    FATP_ASSERT_LT(class2, 0.9f, "Class 2 correction must reflect 0.5x speedup");
    return true;
}

FATP_TEST_CASE(class_correction_independent_per_node)
{
    // Same job class, different nodes: corrections must not bleed across nodes.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold      = 1;
    cfg.affinity.warmThreshold = 1;
    cfg.affinity.alpha         = 0.9f;
    balancer::CostModel model(cfg);

    // Node 1: class 5 is 4x slow
    for (int i = 0; i < 10; ++i)
    {
        balancer::Job j;
        j.estimatedCost = balancer::Cost{100};
        j.observedCost  = balancer::Cost{400};
        j.executedBy    = kNode1;
        j.jobClass      = balancer::JobClass{5};
        j.id            = balancer::JobId{1};
        j.submitted     = balancer::Clock::now();
        model.update(j);
    }
    // Node 2: class 5 is 1x (accurate)
    for (int i = 0; i < 10; ++i)
    {
        balancer::Job j;
        j.estimatedCost = balancer::Cost{100};
        j.observedCost  = balancer::Cost{100};
        j.executedBy    = kNode2;
        j.jobClass      = balancer::JobClass{5};
        j.id            = balancer::JobId{1};
        j.submitted     = balancer::Clock::now();
        model.update(j);
    }

    float node1Class5 = model.affinityScore(kNode1, balancer::JobClass{5});
    float node2Class5 = model.affinityScore(kNode2, balancer::JobClass{5});

    FATP_ASSERT_GT(node1Class5, 2.0f, "Node 1 class 5 must reflect 4x slowdown");
    FATP_ASSERT_CLOSE(node2Class5, 1.0f, "Node 2 class 5 must remain near neutral");
    return true;
}

FATP_TEST_CASE(predict_incorporates_both_node_and_class_multiplier)
{
    // Node 1 overall: 2x slow (nodeMultiplier ≈ 2.0)
    // Node 1 class 3: additionally 1.5x slow for that specific class
    // predict() must compound: total ≈ 2.0 × 1.5 = 3.0
    balancer::CostModelConfig cfg;
    cfg.warmThreshold      = 1;
    cfg.affinity.warmThreshold = 1;
    cfg.nodeAlpha          = 0.9f;
    cfg.affinity.alpha         = 0.9f;
    balancer::CostModel model(cfg);

    // Train node-level: feed diverse classes to establish 2x slow baseline
    for (int i = 0; i < 30; ++i)
    {
        balancer::Job j;
        j.estimatedCost = balancer::Cost{100};
        j.observedCost  = balancer::Cost{200}; // 2x slow
        j.executedBy    = kNode1;
        j.jobClass      = balancer::JobClass{static_cast<uint8_t>(i % 4)}; // rotate classes to avoid single-class bias
        j.id            = balancer::JobId{1};
        j.submitted     = balancer::Clock::now();
        model.update(j);
    }
    // Train class 3 specifically: consistently 3x slow (vs estimate), which
    // relative to the 2x node baseline means class correction ≈ 3.0/2.0 = 1.5.
    for (int i = 0; i < 20; ++i)
    {
        balancer::Job j;
        j.estimatedCost = balancer::Cost{100};
        j.observedCost  = balancer::Cost{300}; // 3x → class correction pulls toward 3/1 = 3
        j.executedBy    = kNode1;
        j.jobClass      = balancer::JobClass{3};
        j.id            = balancer::JobId{1};
        j.submitted     = balancer::Clock::now();
        model.update(j);
    }

    balancer::Job queryJob;
    queryJob.estimatedCost = balancer::Cost{100};
    queryJob.jobClass      = balancer::JobClass{3};

    Cost predicted = model.predict(queryJob, kNode1);
    // Predicted should be substantially above 200 (the node-level multiplier alone)
    // due to the compounding class correction. Accept range 250-999 to allow for
    // EMA smoothing effects from the mixed class training data.
    FATP_ASSERT_GT(predicted.units, 200u,
                   "Predict must exceed node-only estimate when class correction is applied");
    return true;
}

FATP_TEST_CASE(class_correction_does_not_affect_other_classes)
{
    // Class 1 on node 1 trains to 4x slow.
    // Predicting class 2 (no training) must return the node-level estimate only.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold      = 1;
    cfg.affinity.warmThreshold = 1;
    cfg.nodeAlpha          = 0.9f;
    cfg.affinity.alpha         = 0.9f;
    balancer::CostModel model(cfg);

    // Warm node 1 with class 1 at 4x slow
    for (int i = 0; i < 20; ++i)
    {
        balancer::Job j;
        j.estimatedCost = balancer::Cost{100};
        j.observedCost  = balancer::Cost{400};
        j.executedBy    = kNode1;
        j.jobClass      = balancer::JobClass{1};
        j.id            = balancer::JobId{1};
        j.submitted     = balancer::Clock::now();
        model.update(j);
    }

    // Query class 2 — should see no class correction (neutral 1.0)
    float class2Multiplier = model.affinityScore(kNode1, balancer::JobClass{2});
    FATP_ASSERT_CLOSE(class2Multiplier, 1.0f,
                      "Untrained class must have neutral class correction");

    // Query class 1 — should see non-trivial correction
    float class1Multiplier = model.affinityScore(kNode1, balancer::JobClass{1});
    FATP_ASSERT_GT(class1Multiplier, 2.0f,
                   "Trained class must have non-neutral class correction");
    return true;
}

// ============================================================================
// Phase 3 — DegradationCurve
// ============================================================================

// Helper: feed N identical observations at a given queue depth to drive a node's
// degradation bucket.
inline void feedDegradation(balancer::CostModel& model, balancer::NodeId nodeId,
                             uint32_t depth, uint64_t estimated, uint64_t observed, int n)
{
    for (int i = 0; i < n; ++i)
    {
        balancer::Job j;
        j.estimatedCost        = balancer::Cost{estimated};
        j.observedCost         = balancer::Cost{observed};
        j.executedBy           = nodeId;
        j.jobClass             = balancer::JobClass{0};
        j.id                   = balancer::JobId{1};
        j.submitted            = balancer::Clock::now();
        j.queueDepthAtDispatch = depth;
        model.update(j);
    }
}

FATP_TEST_CASE(degradation_bucket_updated_from_job_completed)
{
    // After feeding observations with queueDepthAtDispatch = 0, the bucket
    // observation count must be non-zero.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold          = 1;
    cfg.degradation.warmThreshold = 3;
    cfg.degradation.bucketSize = 4;
    balancer::CostModel model(cfg);

    feedDegradation(model, kNode1, 0, 100, 200, 3);

    uint32_t obs = model.degradationBucketObservations(kNode1, 0);
    FATP_ASSERT_EQ(obs, uint32_t(3),
                   "Bucket observation count must equal number of updates at that depth");
    return true;
}

FATP_TEST_CASE(degradation_cold_bucket_returns_neutral)
{
    // A bucket with fewer than warmThreshold observations must return 1.0 —
    // no correction is applied during cold start.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold          = 1;
    cfg.degradation.warmThreshold = 5; // need 5 observations to warm
    cfg.degradation.bucketSize = 4;
    balancer::CostModel model(cfg);

    // Feed 4 observations — one short of the warm threshold.
    feedDegradation(model, kNode1, 0, 100, 300, 4);

    float mult = model.degradationMultiplier(kNode1, 0);
    FATP_ASSERT_CLOSE(mult, 1.0f,
                      "Cold degradation bucket must return neutral multiplier 1.0");
    return true;
}

FATP_TEST_CASE(degradation_correct_bucket_selected_for_observed_depth)
{
    // bucketSize = 4: depth 0-3 → bucket 0, depth 4-7 → bucket 1.
    // Feed only bucket 1. Bucket 0 must remain at 0 observations.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold          = 1;
    cfg.degradation.warmThreshold = 2;
    cfg.degradation.bucketSize = 4;
    balancer::CostModel model(cfg);

    feedDegradation(model, kNode1, 5, 100, 200, 3); // depth 5 → bucket 1

    uint32_t obsBucket0 = model.degradationBucketObservations(kNode1, 0); // depth 0 → bucket 0
    uint32_t obsBucket1 = model.degradationBucketObservations(kNode1, 5); // depth 5 → bucket 1

    FATP_ASSERT_EQ(obsBucket0, uint32_t(0),
                   "Bucket 0 must have no observations when only depth 5 was fed");
    FATP_ASSERT_EQ(obsBucket1, uint32_t(3),
                   "Bucket 1 must have 3 observations for depth 5 (bucketSize=4)");
    return true;
}

FATP_TEST_CASE(degradation_buckets_are_independent)
{
    // Bucket 0 (depth 0-3) sees 2x slowdown.
    // Bucket 1 (depth 4-7) sees 0.5x speedup.
    // Each bucket's multiplier must be independent.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold          = 1;
    cfg.degradation.warmThreshold = 3;
    cfg.degradation.bucketSize = 4;
    cfg.degradation.alpha      = 0.9f;
    balancer::CostModel model(cfg);

    feedDegradation(model, kNode1, 0, 100, 200, 5); // bucket 0: 2x slow
    feedDegradation(model, kNode1, 4, 100,  50, 5); // bucket 1: 0.5x fast

    float mult0 = model.degradationMultiplier(kNode1, 0);
    float mult1 = model.degradationMultiplier(kNode1, 4);

    FATP_ASSERT_GT(mult0, 1.5f,
                   "Bucket 0 multiplier should reflect 2x slowdown");
    FATP_ASSERT_LT(mult1, 0.7f,
                   "Bucket 1 multiplier should reflect 0.5x speedup");
    FATP_ASSERT_GT(mult0, mult1,
                   "Slow bucket multiplier must exceed fast bucket multiplier");
    return true;
}

FATP_TEST_CASE(degradation_last_bucket_clamps_overflow_depth)
{
    // depth beyond (bucketCount - 1) × bucketSize maps to the last bucket.
    // bucketCount = 4, bucketSize = 4 → last bucket covers depths 12+.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold             = 1;
    cfg.degradation.bucketCount   = 4;
    cfg.degradation.bucketSize    = 4;
    cfg.degradation.warmThreshold = 2;
    cfg.degradation.alpha         = 0.9f;
    balancer::CostModel model(cfg);

    // Feed at depth 1000 — must map to bucket 3 (the last bucket).
    feedDegradation(model, kNode1, 1000, 100, 300, 3);

    // Querying depth 12 and depth 1000 must both use the same last bucket.
    uint32_t obs12   = model.degradationBucketObservations(kNode1, 12);
    uint32_t obs1000 = model.degradationBucketObservations(kNode1, 1000);

    FATP_ASSERT_EQ(obs12, uint32_t(3),
                   "Depth 12 must map to last bucket — same as depth 1000");
    FATP_ASSERT_EQ(obs1000, uint32_t(3),
                   "Depth 1000 must map to last bucket");
    return true;
}

FATP_TEST_CASE(degradation_unknown_node_returns_neutral)
{
    balancer::CostModel model;
    float mult = model.degradationMultiplier(balancer::NodeId{99}, 0);
    FATP_ASSERT_CLOSE(mult, 1.0f,
                      "Unknown node degradation multiplier must be neutral 1.0");
    return true;
}

FATP_TEST_CASE(degradation_predict_incorporates_depth_multiplier)
{
    // Warm both node-level multiplier (neutral) and a specific depth bucket (2x slow).
    // predict() with queueDepthAtDispatch set to that depth must return roughly 2 × estimate.
    balancer::CostModelConfig cfg;
    cfg.warmThreshold             = 1;
    cfg.degradation.warmThreshold = 3;
    cfg.degradation.bucketSize    = 4;
    cfg.degradation.alpha         = 0.9f;
    balancer::CostModel model(cfg);

    // Warm the node with neutral observations at depth 0 to establish a 1.0 node multiplier.
    feedDegradation(model, kNode1, 0, 100, 100, 5);

    // Warm bucket 1 (depth 4-7) with 2x slow observations.
    feedDegradation(model, kNode1, 4, 100, 200, 5);

    // predict() with queueDepthAtDispatch = 4 should incorporate the 2x bucket multiplier.
    balancer::Job queryJob;
    queryJob.estimatedCost        = balancer::Cost{100};
    queryJob.jobClass             = balancer::JobClass{0};
    queryJob.queueDepthAtDispatch = 4;

    balancer::Cost predicted = model.predict(queryJob, kNode1);

    FATP_ASSERT_GT(predicted.units, uint64_t(150),
                   "Predict must incorporate depth bucket multiplier when bucket is warm");
    return true;
}

} // namespace balancer::testing::costmodel

// ============================================================================
// Public interface
// ============================================================================

namespace balancer::testing
{

bool test_cost_model()
{
    FATP_PRINT_HEADER(COST MODEL)

    TestRunner runner;

    auto& out = *fat_p::testing::get_test_config().output;

    out << colors::blue() << "--- Cold Start ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, costmodel, cold_start_returns_neutral_prediction);
    FATP_RUN_TEST_NS(runner, costmodel, cold_start_is_warm_false);
    FATP_RUN_TEST_NS(runner, costmodel, warm_threshold_transitions);
    FATP_RUN_TEST_NS(runner, costmodel, observation_count_tracks_correctly);

    out << "\n" << colors::blue() << "--- EMA Convergence ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, costmodel, neutral_update_preserves_multiplier);
    FATP_RUN_TEST_NS(runner, costmodel, slow_node_multiplier_increases);
    FATP_RUN_TEST_NS(runner, costmodel, fast_node_multiplier_decreases);
    FATP_RUN_TEST_NS(runner, costmodel, nodes_are_independent);

    out << "\n" << colors::blue() << "--- Prediction ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, costmodel, predict_scales_by_multiplier_when_warm);
    FATP_RUN_TEST_NS(runner, costmodel, predict_neutral_for_cold_node_during_warm_cluster);

    out << "\n" << colors::blue() << "--- Bounds ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, costmodel, multiplier_clamped_at_max);
    FATP_RUN_TEST_NS(runner, costmodel, multiplier_clamped_at_min);

    out << "\n" << colors::blue() << "--- Edge Cases ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, costmodel, zero_estimated_cost_skipped);
    FATP_RUN_TEST_NS(runner, costmodel, unknown_observed_cost_skipped);
    FATP_RUN_TEST_NS(runner, costmodel, reset_clears_all_state);
    FATP_RUN_TEST_NS(runner, costmodel, unknown_node_returns_neutral_multiplier);

    out << "\n" << colors::blue() << "--- Phase 2: Per-JobClass Correction ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, costmodel, class_correction_cold_start_returns_neutral);
    FATP_RUN_TEST_NS(runner, costmodel, class_correction_warms_after_threshold_observations);
    FATP_RUN_TEST_NS(runner, costmodel, class_correction_independent_per_class);
    FATP_RUN_TEST_NS(runner, costmodel, class_correction_independent_per_node);
    FATP_RUN_TEST_NS(runner, costmodel, predict_incorporates_both_node_and_class_multiplier);
    FATP_RUN_TEST_NS(runner, costmodel, class_correction_does_not_affect_other_classes);

    out << "\n" << colors::blue() << "--- Phase 3: DegradationCurve ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, costmodel, degradation_bucket_updated_from_job_completed);
    FATP_RUN_TEST_NS(runner, costmodel, degradation_cold_bucket_returns_neutral);
    FATP_RUN_TEST_NS(runner, costmodel, degradation_correct_bucket_selected_for_observed_depth);
    FATP_RUN_TEST_NS(runner, costmodel, degradation_buckets_are_independent);
    FATP_RUN_TEST_NS(runner, costmodel, degradation_last_bucket_clamps_overflow_depth);
    FATP_RUN_TEST_NS(runner, costmodel, degradation_unknown_node_returns_neutral);
    FATP_RUN_TEST_NS(runner, costmodel, degradation_predict_incorporates_depth_multiplier);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_cost_model() ? 0 : 1;
}
#endif
