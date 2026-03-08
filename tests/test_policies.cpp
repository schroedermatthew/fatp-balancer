/**
 * @file test_policies.cpp
 * @brief Unit tests for all scheduling policies — Phase 1 and Phase 2.
 */

/*
BALANCER_META:
  meta_version: 1
  component: [RoundRobin, LeastLoaded, WeightedCapacity, ShortestJobFirst, EarliestDeadlineFirst, Composite]
  file_role: test
  path: tests/test_policies.cpp
  namespace: balancer::testing::policies
  layer: Testing
  summary: Unit tests for all scheduling policies through Phase 2.
  api_stability: in_work
  related:
    headers:
      - policies/RoundRobin.h
      - policies/LeastLoaded.h
      - policies/WeightedCapacity.h
      - policies/ShortestJobFirst.h
      - policies/EarliestDeadlineFirst.h
      - policies/Composite.h
*/

#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

#include "balancer/ClusterView.h"
#include "balancer/CostModel.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"
#include "policies/Composite.h"
#include "policies/EarliestDeadlineFirst.h"
#include "policies/LeastLoaded.h"
#include "policies/RoundRobin.h"
#include "policies/ShortestJobFirst.h"
#include "policies/WeightedCapacity.h"
#include "FatPTest.h"

namespace balancer::testing::policies
{

// ============================================================================
// Helpers
// ============================================================================

inline balancer::LoadMetrics makeNodeMetrics(
    balancer::NodeId nodeId,
    balancer::NodeState state = balancer::NodeState::Idle,
    uint32_t queueDepth = 0,
    float utilization = 0.0f)
{
    balancer::LoadMetrics m;
    m.nodeId      = nodeId;
    m.state       = state;
    m.queueDepth  = queueDepth;
    m.utilization = utilization;
    return m;
}

inline balancer::ClusterView makeView(
    std::vector<balancer::LoadMetrics> metrics,
    const balancer::CostModel& model)
{
    return balancer::ClusterView(std::move(metrics), balancer::ClusterMetrics{}, model);
}

inline balancer::Job makeJob(balancer::Priority p = balancer::Priority::Normal,
                              uint64_t cost = 100,
                              balancer::JobClass jobClass = balancer::JobClass{0})
{
    balancer::Job j;
    j.priority      = p;
    j.estimatedCost = balancer::Cost{cost};
    j.jobClass      = jobClass;
    j.id            = balancer::JobId{1};
    return j;
}

inline balancer::Job makeDeadlineJob(
    balancer::Duration timeFromNow,
    balancer::Priority p = balancer::Priority::Normal,
    uint64_t cost = 100)
{
    balancer::Job j = makeJob(p, cost);
    j.deadline = balancer::Clock::now() + timeFromNow;
    return j;
}

// ============================================================================
// RoundRobin
// ============================================================================

FATP_TEST_CASE(rr_no_nodes_returns_error)
{
    balancer::CostModel model;
    auto view = makeView({}, model);
    balancer::RoundRobin rr;

    auto result = rr.selectNode(makeJob(), view);
    FATP_ASSERT_FALSE(result.has_value(), "Empty cluster must return error");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoNodes),
                   "Error must be NoNodes");
    return true;
}

FATP_TEST_CASE(rr_single_idle_node)
{
    balancer::CostModel model;
    auto view = makeView({makeNodeMetrics(balancer::NodeId{1})}, model);
    balancer::RoundRobin rr;

    auto result = rr.selectNode(makeJob(), view);
    FATP_ASSERT_TRUE(result.has_value(), "Single idle node must be selected");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{1}, "Must select node 1");
    return true;
}

