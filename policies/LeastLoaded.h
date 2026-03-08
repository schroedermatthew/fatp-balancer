#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: LeastLoaded
  file_role: public_header
  path: policies/LeastLoaded.h
  namespace: balancer
  layer: Policies
  summary: Routes jobs to the eligible node with the lowest predicted load.
  api_stability: in_work
  related:
    tests:
      - tests/test_policies.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file LeastLoaded.h
 * @brief Least-loaded scheduling policy.
 *
 * Routes each job to the eligible node with the lowest effective load,
 * computed as:
 *
 *   effectiveLoad(node) = (queueDepth(node) + 1) * predictedCostMultiplier(node, job)
 *
 * The `+1` base ensures every idle node scores 1.0 × multiplier rather than
 * 0.0. Without it, all idle nodes score zero and the tiebreak always resolves
 * to the lowest NodeId, concentrating all dispatches onto Node 0 until it
 * saturates. With the `+1`, as soon as any node accepts a job its score
 * doubles relative to fully-idle peers, spreading load across all nodes within
 * the first N dispatches (where N is the cluster size).
 *
 * The predictedCostMultiplier integrates the CostModel's learned throughput
 * multiplier. A node that processes jobs 2x slower than estimated is treated
 * as having 2x its actual (queue+1) depth for routing purposes. This makes
 * LeastLoaded cost-aware from the first warm observation.
 *
 * During cold start, predictedCostMultiplier = 1.0 (neutral), so the policy
 * degrades gracefully to pure (queue+1) routing.
 *
 * Thread-safe: stateless between calls (no mutable state).
 *
 * Complexity: O(N) per call.
 */

#include <limits>

#include "Expected.h"

#include "balancer/ISchedulingPolicy.h"

namespace balancer
{

/**
 * @brief Least-loaded routing policy.
 */
class LeastLoaded final : public ISchedulingPolicy
{
public:
    LeastLoaded() noexcept = default;

    /**
     * @brief Route to the eligible node with the lowest effective load.
     *
     * Effective load = (queueDepth + 1) * learnedMultiplier.
     * The +1 base prevents all-zero scores when the cluster is idle, which
     * would otherwise always resolve ties to the lowest NodeId.
     * Ties broken by NodeId (stable, deterministic).
     *
     * @param job   Job to route.
     * @param view  Cluster snapshot.
     * @return      Selected NodeId, or SelectError.
     */
    [[nodiscard]] fat_p::Expected<NodeId, SelectError>
    selectNode(const Job& job, const ClusterView& view) const noexcept override
    {
        auto nodes = view.nodes();
        if (nodes.empty())
        {
            return fat_p::unexpected(SelectError::NoNodes);
        }

        NodeId bestNode;
        float  bestLoad  = std::numeric_limits<float>::max();
        bool   anyFound  = false;

        for (const auto& m : nodes)
        {
            if (!nodeAcceptsPriority(m.state, job.priority))
            {
                continue;
            }

            // Effective load integrates the CostModel multiplier.
            // The +1 base ensures idle nodes score 1×multiplier rather than 0,
            // preventing all idle nodes from tying at 0 and collapsing every
            // dispatch onto Node 0 until it saturates.
            float multiplier    = view.nodeMultiplier(m.nodeId);
            float effectiveLoad = static_cast<float>(m.queueDepth + 1) * multiplier;

            if (!anyFound || effectiveLoad < bestLoad
                || (effectiveLoad == bestLoad && m.nodeId < bestNode))
            {
                bestLoad  = effectiveLoad;
                bestNode  = m.nodeId;
                anyFound  = true;
            }
        }

        if (!anyFound)
        {
            return fat_p::unexpected(SelectError::NoneEligible);
        }

        return bestNode;
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "LeastLoaded";
    }
};

} // namespace balancer
