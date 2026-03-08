#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: Composite
  file_role: public_header
  path: policies/Composite.h
  namespace: balancer
  layer: Policies
  summary: Chains multiple scheduling policies — tries each in order, returns the first success.
  api_stability: in_work
  related:
    tests:
      - tests/test_policies.cpp
      - tests/test_Composite_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file Composite.h
 * @brief Composite scheduling policy.
 *
 * Composite implements the chain-of-responsibility pattern for scheduling
 * policies. It holds an ordered list of policies and tries each in order,
 * returning the first successful node selection.
 *
 * A policy is skipped when it returns SelectError. The Composite falls
 * through to the next policy only on NoneEligible or NoNodes — it does not
 * retry on success. The final error returned reflects whether any policy
 * saw nodes at all:
 * - If any policy returned NoneEligible, the result is NoneEligible.
 * - If all policies returned NoNodes, the result is NoNodes.
 *
 * Intended use: pair specialized policies with general fallbacks.
 *
 * Example — deadline-aware with LeastLoaded fallback:
 * @code
 * auto policies = std::vector<std::unique_ptr<ISchedulingPolicy>>{};
 * policies.push_back(std::make_unique<EarliestDeadlineFirst>());
 * policies.push_back(std::make_unique<LeastLoaded>());
 * auto composite = std::make_unique<Composite>(std::move(policies));
 * @endcode
 *
 * In this configuration:
 * - Deadline jobs: EarliestDeadlineFirst routes to the node with the most slack.
 *   If it succeeds, LeastLoaded is never called.
 * - Non-deadline jobs: EarliestDeadlineFirst still routes (it falls back to
 *   least-depth for non-deadline jobs internally). LeastLoaded acts as a
 *   safety net if EarliestDeadlineFirst returns NoneEligible.
 *
 * Thread-safety: Composite itself is stateless after construction. Each
 * contained policy is responsible for its own thread-safety. The policy list
 * is immutable after construction, so iteration is lock-free.
 *
 * Complexity: O(P × N) per call where P = policy count, N = node count.
 *             Stops at the first policy that returns a node.
 */

#include <memory>
#include <string_view>
#include <vector>

#include "Expected.h"

#include "balancer/ISchedulingPolicy.h"

namespace balancer
{

/**
 * @brief Composite scheduling policy — ordered chain of responsibility.
 *
 * Owns its contained policies. The policy list is fixed at construction.
 */
class Composite final : public ISchedulingPolicy
{
public:
    /**
     * @brief Construct with an ordered policy chain.
     *
     * Policies are tried in the order given. The first to return a NodeId
     * wins; remaining policies are not called.
     *
     * @param policies  Ordered list of policies. Must not be empty.
     */
    explicit Composite(std::vector<std::unique_ptr<ISchedulingPolicy>> policies) noexcept
        : mPolicies(std::move(policies))
    {}

    /**
     * @brief Try each policy in order; return the first successful selection.
     *
     * Returns NoneEligible if any policy saw nodes but none were eligible.
     * Returns NoNodes only if all policies saw an empty cluster.
     *
     * @param job   Job to route.
     * @param view  Cluster snapshot.
     * @return      Selected NodeId from the first succeeding policy, or SelectError.
     */
    [[nodiscard]] fat_p::Expected<NodeId, SelectError>
    selectNode(const Job& job, const ClusterView& view) const noexcept override
    {
        if (mPolicies.empty())
        {
            return fat_p::unexpected(SelectError::NoNodes);
        }

        bool anyPolicySawNodes = false;

        for (const auto& policy : mPolicies)
        {
            auto result = policy->selectNode(job, view);
            if (result.has_value())
            {
                return result;
            }
            // Track whether any policy reported nodes exist (vs empty cluster).
            if (result.error() == SelectError::NoneEligible)
            {
                anyPolicySawNodes = true;
            }
        }

        return fat_p::unexpected(
            anyPolicySawNodes ? SelectError::NoneEligible : SelectError::NoNodes);
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "Composite";
    }

private:
    std::vector<std::unique_ptr<ISchedulingPolicy>> mPolicies;
};

} // namespace balancer
