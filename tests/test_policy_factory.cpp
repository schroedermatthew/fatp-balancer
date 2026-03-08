/*
BALANCER_META:
  meta_version: 1
  component: PolicyFactory
  file_role: test
  path: tests/test_policy_factory.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    Functional tests for PolicyFactory::makePolicy(). Covers all policy names,
    AffinityRouting CostModel wiring, CompositePolicyConfig chain assembly,
    composite recursion, unknown-name fallback, and empty-chain degenerate case.
  api_stability: internal
  related:
    headers:
      - include/balancer/PolicyFactory.h
      - include/balancer/BalancerConfig.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

#include "FatPTest.h"

#include "balancer/BalancerConfig.h"
#include "balancer/BalancerFeatures.h"
#include "balancer/CostModel.h"
#include "balancer/PolicyFactory.h"

using fat_p::testing::TestRunner;

namespace balancer::testing::pfns
{

FATP_TEST_CASE(round_robin_by_name)
{
    auto p = makePolicy(features::kPolicyRoundRobin);
    FATP_ASSERT_TRUE(p != nullptr, "not null");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("RoundRobin"), "name");
    return true;
}

FATP_TEST_CASE(least_loaded_by_name)
{
    auto p = makePolicy(features::kPolicyLeastLoaded);
    FATP_ASSERT_TRUE(p != nullptr, "not null");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("LeastLoaded"), "name");
    return true;
}

FATP_TEST_CASE(work_stealing_by_name)
{
    auto p = makePolicy(features::kPolicyWorkStealing);
    FATP_ASSERT_TRUE(p != nullptr, "not null");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("WorkStealing"), "name");
    return true;
}

FATP_TEST_CASE(affinity_routing_with_cost_model)
{
    CostModelConfig cfg;
    CostModel cm(cfg);

    auto p = makePolicy(features::kPolicyAffinity, &cm);
    FATP_ASSERT_TRUE(p != nullptr, "not null with CostModel");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("AffinityRouting"),
        "AffinityRouting name when CostModel supplied");
    return true;
}

FATP_TEST_CASE(affinity_routing_null_cost_model_falls_back)
{
    auto p = makePolicy(features::kPolicyAffinity, nullptr);
    FATP_ASSERT_TRUE(p != nullptr, "not null");
    // Must fall back to RoundRobin when no CostModel is provided.
    FATP_ASSERT_EQ(std::string(p->name()), std::string("RoundRobin"),
        "falls back to RoundRobin without CostModel");
    return true;
}

FATP_TEST_CASE(composite_default_chain)
{
    // Default CompositePolicyConfig: EarliestDeadlineFirst → LeastLoaded.
    auto p = makePolicy(features::kPolicyComposite);
    FATP_ASSERT_TRUE(p != nullptr, "Composite not null");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("Composite"),
        "Composite policy name");
    return true;
}

FATP_TEST_CASE(composite_custom_single_child)
{
    CompositePolicyConfig cfg;
    cfg.chain = {std::string(features::kPolicyWorkStealing)};

    auto p = makePolicy(features::kPolicyComposite, nullptr, cfg);
    FATP_ASSERT_TRUE(p != nullptr, "single-child Composite not null");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("Composite"), "name");
    return true;
}

FATP_TEST_CASE(composite_custom_multi_child)
{
    CompositePolicyConfig cfg;
    cfg.chain = {
        std::string(features::kPolicyRoundRobin),
        std::string(features::kPolicyLeastLoaded),
        std::string(features::kPolicyWorkStealing),
    };

    auto p = makePolicy(features::kPolicyComposite, nullptr, cfg);
    FATP_ASSERT_TRUE(p != nullptr, "multi-child Composite not null");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("Composite"), "name");
    return true;
}

FATP_TEST_CASE(composite_empty_chain_falls_back_to_round_robin)
{
    CompositePolicyConfig cfg;
    cfg.chain = {};  // Empty — degenerate case.

    auto p = makePolicy(features::kPolicyComposite, nullptr, cfg);
    FATP_ASSERT_TRUE(p != nullptr, "degenerate Composite not null");
    // Empty chain → falls through to RoundRobin default.
    FATP_ASSERT_EQ(std::string(p->name()), std::string("RoundRobin"),
        "empty composite chain → RoundRobin fallback");
    return true;
}

FATP_TEST_CASE(string_aliases_for_non_feature_graph_policies)
{
    auto sjf = makePolicy("shortest_job_first");
    auto wc  = makePolicy("weighted_capacity");
    auto edf = makePolicy("earliest_deadline_first");

    FATP_ASSERT_TRUE(sjf != nullptr, "ShortestJobFirst not null");
    FATP_ASSERT_TRUE(wc  != nullptr, "WeightedCapacity not null");
    FATP_ASSERT_TRUE(edf != nullptr, "EarliestDeadlineFirst not null");

    FATP_ASSERT_EQ(std::string(sjf->name()), std::string("ShortestJobFirst"),    "SJF name");
    FATP_ASSERT_EQ(std::string(wc->name()),  std::string("WeightedCapacity"),    "WC name");
    FATP_ASSERT_EQ(std::string(edf->name()), std::string("EarliestDeadlineFirst"), "EDF name");
    return true;
}

FATP_TEST_CASE(unknown_name_yields_round_robin)
{
    auto p = makePolicy("this_is_not_a_policy");
    FATP_ASSERT_TRUE(p != nullptr, "not null");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("RoundRobin"),
        "unknown name falls back to RoundRobin");
    return true;
}

FATP_TEST_CASE(each_call_returns_distinct_object)
{
    auto p1 = makePolicy(features::kPolicyRoundRobin);
    auto p2 = makePolicy(features::kPolicyRoundRobin);
    FATP_ASSERT_TRUE(p1.get() != p2.get(),
        "each makePolicy() call returns a distinct object");
    return true;
}

FATP_TEST_CASE(composite_with_affinity_child_and_cost_model)
{
    CostModelConfig cmCfg;
    CostModel cm(cmCfg);

    CompositePolicyConfig cfg;
    cfg.chain = {
        std::string(features::kPolicyAffinity),
        std::string(features::kPolicyLeastLoaded),
    };

    auto p = makePolicy(features::kPolicyComposite, &cm, cfg);
    FATP_ASSERT_TRUE(p != nullptr, "Composite with AffinityRouting child not null");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("Composite"), "name");
    return true;
}

} // namespace balancer::testing::pfns

namespace balancer::testing
{

bool test_policy_factory()
{
    FATP_PRINT_HEADER(POLICY FACTORY)
    TestRunner runner;

    FATP_RUN_TEST_NS(runner, pfns, round_robin_by_name);
    FATP_RUN_TEST_NS(runner, pfns, least_loaded_by_name);
    FATP_RUN_TEST_NS(runner, pfns, work_stealing_by_name);
    FATP_RUN_TEST_NS(runner, pfns, affinity_routing_with_cost_model);
    FATP_RUN_TEST_NS(runner, pfns, affinity_routing_null_cost_model_falls_back);
    FATP_RUN_TEST_NS(runner, pfns, composite_default_chain);
    FATP_RUN_TEST_NS(runner, pfns, composite_custom_single_child);
    FATP_RUN_TEST_NS(runner, pfns, composite_custom_multi_child);
    FATP_RUN_TEST_NS(runner, pfns, composite_empty_chain_falls_back_to_round_robin);
    FATP_RUN_TEST_NS(runner, pfns, string_aliases_for_non_feature_graph_policies);
    FATP_RUN_TEST_NS(runner, pfns, unknown_name_yields_round_robin);
    FATP_RUN_TEST_NS(runner, pfns, each_call_returns_distinct_object);
    FATP_RUN_TEST_NS(runner, pfns, composite_with_affinity_child_and_cost_model);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main() { return balancer::testing::test_policy_factory() ? 0 : 1; }
#endif
