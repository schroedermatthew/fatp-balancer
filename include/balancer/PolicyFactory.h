#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: PolicyFactory
  file_role: public_header
  path: include/balancer/PolicyFactory.h
  namespace: balancer
  layer: Core
  summary: >
    Free function makePolicy() maps a feature-name string to a freshly
    constructed unique_ptr<ISchedulingPolicy>. Resolves DEBT-003: the only
    prior string-to-policy mapping lived inside wasm/BalancerBindings.cpp.
    FeatureSupervisor delegates to this factory; BalancerBindings.cpp can do
    the same. CompositeConfig (in BalancerConfig.h) drives the default child
    chain when "composite" is requested.
  api_stability: in_work
  related:
    docs_search: "PolicyFactory"
    tests:
      - tests/test_PolicyFactory_HeaderSelfContained.cpp
      - tests/test_policy_factory.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file PolicyFactory.h
 * @brief String-to-policy factory for the balancer policy hierarchy.
 *
 * ## Usage
 *
 * ```cpp
 * // Default — RoundRobin, no CostModel needed
 * auto p = makePolicy(features::kPolicyRoundRobin);
 *
 * // AffinityRouting — pass a CostModel reference
 * auto p = makePolicy(features::kPolicyAffinity, &balancer.costModel());
 *
 * // Composite — driven by CompositePolicyConfig from BalancerConfig.h
 * CompositePolicyConfig cfg;
 * cfg.chain = {features::kPolicyWorkStealing, features::kPolicyLeastLoaded};
 * auto p = makePolicy(features::kPolicyComposite, nullptr, cfg);
 * ```
 *
 * ## Recognised names (via BalancerFeatures constants)
 *
 * | Constant                    | Policy              | Needs CostModel? |
 * |-----------------------------|---------------------|------------------|
 * | `kPolicyRoundRobin`         | RoundRobin          | No               |
 * | `kPolicyLeastLoaded`        | LeastLoaded         | No               |
 * | `kPolicyWorkStealing`       | WorkStealing        | No               |
 * | `kPolicyAffinity`           | AffinityRouting     | **Yes**          |
 * | `kPolicyComposite`          | Composite           | No (children may)|
 * | `kPolicyShortestJobFirst`*  | ShortestJobFirst    | No               |
 * | `kPolicyWeightedCapacity`*  | WeightedCapacity    | No               |
 * | `kPolicyEDF`*               | EarliestDeadlineFirst | No             |
 *
 * *These names are not defined in BalancerFeatures (they have no feature-graph
 * entry) but are accepted by makePolicy() for use in CompositeConfig chains.
 *
 * ## AffinityRouting + null CostModel
 *
 * If `kPolicyAffinity` is requested and @p costModel is nullptr, makePolicy()
 * falls back to RoundRobin and the returned policy's name reflects this.
 * Callers that know they need AffinityRouting must supply a valid CostModel*.
 *
 * ## Composite recursion
 *
 * Child policies in CompositePolicyConfig::chain are resolved with a null
 * costModel (they receive the same costModel pointer as the parent call). To
 * use AffinityRouting as a Composite child, include the CostModel in the
 * parent call.
 *
 * ## Transferability
 *
 * This header does not include any `sim/` header. It ships unchanged to
 * production and WASM bindings.
 */

#include <memory>
#include <string_view>

#include "balancer/BalancerConfig.h"
#include "balancer/BalancerFeatures.h"
#include "balancer/CostModel.h"
#include "balancer/ISchedulingPolicy.h"

#include "policies/AffinityRouting.h"
#include "policies/Composite.h"
#include "policies/EarliestDeadlineFirst.h"
#include "policies/LeastLoaded.h"
#include "policies/RoundRobin.h"
#include "policies/ShortestJobFirst.h"
#include "policies/WeightedCapacity.h"
#include "policies/WorkStealing.h"

namespace balancer
{

/**
 * @brief Construct a scheduling policy by feature name.
 *
 * Returns a freshly constructed `unique_ptr<ISchedulingPolicy>`. Unrecognised
 * names fall back to RoundRobin — this is a deliberate safe default rather
 * than an error, because FeatureSupervisor must never leave the Balancer
 * without a policy.
 *
 * @param name            Policy feature name (use `features::kPolicy*` constants).
 * @param costModel       Required for `kPolicyAffinity`; ignored for others.
 *                        May be nullptr — falls back to RoundRobin.
 * @param compositeConfig Drives the child chain when `kPolicyComposite` is
 *                        requested. Defaults to the CompositePolicyConfig
 *                        defaults (EarliestDeadlineFirst → LeastLoaded).
 * @return                Freshly-constructed policy. Never null.
 */
[[nodiscard]] inline std::unique_ptr<ISchedulingPolicy>
makePolicy(std::string_view          name,
           const CostModel*          costModel       = nullptr,
           const CompositePolicyConfig& compositeConfig = {})
{
    using namespace features;

    if (name == kPolicyLeastLoaded)
        return std::make_unique<LeastLoaded>();

    if (name == kPolicyWorkStealing)
        return std::make_unique<WorkStealing>();

    if (name == kPolicyAffinity)
    {
        // AffinityRouting requires a live CostModel. If none is supplied,
        // fall through to the default (RoundRobin) with a clear name.
        if (costModel != nullptr)
            return std::make_unique<AffinityRouting>(*costModel);
        // Fall through — no CostModel available.
    }

    if (name == kPolicyComposite)
    {
        std::vector<std::unique_ptr<ISchedulingPolicy>> children;
        children.reserve(compositeConfig.chain.size());

        for (const auto& childName : compositeConfig.chain)
        {
            // Recurse: children that need AffinityRouting receive the same
            // costModel pointer as the parent call.
            children.push_back(makePolicy(childName, costModel, {}));
        }

        if (!children.empty())
            return std::make_unique<Composite>(std::move(children));

        // Empty chain — degenerate Composite; fall through to RoundRobin.
    }

    // Named alternatives that exist as policies but have no BalancerFeatures
    // constant (usable in CompositePolicyConfig chains).
    if (name == "shortest_job_first")
        return std::make_unique<ShortestJobFirst>();

    if (name == "weighted_capacity")
        return std::make_unique<WeightedCapacity>();

    if (name == "earliest_deadline_first")
        return std::make_unique<EarliestDeadlineFirst>();

    // kPolicyRoundRobin and any unrecognised name — safe default.
    return std::make_unique<RoundRobin>();
}

} // namespace balancer