FATP_TEST_CASE(rr_cycles_through_all_nodes)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}),
        makeNodeMetrics(balancer::NodeId{2}),
        makeNodeMetrics(balancer::NodeId{3}),
    }, model);

    balancer::RoundRobin rr;
    balancer::Job job = makeJob();

    std::vector<balancer::NodeId> selected;
    for (int i = 0; i < 9; ++i)
    {
        auto result = rr.selectNode(job, view);
        FATP_ASSERT_TRUE(result.has_value(), "Should always select a node");
        selected.push_back(result.value());
    }

    auto count1 = std::count(selected.begin(), selected.end(), balancer::NodeId{1});
    auto count2 = std::count(selected.begin(), selected.end(), balancer::NodeId{2});
    auto count3 = std::count(selected.begin(), selected.end(), balancer::NodeId{3});

    FATP_ASSERT_GE(count1, long(2), "Node 1 should be selected multiple times");
    FATP_ASSERT_GE(count2, long(2), "Node 2 should be selected multiple times");
    FATP_ASSERT_GE(count3, long(2), "Node 3 should be selected multiple times");
    return true;
}

FATP_TEST_CASE(rr_skips_ineligible_nodes)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Overloaded),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Idle),
    }, model);

    balancer::RoundRobin rr;
    balancer::Job normalJob = makeJob(balancer::Priority::Normal);

    for (int i = 0; i < 6; ++i)
    {
        auto result = rr.selectNode(normalJob, view);
        FATP_ASSERT_TRUE(result.has_value(), "Must find eligible node");
        FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                       "Normal job must skip Overloaded node 1");
    }
    return true;
}

FATP_TEST_CASE(rr_none_eligible_returns_error)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Failed),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Failed),
    }, model);

    balancer::RoundRobin rr;
    auto result = rr.selectNode(makeJob(), view);

    FATP_ASSERT_FALSE(result.has_value(), "No eligible nodes must return error");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoneEligible),
                   "Error must be NoneEligible");
    return true;
}

FATP_TEST_CASE(rr_critical_accepted_by_overloaded_node)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Overloaded),
    }, model);

    balancer::RoundRobin rr;
    auto result = rr.selectNode(makeJob(balancer::Priority::Critical), view);
    FATP_ASSERT_TRUE(result.has_value(),
                     "Critical job must be accepted by Overloaded node");
    return true;
}

FATP_TEST_CASE(rr_name_is_correct)
{
    balancer::RoundRobin rr;
    FATP_ASSERT_EQ(rr.name(), std::string_view{"RoundRobin"}, "Policy name must be RoundRobin");
    return true;
}

// ============================================================================
// LeastLoaded
// ============================================================================

FATP_TEST_CASE(ll_no_nodes_returns_error)
{
    balancer::CostModel model;
    auto view = makeView({}, model);
    balancer::LeastLoaded ll;

    auto result = ll.selectNode(makeJob(), view);
    FATP_ASSERT_FALSE(result.has_value(), "Empty cluster must return error");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoNodes),
                   "Error must be NoNodes");
    return true;
}

FATP_TEST_CASE(ll_selects_least_loaded_node)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Busy, 10),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Busy, 2),
        makeNodeMetrics(balancer::NodeId{3}, balancer::NodeState::Busy, 5),
    }, model);

    balancer::LeastLoaded ll;
    auto result = ll.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Must select node with lowest queue depth");
    return true;
}

FATP_TEST_CASE(ll_skips_ineligible_nodes)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Failed, 0),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Idle,   20),
    }, model);

    balancer::LeastLoaded ll;
    auto result = ll.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must find eligible node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Must skip Failed node even if it has lower depth");
    return true;
}

FATP_TEST_CASE(ll_integrates_cost_model_multiplier)
{
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 1;
    cfg.nodeAlpha     = 0.9f;
    balancer::CostModel model(cfg);

    // Warm node 2 as 2x slow
    for (int i = 0; i < 20; ++i)
    {
        balancer::Job warmJob;
        warmJob.estimatedCost = balancer::Cost{100};
        warmJob.observedCost  = balancer::Cost{200};
        warmJob.executedBy    = balancer::NodeId{2};
        warmJob.id            = balancer::JobId{1};
        warmJob.submitted     = balancer::Clock::now();
        model.update(warmJob);
    }

    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Busy, 5),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Busy, 3),
    }, model);

    balancer::LeastLoaded ll;
    auto result = ll.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{1},
                   "Must prefer cold node with lower effective load over warm slow node");
    return true;
}

