#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: WeightedCapacity
  file_role: public_header
  path: policies/WeightedCapacity.h
  namespace: balancer
  layer: Policies
  summary: Routes jobs to the eligible node with the most available weighted capacity.
  api_stability: in_work
  related:
    tests:
      - tests/test_policies.cpp
      - tests/test_WeightedCapacity_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file WeightedCapacity.h
 * @brief Weighted capacity scheduling policy.
 *
 * Routes each job to the eligible node with the highest available weighted
 * capacity, computed as:
 *
 *   availableCapacity(node) = weight(node) × (1.0 - utilization(node))
 *
 * weight(node) is a caller-supplied value representing the node's processing
 * power relative to others. Nodes not in the weight map default to 1.0 (equal
 * weighting). A node with weight 2.0 can absorb twice as much work before its
 * effective capacity is depleted.
 *
 * This policy is designed for heterogeneous clusters where some nodes have
 * more resources than others. For homogeneous clusters with equal node weights,
 * availableCapacity reduces to (1.0 - utilization), routing to the least-utilized node.
 *
 * Relationship to LeastLoaded:
 * LeastLoaded minimizes queue depth weighted by learned throughput. WeightedCapacity
 * minimizes utilization weighted by node capacity. LeastLoaded is better when
 * job costs vary widely; WeightedCapacity is better when job costs are uniform
 * but node capacities differ.
 *
 * Thread-safe: stateless between calls.
 *
 * Complexity: O(N) per call, O(W) per weight lookup where W = weight map size.
 */

#include <limits>
#include <utility>
#include <vector>

#include "Expected.h"

#include "balancer/ISchedulingPolicy.h"

namespace balancer
{

/**
 * @brief Weighted capacity routing policy.
 *
 * Weights are configured at construction. The weight map is immutable after
 * construction — the policy is stateless between selectNode() calls.
 */
class WeightedCapacity final : public ISchedulingPolicy
{
public:
    /**
     * @brief Construct with optional per-node weights.
     *
     * @param weights  Pairs of (NodeId, weight). Nodes not listed default to 1.0.
     *                 Weight must be positive; non-positive weights are treated as 1.0.
     */
    explicit WeightedCapacity(std::vector<std::pair<NodeId, float>> weights = {}) noexcept
        : mWeights(std::move(weights))
    {}

    /**
     * @brief Route to the eligible node with the highest available weighted capacity.
     *
     * Available capacity = weight × (1.0 - utilization).
     * Ties broken by NodeId (lower wins, stable and deterministic).
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
        float  bestCapacity = -std::numeric_limits<float>::max();
        bool   anyFound     = false;

        for (const auto& m : nodes)
        {
            if (!nodeAcceptsPriority(m.state, job.priority))
            {
                continue;
            }

            float weight            = weightFor(m.nodeId);
            float availableCapacity = weight * (1.0f - m.utilization);

            if (!anyFound || availableCapacity > bestCapacity
                || (availableCapacity == bestCapacity && m.nodeId < bestNode))
            {
                bestCapacity = availableCapacity;
                bestNode     = m.nodeId;
                anyFound     = true;
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
        return "WeightedCapacity";
    }

private:
    /// Look up the configured weight for a node. Returns 1.0 for unlisted nodes
    /// or for entries with non-positive weight.
    [[nodiscard]] float weightFor(NodeId id) const noexcept
    {
        for (const auto& [nodeId, weight] : mWeights)
        {
            if (nodeId == id)
            {
                return weight > 0.0f ? weight : 1.0f;
            }
        }
        return 1.0f;
    }

    std::vector<std::pair<NodeId, float>> mWeights;
};

} // namespace balancer
