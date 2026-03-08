#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: ClusterView
  file_role: public_header
  path: include/balancer/ClusterView.h
  namespace: balancer
  layer: Core
  summary: Immutable cluster snapshot passed to scheduling policies.
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
 * @file ClusterView.h
 * @brief Immutable cluster snapshot for scheduling policy decisions.
 *
 * ClusterView is built by the Balancer immediately before a policy call.
 * It gives policies read access to all node metrics and the shared CostModel
 * without exposing mutable balancer internals.
 *
 * Policies must not cache ClusterView across calls — snapshots become stale.
 */

#include <span>
#include <vector>

#include "balancer/CostModel.h"
#include "balancer/INode.h"
#include "balancer/LoadMetrics.h"

namespace balancer
{

/**
 * @brief Immutable snapshot of cluster state for scheduling decisions.
 *
 * ClusterView is constructed on the stack before each policy call and
 * destroyed after. Its lifetime is bounded by the policy call.
 *
 * @note Thread-safety: ClusterView is not thread-safe. It is constructed
 *       and consumed within the balancer's scheduling lock.
 */
class ClusterView
{
public:
    /**
     * @brief Construct from a snapshot of node metrics and the shared model.
     *
     * @param nodeMetrics   Snapshot of each node's LoadMetrics.
     * @param clusterMetrics Aggregate cluster metrics.
     * @param costModel      Shared learning model (non-owning reference).
     */
    ClusterView(std::vector<LoadMetrics>  nodeMetrics,
                ClusterMetrics            clusterMetrics,
                const CostModel&          costModel) noexcept
        : mNodeMetrics(std::move(nodeMetrics))
        , mClusterMetrics(clusterMetrics)
        , mCostModel(costModel)
    {}

    // ---- Node queries ------------------------------------------------------

    /**
     * @brief All node metrics in this snapshot.
     * @return Span of LoadMetrics. Order is stable within a snapshot.
     */
    [[nodiscard]] std::span<const LoadMetrics> nodes() const noexcept
    {
        return mNodeMetrics;
    }

    /**
     * @brief Metrics for a specific node.
     * @param nodeId  Node to look up.
     * @return        Pointer to metrics, or nullptr if node not in snapshot.
     */
    [[nodiscard]] const LoadMetrics* nodeMetrics(NodeId nodeId) const noexcept
    {
        for (const auto& m : mNodeMetrics)
        {
            if (m.nodeId == nodeId)
            {
                return &m;
            }
        }
        return nullptr;
    }

    /**
     * @brief Cluster-level aggregate metrics.
     */
    [[nodiscard]] const ClusterMetrics& cluster() const noexcept
    {
        return mClusterMetrics;
    }

    // ---- Cost model access -------------------------------------------------

    /**
     * @brief Predict the cost of running job on candidate node.
     *
     * Delegates to the shared CostModel. Returns job.estimatedCost for
     * cold or unknown nodes.
     *
     * @param job       Job to route.
     * @param nodeId    Candidate node.
     * @return          Predicted cost on this node.
     */
    [[nodiscard]] Cost predictCost(const Job& job, NodeId nodeId) const noexcept
    {
        return mCostModel.predict(job, nodeId);
    }

    /**
     * @brief True if the node's cost model is warm (enough observations).
     */
    [[nodiscard]] bool isNodeWarm(NodeId nodeId) const noexcept
    {
        return mCostModel.isWarm(nodeId);
    }

    /**
     * @brief Learned throughput multiplier for a node. 1.0 if cold or unknown.
     */
    [[nodiscard]] float nodeMultiplier(NodeId nodeId) const noexcept
    {
        return mCostModel.nodeMultiplier(nodeId);
    }

    // ---- Eligibility helpers -----------------------------------------------

    /**
     * @brief Returns all nodes eligible to accept a job at the given priority.
     *
     * Allocates; intended for use in O(N)-per-call policies only.
     * Hot policies should iterate nodes() directly.
     *
     * @param priority  Required priority.
     * @return          Vector of NodeIds that can accept this priority.
     */
    [[nodiscard]] std::vector<NodeId> eligibleNodes(Priority priority) const
    {
        std::vector<NodeId> result;
        result.reserve(mNodeMetrics.size());
        for (const auto& m : mNodeMetrics)
        {
            if (nodeAcceptsPriority(m.state, priority))
            {
                result.push_back(m.nodeId);
            }
        }
        return result;
    }

private:
    std::vector<LoadMetrics> mNodeMetrics;
    ClusterMetrics           mClusterMetrics;
    const CostModel&         mCostModel;
};

} // namespace balancer