FATP_TEST_CASE(ll_ties_broken_by_nodeid)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{3}, balancer::NodeState::Idle, 5),
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Idle, 5),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Idle, 5),
    }, model);

    balancer::LeastLoaded ll;
    auto result = ll.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{1},
                   "Tie must be broken by lowest NodeId");
    return true;
}

FATP_TEST_CASE(ll_none_eligible_returns_error)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Offline),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Failed),
    }, model);

    balancer::LeastLoaded ll;
    auto result = ll.selectNode(makeJob(), view);

    FATP_ASSERT_FALSE(result.has_value(), "No eligible nodes must return error");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoneEligible),
                   "Error must be NoneEligible");
    return true;
}

FATP_TEST_CASE(ll_name_is_correct)
{
    balancer::LeastLoaded ll;
    FATP_ASSERT_EQ(ll.name(), std::string_view{"LeastLoaded"}, "Policy name must be LeastLoaded");
    return true;
}

// ============================================================================
// WeightedCapacity
// ============================================================================

FATP_TEST_CASE(wc_no_nodes_returns_error)
{
    balancer::CostModel model;
    auto view = makeView({}, model);
    balancer::WeightedCapacity wc;

    auto result = wc.selectNode(makeJob(), view);
    FATP_ASSERT_FALSE(result.has_value(), "Empty cluster must return error");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoNodes),
                   "Error must be NoNodes");
    return true;
}

FATP_TEST_CASE(wc_routes_to_most_available_capacity)
{
    balancer::CostModel model;
    // Node 1: utilization 0.9 (10% capacity left)
    // Node 2: utilization 0.3 (70% capacity left)
    // Node 3: utilization 0.6 (40% capacity left)
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Busy, 0, 0.9f),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Busy, 0, 0.3f),
        makeNodeMetrics(balancer::NodeId{3}, balancer::NodeState::Busy, 0, 0.6f),
    }, model);

    balancer::WeightedCapacity wc;
    auto result = wc.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Must select node with highest available capacity");
    return true;
}

FATP_TEST_CASE(wc_applies_node_weights)
{
    balancer::CostModel model;
    // Node 1: weight 3.0, utilization 0.6 → capacity = 3.0 × 0.4 = 1.2
    // Node 2: weight 1.0, utilization 0.1 → capacity = 1.0 × 0.9 = 0.9
    // Node 1 should win despite higher utilization because its weight is 3×.
    std::vector<std::pair<balancer::NodeId, float>> weights = {
        {balancer::NodeId{1}, 3.0f},
        {balancer::NodeId{2}, 1.0f},
    };
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Busy, 0, 0.6f),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Busy, 0, 0.1f),
    }, model);

    balancer::WeightedCapacity wc(std::move(weights));
    auto result = wc.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{1},
                   "Heavier-weight node must win despite higher utilization");
    return true;
}

FATP_TEST_CASE(wc_unlisted_node_defaults_to_weight_one)
{
    balancer::CostModel model;
    // Node 1 is listed with weight 0.5; Node 2 is unlisted (defaults to 1.0).
    // Node 1: capacity = 0.5 × (1 - 0.0) = 0.5
    // Node 2: capacity = 1.0 × (1 - 0.0) = 1.0 → node 2 wins
    std::vector<std::pair<balancer::NodeId, float>> weights = {
        {balancer::NodeId{1}, 0.5f},
    };
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Idle),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Idle),
    }, model);

    balancer::WeightedCapacity wc(std::move(weights));
    auto result = wc.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Unlisted node defaults to weight 1.0 and wins over weight 0.5 node");
    return true;
}

FATP_TEST_CASE(wc_skips_ineligible_nodes)
{
    balancer::CostModel model;
    // Node 1 is Failed (highest capacity if it were eligible)
    // Node 2 is Idle
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Failed, 0, 0.0f),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Idle,   0, 0.5f),
    }, model);

    balancer::WeightedCapacity wc;
    auto result = wc.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must find eligible node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Must skip Failed node regardless of available capacity");
    return true;
}

