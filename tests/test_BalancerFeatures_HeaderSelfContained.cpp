/*
BALANCER_META:
  meta_version: 1
  component: BalancerFeatures
  file_role: test
  path: tests/test_BalancerFeatures_HeaderSelfContained.cpp
  namespace: balancer::testing
  layer: Testing
  summary: Self-contained include verification for BalancerFeatures.h — confirms no undeclared dependencies and validates all feature name constants.
  api_stability: internal
  related:
    headers:
      - include/balancer/BalancerFeatures.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

// Intentionally the only balancer header. If this translation unit compiles
// in isolation, BalancerFeatures.h has no undeclared dependencies.
#include "balancer/BalancerFeatures.h"

#include <iostream>
#include <string_view>

#include "FatPTest.h"

using fat_p::testing::TestRunner;

namespace balancer::testing::balancerfeatures
{

FATP_TEST_CASE(all_policy_names_are_non_empty)
{
    using namespace balancer::features;
    FATP_ASSERT_FALSE(kPolicyRoundRobin.empty(),   "kPolicyRoundRobin must not be empty");
    FATP_ASSERT_FALSE(kPolicyLeastLoaded.empty(),  "kPolicyLeastLoaded must not be empty");
    FATP_ASSERT_FALSE(kPolicyAffinity.empty(),     "kPolicyAffinity must not be empty");
    FATP_ASSERT_FALSE(kPolicyWorkStealing.empty(), "kPolicyWorkStealing must not be empty");
    FATP_ASSERT_FALSE(kPolicyComposite.empty(),    "kPolicyComposite must not be empty");
    return true;
}

FATP_TEST_CASE(all_behavioral_feature_names_are_non_empty)
{
    using namespace balancer::features;
    FATP_ASSERT_FALSE(kFeatureAffinityMatrix.empty(),     "kFeatureAffinityMatrix must not be empty");
    FATP_ASSERT_FALSE(kFeatureAffinityMatrixWarm.empty(), "kFeatureAffinityMatrixWarm must not be empty");
    FATP_ASSERT_FALSE(kFeatureAging.empty(),              "kFeatureAging must not be empty");
    FATP_ASSERT_FALSE(kFeatureDegradationCurve.empty(),   "kFeatureDegradationCurve must not be empty");
    return true;
}

FATP_TEST_CASE(all_admission_names_are_non_empty)
{
    using namespace balancer::features;
    FATP_ASSERT_FALSE(kAdmissionBulkShed.empty(), "kAdmissionBulkShed must not be empty");
    FATP_ASSERT_FALSE(kAdmissionStrict.empty(),   "kAdmissionStrict must not be empty");
    return true;
}

FATP_TEST_CASE(all_alert_names_are_non_empty)
{
    using namespace balancer::features;
    FATP_ASSERT_FALSE(kAlertOverload.empty(),  "kAlertOverload must not be empty");
    FATP_ASSERT_FALSE(kAlertLatency.empty(),   "kAlertLatency must not be empty");
    FATP_ASSERT_FALSE(kAlertNodeFault.empty(), "kAlertNodeFault must not be empty");
    return true;
}

FATP_TEST_CASE(all_names_have_correct_prefix)
{
    using namespace balancer::features;
    FATP_ASSERT_TRUE(kPolicyRoundRobin.starts_with("policy."),   "kPolicyRoundRobin must start with 'policy.'");
    FATP_ASSERT_TRUE(kPolicyLeastLoaded.starts_with("policy."),  "kPolicyLeastLoaded must start with 'policy.'");
    FATP_ASSERT_TRUE(kPolicyAffinity.starts_with("policy."),     "kPolicyAffinity must start with 'policy.'");
    FATP_ASSERT_TRUE(kPolicyWorkStealing.starts_with("policy."), "kPolicyWorkStealing must start with 'policy.'");
    FATP_ASSERT_TRUE(kPolicyComposite.starts_with("policy."),    "kPolicyComposite must start with 'policy.'");

    FATP_ASSERT_TRUE(kFeatureAffinityMatrix.starts_with("feature."),
                     "kFeatureAffinityMatrix must start with 'feature.'");
    FATP_ASSERT_TRUE(kFeatureAffinityMatrixWarm.starts_with("feature."),
                     "kFeatureAffinityMatrixWarm must start with 'feature.'");
    FATP_ASSERT_TRUE(kFeatureAging.starts_with("feature."),
                     "kFeatureAging must start with 'feature.'");
    FATP_ASSERT_TRUE(kFeatureDegradationCurve.starts_with("feature."),
                     "kFeatureDegradationCurve must start with 'feature.'");

    FATP_ASSERT_TRUE(kAdmissionBulkShed.starts_with("admission."),
                     "kAdmissionBulkShed must start with 'admission.'");
    FATP_ASSERT_TRUE(kAdmissionStrict.starts_with("admission."),
                     "kAdmissionStrict must start with 'admission.'");

    FATP_ASSERT_TRUE(kAlertOverload.starts_with("alert."),  "kAlertOverload must start with 'alert.'");
    FATP_ASSERT_TRUE(kAlertLatency.starts_with("alert."),   "kAlertLatency must start with 'alert.'");
    FATP_ASSERT_TRUE(kAlertNodeFault.starts_with("alert."), "kAlertNodeFault must start with 'alert.'");
    return true;
}

FATP_TEST_CASE(all_names_are_unique)
{
    using namespace balancer::features;
    const std::string_view names[] = {
        kPolicyRoundRobin, kPolicyLeastLoaded, kPolicyAffinity,
        kPolicyWorkStealing, kPolicyComposite,
        kFeatureAffinityMatrix, kFeatureAffinityMatrixWarm,
        kFeatureAging, kFeatureDegradationCurve,
        kAdmissionBulkShed, kAdmissionStrict,
        kAlertOverload, kAlertLatency, kAlertNodeFault
    };
    constexpr size_t kCount = sizeof(names) / sizeof(names[0]);

    for (size_t i = 0; i < kCount; ++i)
    {
        for (size_t j = i + 1; j < kCount; ++j)
        {
            FATP_ASSERT_NE(names[i], names[j], "Feature names must be unique");
        }
    }
    return true;
}

} // namespace balancer::testing::balancerfeatures

namespace balancer::testing
{

bool test_BalancerFeatures()
{
    FATP_PRINT_HEADER(BALANCER FEATURES)

    TestRunner runner;

    FATP_RUN_TEST_NS(runner, balancerfeatures, all_policy_names_are_non_empty);
    FATP_RUN_TEST_NS(runner, balancerfeatures, all_behavioral_feature_names_are_non_empty);
    FATP_RUN_TEST_NS(runner, balancerfeatures, all_admission_names_are_non_empty);
    FATP_RUN_TEST_NS(runner, balancerfeatures, all_alert_names_are_non_empty);
    FATP_RUN_TEST_NS(runner, balancerfeatures, all_names_have_correct_prefix);
    FATP_RUN_TEST_NS(runner, balancerfeatures, all_names_are_unique);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_BalancerFeatures() ? 0 : 1;
}
#endif
