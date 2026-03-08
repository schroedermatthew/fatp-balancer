/*
BALANCER_META:
  meta_version: 1
  component: PolicyFactory
  file_role: test
  path: tests/test_PolicyFactory_HeaderSelfContained.cpp
  namespace: balancer::testing
  layer: Testing
  summary: Self-contained include verification for PolicyFactory.h.
  api_stability: internal
  related:
    headers:
      - include/balancer/PolicyFactory.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

#include "balancer/PolicyFactory.h"

#include "FatPTest.h"

using fat_p::testing::TestRunner;

namespace balancer::testing::pfhscns
{

FATP_TEST_CASE(policy_factory_header_self_contained)
{
    // makePolicy() returns a non-null policy for every canonical name.
    auto rr  = makePolicy(features::kPolicyRoundRobin);
    auto ll  = makePolicy(features::kPolicyLeastLoaded);
    auto ws  = makePolicy(features::kPolicyWorkStealing);
    auto aff = makePolicy(features::kPolicyAffinity);   // no CostModel → RoundRobin fallback

    FATP_ASSERT_TRUE(rr  != nullptr, "RoundRobin must not be null");
    FATP_ASSERT_TRUE(ll  != nullptr, "LeastLoaded must not be null");
    FATP_ASSERT_TRUE(ws  != nullptr, "WorkStealing must not be null");
    FATP_ASSERT_TRUE(aff != nullptr, "affinity fallback must not be null");

    FATP_ASSERT_EQ(std::string(rr->name()),  std::string("RoundRobin"),   "RoundRobin name");
    FATP_ASSERT_EQ(std::string(ll->name()),  std::string("LeastLoaded"),  "LeastLoaded name");
    FATP_ASSERT_EQ(std::string(ws->name()),  std::string("WorkStealing"), "WorkStealing name");

    return true;
}

FATP_TEST_CASE(composite_default_chain_is_non_null)
{
    auto comp = makePolicy(features::kPolicyComposite);
    FATP_ASSERT_TRUE(comp != nullptr, "Composite must not be null");
    FATP_ASSERT_EQ(std::string(comp->name()), std::string("Composite"), "Composite name");
    return true;
}

FATP_TEST_CASE(unknown_name_returns_round_robin)
{
    auto p = makePolicy("no_such_policy");
    FATP_ASSERT_TRUE(p != nullptr, "unknown name must return non-null");
    FATP_ASSERT_EQ(std::string(p->name()), std::string("RoundRobin"), "unknown → RoundRobin");
    return true;
}

} // namespace balancer::testing::pfhscns

namespace balancer::testing
{

bool test_PolicyFactory_HeaderSelfContained()
{
    FATP_PRINT_HEADER(POLICY FACTORY HSC)
    TestRunner runner;
    FATP_RUN_TEST_NS(runner, pfhscns, policy_factory_header_self_contained);
    FATP_RUN_TEST_NS(runner, pfhscns, composite_default_chain_is_non_null);
    FATP_RUN_TEST_NS(runner, pfhscns, unknown_name_returns_round_robin);
    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main() { return balancer::testing::test_PolicyFactory_HeaderSelfContained() ? 0 : 1; }
#endif