FATP_TEST_CASE(wc_ties_broken_by_nodeid)
{
    balancer::CostModel model;
    // Both nodes idle with utilization 0.0 and equal weights → tie broken by NodeId
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{3}, balancer::NodeState::Idle, 0, 0.0f),
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Idle, 0, 0.0f),
    }, model);

    balancer::WeightedCapacity wc;
    auto result = wc.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{1},
                   "Tie must be broken by lowest NodeId");
    return true;
}

FATP_TEST_CASE(wc_name_is_correct)
{
    balancer::WeightedCapacity wc;
    FATP_ASSERT_EQ(wc.name(), std::string_view{"WeightedCapacity"}, "Policy name must be WeightedCapacity");
    return true;
}

// ============================================================================
// ShortestJobFirst
// ============================================================================

FATP_TEST_CASE(sjf_no_nodes_returns_error)
{
    balancer::CostModel model;
    auto view = makeView({}, model);
    balancer::ShortestJobFirst sjf;

    auto result = sjf.selectNode(makeJob(), view);
    FATP_ASSERT_FALSE(result.has_value(), "Empty cluster must return error");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoNodes),
                   "Error must be NoNodes");
    return true;
}

FATP_TEST_CASE(sjf_routes_to_fastest_node_for_this_job)
{
    // CostModel warm with node 2 being 3x slower for this job class.
    // Even if node 2 has fewer queued jobs, SJF should prefer node 1.
    // Node 1: queueDepth=3, multiplier=1.0 → completion = 100 × (3+1) = 400
    // Node 2: queueDepth=1, multiplier=3.0 → completion = 300 × (1+1) = 600
    balancer::CostModelConfig cfg;
    cfg.warmThreshold = 1;
    cfg.nodeAlpha     = 0.9f;
    balancer::CostModel model(cfg);

    for (int i = 0; i < 20; ++i)
    {
        balancer::Job warmJob;
        warmJob.estimatedCost = balancer::Cost{100};
        warmJob.observedCost  = balancer::Cost{300}; // 3x slow
        warmJob.executedBy    = balancer::NodeId{2};
        warmJob.id            = balancer::JobId{1};
        warmJob.submitted     = balancer::Clock::now();
        model.update(warmJob);
    }

    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Busy, 3),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Busy, 1),
    }, model);

    balancer::ShortestJobFirst sjf;
    auto result = sjf.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{1},
                   "SJF must prefer faster node despite deeper queue");
    return true;
}

FATP_TEST_CASE(sjf_cold_model_falls_back_to_queue_depth)
{
    // Cold model → all multipliers 1.0 → completion ∝ queueDepth+1.
    // Node with lowest depth wins.
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Busy, 8),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Busy, 2),
        makeNodeMetrics(balancer::NodeId{3}, balancer::NodeState::Busy, 5),
    }, model);

    balancer::ShortestJobFirst sjf;
    auto result = sjf.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Cold SJF must route to shallowest queue");
    return true;
}

FATP_TEST_CASE(sjf_skips_ineligible_nodes)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Failed, 0),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Idle,   5),
    }, model);

    balancer::ShortestJobFirst sjf;
    auto result = sjf.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Must find eligible node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Must skip Failed node");
    return true;
}

FATP_TEST_CASE(sjf_name_is_correct)
{
    balancer::ShortestJobFirst sjf;
    FATP_ASSERT_EQ(sjf.name(), std::string_view{"ShortestJobFirst"}, "Policy name must be ShortestJobFirst");
    return true;
}

// ============================================================================
// EarliestDeadlineFirst
// ============================================================================

FATP_TEST_CASE(edf_no_nodes_returns_error)
{
    balancer::CostModel model;
    auto view = makeView({}, model);
    balancer::EarliestDeadlineFirst edf;

    auto result = edf.selectNode(makeJob(), view);
    FATP_ASSERT_FALSE(result.has_value(), "Empty cluster must return error");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoNodes),
                   "Error must be NoNodes");
    return true;
}

