#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: ShortestJobFirst
  file_role: public_header
  path: policies/ShortestJobFirst.h
  namespace: balancer
  layer: Policies
  summary: Routes jobs to the node that minimizes predicted time-to-completion for the submitted job.
  api_stability: in_work
  related:
    tests:
      - tests/test_policies.cpp
      - tests/test_ShortestJobFirst_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file ShortestJobFirst.h
 * @brief Shortest-job-first routing policy.
 *
 * Routes each job to the eligible node that will finish THIS job soonest.
 * The estimated completion time for a job on a candidate node is:
 *
 *   estimatedCompletion(job, node) = predictedCost(job, node) × (queueDepth(node) + 1)
 *
 * predictedCost(job, node) integrates the CostModel's learned throughput
 * multiplier and per-JobClass correction. It approximates how long this specific
 * job will take on this node. Multiplying by (queueDepth + 1) treats the queue
 * as if it were filled with jobs of similar cost to the incoming job — a
 * reasonable approximation when job costs within a class are consistent.
 *
 * Relationship to LeastLoaded:
 * LeastLoaded minimizes queue depth × global multiplier, which is queue-depth-centric.
 * ShortestJobFirst minimizes predictedCost × queue depth, which is job-centric.
 * When all jobs have the same estimated cost, both policies produce identical routing.
 * ShortestJobFirst diverges when job classes have different costs: it prefers the
 * node where THIS job will finish fastest, even if that node has a deeper queue.
 *
 * Ties broken by NodeId (lower wins, stable and deterministic).
 *
 * Thread-safe: stateless between calls.
 *
 * Complexity: O(N) per call.
 */

#include <limits>

#include "Expected.h"

#include "balancer/ISchedulingPolicy.h"

namespace balancer
{

/**
 * @brief Shortest-job-first routing policy.
 */
class ShortestJobFirst final : public ISchedulingPolicy
{
public:
    ShortestJobFirst() noexcept = default;

    /**
     * @brief Route to the node that will finish this job soonest.
     *
     * Minimizes predictedCost(job, node) × (queueDepth(node) + 1).
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
        float  bestCompletion = std::numeric_limits<float>::max();
        bool   anyFound       = false;

        for (const auto& m : nodes)
        {
            if (!nodeAcceptsPriority(m.state, job.priority))
            {
                continue;
            }

            Cost  predicted   = view.predictCost(job, m.nodeId);
            // estimatedCompletion ≈ cost × (depth + 1): total work before this job finishes.
            float completion  = static_cast<float>(predicted.units)
                              * static_cast<float>(m.queueDepth + 1u);

            if (!anyFound || completion < bestCompletion
                || (completion == bestCompletion && m.nodeId < bestNode))
            {
                bestCompletion = completion;
                bestNode       = m.nodeId;
                anyFound       = true;
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
        return "ShortestJobFirst";
    }
};

} // namespace balancer