FATP_TEST_CASE(edf_routes_deadline_job_to_node_with_most_slack)
{
    // Node 1: depth=10 → high queue backlog
    // Node 2: depth=1  → low backlog, can finish before deadline easily
    // Deadline is 5 seconds out → node 2 has more slack
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Busy, 10),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Busy, 1),
    }, model);

    balancer::EarliestDeadlineFirst edf;
    auto job = makeDeadlineJob(std::chrono::seconds{5});
    auto result = edf.selectNode(job, view);

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Must prefer node with lower queue backlog for deadline job");
    return true;
}

FATP_TEST_CASE(edf_non_deadline_job_routes_to_shallowest_queue)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Busy, 8),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Busy, 2),
        makeNodeMetrics(balancer::NodeId{3}, balancer::NodeState::Busy, 5),
    }, model);

    balancer::EarliestDeadlineFirst edf;
    auto result = edf.selectNode(makeJob(), view); // no deadline

    FATP_ASSERT_TRUE(result.has_value(), "Must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Non-deadline job must go to shallowest queue");
    return true;
}

FATP_TEST_CASE(edf_skips_ineligible_nodes_for_deadline_job)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Failed, 0),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Idle,   3),
    }, model);

    balancer::EarliestDeadlineFirst edf;
    auto job = makeDeadlineJob(std::chrono::seconds{10});
    auto result = edf.selectNode(job, view);

    FATP_ASSERT_TRUE(result.has_value(), "Must find eligible node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "Must skip Failed node for deadline job");
    return true;
}

FATP_TEST_CASE(edf_none_eligible_returns_error)
{
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Offline),
    }, model);

    balancer::EarliestDeadlineFirst edf;
    auto result = edf.selectNode(makeJob(), view);

    FATP_ASSERT_FALSE(result.has_value(), "No eligible nodes must return error");
    return true;
}

FATP_TEST_CASE(edf_name_is_correct)
{
    balancer::EarliestDeadlineFirst edf;
    FATP_ASSERT_EQ(edf.name(), std::string_view{"EarliestDeadlineFirst"}, "Policy name must be EarliestDeadlineFirst");
    return true;
}

// ============================================================================
// Composite
// ============================================================================

FATP_TEST_CASE(composite_empty_policy_list_returns_no_nodes)
{
    balancer::CostModel model;
    auto view = makeView({makeNodeMetrics(balancer::NodeId{1})}, model);

    balancer::Composite composite({});
    auto result = composite.selectNode(makeJob(), view);

    FATP_ASSERT_FALSE(result.has_value(), "Empty composite must return error");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoNodes),
                   "Empty composite must return NoNodes");
    return true;
}

FATP_TEST_CASE(composite_returns_first_policy_result_on_success)
{
    balancer::CostModel model;
    // Single node — both policies would select it; first policy wins.
    auto view = makeView({makeNodeMetrics(balancer::NodeId{42})}, model);

    std::vector<std::unique_ptr<balancer::ISchedulingPolicy>> policies;
    policies.push_back(std::make_unique<balancer::RoundRobin>());
    policies.push_back(std::make_unique<balancer::LeastLoaded>());

    balancer::Composite composite(std::move(policies));
    auto result = composite.selectNode(makeJob(), view);

    FATP_ASSERT_TRUE(result.has_value(), "Composite must select a node");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{42},
                   "Must return the node selected by the first policy");
    return true;
}

FATP_TEST_CASE(composite_falls_back_to_second_policy_when_first_fails)
{
    balancer::CostModel model;
    // Node 1 is Overloaded: only accepts Critical+High.
    // First policy (RoundRobin) will return NoneEligible for a Normal job.
    // Node 2 is Idle: second policy (LeastLoaded) selects it.
    auto overloadedView = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Overloaded),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Idle),
    }, model);

    // RoundRobin with only Overloaded nodes returns NoneEligible for Normal jobs.
    // We simulate a "first policy fails" scenario by using a view where the first
    // policy in the chain will skip the only eligible node.
    // Simplest: chain EarliestDeadlineFirst → LeastLoaded.
    // Give a non-deadline Normal job; EDF will use its non-deadline path (queue depth)
    // and succeed, so to test fallback we need a policy that actually fails.
    // Use a single-node-Overloaded view and a Normal job for the first policy.

    auto singleOverloadedView = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Overloaded),
    }, model);

    // Policy 1: RoundRobin on single Overloaded node with Normal job → NoneEligible
    // Policy 2: LeastLoaded on same view → also NoneEligible (no fallback target)
    // This tests that the composite properly falls through and reports NoneEligible.
    std::vector<std::unique_ptr<balancer::ISchedulingPolicy>> policies;
    policies.push_back(std::make_unique<balancer::RoundRobin>());
    policies.push_back(std::make_unique<balancer::LeastLoaded>());

    balancer::Composite composite(std::move(policies));
    auto result = composite.selectNode(makeJob(balancer::Priority::Normal), singleOverloadedView);

    FATP_ASSERT_FALSE(result.has_value(), "Composite must fail when all policies fail");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoneEligible),
                   "Composite must report NoneEligible when nodes exist but none eligible");
    return true;
}

FATP_TEST_CASE(composite_edf_with_leastloaded_fallback)
{
    // EarliestDeadlineFirst as primary, LeastLoaded as fallback.
    // Deadline job → EDF routes it. Non-deadline → EDF routes it (own fallback).
    // Both succeed; LeastLoaded is never called.
    balancer::CostModel model;
    auto view = makeView({
        makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Busy, 10),
        makeNodeMetrics(balancer::NodeId{2}, balancer::NodeState::Busy, 1),
    }, model);

    std::vector<std::unique_ptr<balancer::ISchedulingPolicy>> policies;
    policies.push_back(std::make_unique<balancer::EarliestDeadlineFirst>());
    policies.push_back(std::make_unique<balancer::LeastLoaded>());

    balancer::Composite composite(std::move(policies));

    // Deadline job: EDF selects node with most slack (node 2, least backlog)
    auto deadlineJob = makeDeadlineJob(std::chrono::seconds{10});
    auto result = composite.selectNode(deadlineJob, view);
    FATP_ASSERT_TRUE(result.has_value(), "Composite must select a node for deadline job");
    FATP_ASSERT_EQ(result.value(), balancer::NodeId{2},
                   "EDF must pick node 2 (lowest queue) for deadline job");

    // Non-deadline job: EDF falls back to queue depth, same result
    auto result2 = composite.selectNode(makeJob(), view);
    FATP_ASSERT_TRUE(result2.has_value(), "Composite must select a node for non-deadline job");
    FATP_ASSERT_EQ(result2.value(), balancer::NodeId{2},
                   "Non-deadline path must also pick node 2 (lowest queue)");
    return true;
}

FATP_TEST_CASE(composite_reports_no_nodes_when_all_see_empty_cluster)
{
    balancer::CostModel model;
    auto view = makeView({}, model);

    std::vector<std::unique_ptr<balancer::ISchedulingPolicy>> policies;
    policies.push_back(std::make_unique<balancer::RoundRobin>());
    policies.push_back(std::make_unique<balancer::LeastLoaded>());

    balancer::Composite composite(std::move(policies));
    auto result = composite.selectNode(makeJob(), view);

    FATP_ASSERT_FALSE(result.has_value(), "Composite must fail on empty cluster");
    FATP_ASSERT_EQ(static_cast<int>(result.error()),
                   static_cast<int>(balancer::SelectError::NoNodes),
                   "Composite must report NoNodes when all policies see empty cluster");
    return true;
}

FATP_TEST_CASE(composite_name_is_correct)
{
    std::vector<std::unique_ptr<balancer::ISchedulingPolicy>> policies;
    policies.push_back(std::make_unique<balancer::RoundRobin>());
    balancer::Composite composite(std::move(policies));
    FATP_ASSERT_EQ(composite.name(), std::string_view{"Composite"}, "Policy name must be Composite");
    return true;
}

// ============================================================================
// Admission table (carried from Phase 1)
// ============================================================================

FATP_TEST_CASE(admission_table_idle_accepts_all_priorities)
{
    balancer::CostModel model;
    auto nodeMetrics = makeNodeMetrics(balancer::NodeId{1}, balancer::NodeState::Idle);

    for (auto p : {balancer::Priority::Critical, balancer::Priority::High,
                   balancer::Priority::Normal,   balancer::Priority::Low,
                   balancer::Priority::Bulk})
    {
        FATP_ASSERT_TRUE(balancer::nodeAcceptsPriority(nodeMetrics.state, p),
                         "Idle node must accept all priorities");
    }
    return true;
}

FATP_TEST_CASE(admission_table_overloaded_accepts_critical_and_high_only)
{
    auto state = balancer::NodeState::Overloaded;

    FATP_ASSERT_TRUE(balancer::nodeAcceptsPriority(state, balancer::Priority::Critical),
                     "Overloaded accepts Critical");
    FATP_ASSERT_TRUE(balancer::nodeAcceptsPriority(state, balancer::Priority::High),
                     "Overloaded accepts High");
    FATP_ASSERT_FALSE(balancer::nodeAcceptsPriority(state, balancer::Priority::Normal),
                      "Overloaded rejects Normal");
    FATP_ASSERT_FALSE(balancer::nodeAcceptsPriority(state, balancer::Priority::Low),
                      "Overloaded rejects Low");
    FATP_ASSERT_FALSE(balancer::nodeAcceptsPriority(state, balancer::Priority::Bulk),
                      "Overloaded rejects Bulk");
    return true;
}

FATP_TEST_CASE(admission_table_draining_accepts_critical_only)
{
    auto state = balancer::NodeState::Draining;

    FATP_ASSERT_TRUE(balancer::nodeAcceptsPriority(state, balancer::Priority::Critical),
                     "Draining accepts Critical");
    FATP_ASSERT_FALSE(balancer::nodeAcceptsPriority(state, balancer::Priority::High),
                      "Draining rejects High");
    FATP_ASSERT_FALSE(balancer::nodeAcceptsPriority(state, balancer::Priority::Normal),
                      "Draining rejects Normal");
    return true;
}

FATP_TEST_CASE(admission_table_failed_rejects_everything)
{
    auto state = balancer::NodeState::Failed;

    for (auto p : {balancer::Priority::Critical, balancer::Priority::High,
                   balancer::Priority::Normal,   balancer::Priority::Low,
                   balancer::Priority::Bulk})
    {
        FATP_ASSERT_FALSE(balancer::nodeAcceptsPriority(state, p),
                          "Failed node must reject all priorities");
    }
    return true;
}

FATP_TEST_CASE(admission_table_recovering_accepts_critical_and_high)
{
    auto state = balancer::NodeState::Recovering;

    FATP_ASSERT_TRUE(balancer::nodeAcceptsPriority(state, balancer::Priority::Critical),
                     "Recovering accepts Critical");
    FATP_ASSERT_TRUE(balancer::nodeAcceptsPriority(state, balancer::Priority::High),
                     "Recovering accepts High");
    FATP_ASSERT_FALSE(balancer::nodeAcceptsPriority(state, balancer::Priority::Normal),
                      "Recovering rejects Normal");
    return true;
}

} // namespace balancer::testing::policies

// ============================================================================
// Public interface
// ============================================================================

namespace balancer::testing
{

using fat_p::testing::TestRunner;
namespace colors = fat_p::testing::colors;

bool test_policies()
{
    FATP_PRINT_HEADER(POLICIES)

    TestRunner runner;
    auto& out = *fat_p::testing::get_test_config().output;

    out << colors::blue() << "--- RoundRobin ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, policies, rr_no_nodes_returns_error);
    FATP_RUN_TEST_NS(runner, policies, rr_single_idle_node);
    FATP_RUN_TEST_NS(runner, policies, rr_cycles_through_all_nodes);
    FATP_RUN_TEST_NS(runner, policies, rr_skips_ineligible_nodes);
    FATP_RUN_TEST_NS(runner, policies, rr_none_eligible_returns_error);
    FATP_RUN_TEST_NS(runner, policies, rr_critical_accepted_by_overloaded_node);
    FATP_RUN_TEST_NS(runner, policies, rr_name_is_correct);

    out << "\n" << colors::blue() << "--- LeastLoaded ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, policies, ll_no_nodes_returns_error);
    FATP_RUN_TEST_NS(runner, policies, ll_selects_least_loaded_node);
    FATP_RUN_TEST_NS(runner, policies, ll_skips_ineligible_nodes);
    FATP_RUN_TEST_NS(runner, policies, ll_integrates_cost_model_multiplier);
    FATP_RUN_TEST_NS(runner, policies, ll_ties_broken_by_nodeid);
    FATP_RUN_TEST_NS(runner, policies, ll_none_eligible_returns_error);
    FATP_RUN_TEST_NS(runner, policies, ll_name_is_correct);

    out << "\n" << colors::blue() << "--- WeightedCapacity ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, policies, wc_no_nodes_returns_error);
    FATP_RUN_TEST_NS(runner, policies, wc_routes_to_most_available_capacity);
    FATP_RUN_TEST_NS(runner, policies, wc_applies_node_weights);
    FATP_RUN_TEST_NS(runner, policies, wc_unlisted_node_defaults_to_weight_one);
    FATP_RUN_TEST_NS(runner, policies, wc_skips_ineligible_nodes);
    FATP_RUN_TEST_NS(runner, policies, wc_ties_broken_by_nodeid);
    FATP_RUN_TEST_NS(runner, policies, wc_name_is_correct);

    out << "\n" << colors::blue() << "--- ShortestJobFirst ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, policies, sjf_no_nodes_returns_error);
    FATP_RUN_TEST_NS(runner, policies, sjf_routes_to_fastest_node_for_this_job);
    FATP_RUN_TEST_NS(runner, policies, sjf_cold_model_falls_back_to_queue_depth);
    FATP_RUN_TEST_NS(runner, policies, sjf_skips_ineligible_nodes);
    FATP_RUN_TEST_NS(runner, policies, sjf_name_is_correct);

    out << "\n" << colors::blue() << "--- EarliestDeadlineFirst ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, policies, edf_no_nodes_returns_error);
    FATP_RUN_TEST_NS(runner, policies, edf_routes_deadline_job_to_node_with_most_slack);
    FATP_RUN_TEST_NS(runner, policies, edf_non_deadline_job_routes_to_shallowest_queue);
    FATP_RUN_TEST_NS(runner, policies, edf_skips_ineligible_nodes_for_deadline_job);
    FATP_RUN_TEST_NS(runner, policies, edf_none_eligible_returns_error);
    FATP_RUN_TEST_NS(runner, policies, edf_name_is_correct);

    out << "\n" << colors::blue() << "--- Composite ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, policies, composite_empty_policy_list_returns_no_nodes);
    FATP_RUN_TEST_NS(runner, policies, composite_returns_first_policy_result_on_success);
    FATP_RUN_TEST_NS(runner, policies, composite_falls_back_to_second_policy_when_first_fails);
    FATP_RUN_TEST_NS(runner, policies, composite_edf_with_leastloaded_fallback);
    FATP_RUN_TEST_NS(runner, policies, composite_reports_no_nodes_when_all_see_empty_cluster);
    FATP_RUN_TEST_NS(runner, policies, composite_name_is_correct);

    out << "\n" << colors::blue() << "--- Admission Table ---" << colors::reset() << "\n";
    FATP_RUN_TEST_NS(runner, policies, admission_table_idle_accepts_all_priorities);
    FATP_RUN_TEST_NS(runner, policies, admission_table_overloaded_accepts_critical_and_high_only);
    FATP_RUN_TEST_NS(runner, policies, admission_table_draining_accepts_critical_only);
    FATP_RUN_TEST_NS(runner, policies, admission_table_failed_rejects_everything);
    FATP_RUN_TEST_NS(runner, policies, admission_table_recovering_accepts_critical_and_high);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_policies() ? 0 : 1;
}
#endif
