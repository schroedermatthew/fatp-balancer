#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: AffinityRouting
  file_role: public_header
  path: policies/AffinityRouting.h
  namespace: balancer
  layer: Policies
  summary: Affinity-guided routing policy — prefers the node with the highest learned affinity score for the job's class; falls back to least-loaded when no node is warm.
  api_stability: in_work
  related:
    docs_search: "AffinityRouting"
    tests:
      - tests/test_policies.cpp
      - tests/test_AffinityRouting_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file AffinityRouting.h
 * @brief Affinity-guided routing policy (Phase 5).
 *
 * AffinityRouting selects the candidate node that the CostModel believes is
 * most suited to the incoming job's class. It queries CostModel::affinityScore()
 * for every admitted candidate and picks the node with the lowest combined score:
 *
 *   score(node) = predictedCost(node) * (1.0 + affinityPenalty(node))
 *
 * where affinityPenalty is 0 for the node with the best (lowest) raw affinity
 * multiplier and proportionally higher for less-suited nodes.
 *
 * **Warm/cold gating:** if no candidate has a warm affinity cell for the
 * incoming job's class, AffinityRouting falls back to the node with the lowest
 * predicted cost — identical to LeastLoaded. This ensures the policy is never
 * worse than LeastLoaded during cold start.
 *
 * **Transferability:** AffinityRouting depends only on ClusterView and CostModel
 * (both in include/balancer/). It makes no assumptions about node implementation.
 *
 * **Concurrency model:** selectNode() is stateless between calls. All state lives
 * in the CostModel. selectNode() may be called concurrently from multiple threads.
 *
 * **Error model:** Returns Expected<NodeId, RoutingError>. Returns
 * RoutingError::NoEligibleNode when the cluster view contains no admitted nodes.
 */

#include <cstddef>
#include <limits>

// Balancer Interface
#include "balancer/ClusterView.h"
#include "balancer/CostModel.h"
#include "balancer/ISchedulingPolicy.h"
#include "balancer/Job.h"

namespace balancer
{

// ============================================================================
// AffinityRouting
// ============================================================================

/**
 * @brief Routes jobs to the node with the highest learned class affinity.
 *
 * When affinity data is warm for the incoming job's class, AffinityRouting
 * scores each candidate as `predictedCost / affinityScore` — a lower affinity
 * multiplier (node runs this class efficiently) translates to a lower score and
 * therefore a higher preference.
 *
 * When no warm affinity data exists for the job's class on any candidate, the
 * policy falls back to routing by predicted cost alone (equivalent to LeastLoaded).
 */
class AffinityRouting final : public ISchedulingPolicy
{
public:
    /**
     * @brief Construct with a reference to the shared CostModel.
     * @param costModel  The CostModel shared with all other policies.
     */
    explicit AffinityRouting(const CostModel& costModel) noexcept
        : mCostModel(costModel)
    {}

    /**
     * @brief Select the best node for the given job using affinity scoring.
     *
     * Algorithm:
     * 1. Collect all nodes admitted for the job's priority from the ClusterView.
     * 2. For each candidate, compute:
     *      score = predictedCost / affinityScore(node, jobClass)
     *    where affinityScore is 1.0 (neutral) for cold cells.
     * 3. Check if any candidate has a warm affinity cell. If none are warm,
     *    use predictedCost directly (cold-start fallback = LeastLoaded).
     * 4. Return the candidate with the lowest score.
     *
     * @param job   Job to route.
     * @param view  Immutable cluster snapshot.
     * @return      Selected NodeId, or RoutingError::NoEligibleNode.
     */
    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "AffinityRouting";
    }

    /**
     * @brief Select the best node for the given job using affinity scoring.
     *
     * Algorithm:
     * 1. Iterate view.nodes(), skip any not admitted for the job's priority.
     * 2. For each candidate, compute:
     *      score = predictedCost / affinityScore(node, jobClass)
     *    where affinityScore is 1.0 (neutral) for cold cells.
     * 3. Check if any candidate has a warm affinity cell. If none are warm,
     *    use predictedCost directly (cold-start fallback = LeastLoaded).
     * 4. Return the candidate with the lowest score.
     *
     * @param job   Job to route.
     * @param view  Immutable cluster snapshot.
     * @return      Selected NodeId, or SelectError::NoneEligible.
     */
    [[nodiscard]] fat_p::Expected<NodeId, SelectError>
    selectNode(const Job& job, const ClusterView& view) const noexcept override
    {
        NodeId   bestNode{0};
        float    bestScore    = std::numeric_limits<float>::max();
        bool     anyWarm      = false;
        bool     anyCandidate = false;

        // First pass: check whether any admitted candidate has a warm affinity cell.
        for (const LoadMetrics& m : view.nodes())
        {
            if (!nodeAcceptsPriority(m.state, job.priority)) { continue; }
            anyCandidate = true;
            if (mCostModel.isAffinityCellWarm(m.nodeId, job.jobClass))
            {
                anyWarm = true;
                break; // early exit — we only need to know if any are warm
            }
        }

        if (!anyCandidate)
        {
            return fat_p::unexpected(SelectError::NoneEligible);
        }

        // Second pass: score and pick.
        for (const LoadMetrics& m : view.nodes())
        {
            if (!nodeAcceptsPriority(m.state, job.priority)) { continue; }

            Cost  predicted = mCostModel.predict(job, m.nodeId);
            // Weight predicted cost by (queueDepth+1) so a busy node appears
            // more expensive than an idle one even when affinities are equal.
            // Without this, cold-start predicted costs are identical for all
            // nodes and every dispatch collapses onto the lowest NodeId.
            float queueWeight = static_cast<float>(m.queueDepth + 1);
            float score;

            if (anyWarm)
            {
                // Affinity-weighted: divide predicted cost by the affinity
                // multiplier. A low multiplier (efficient node for this class)
                // raises the denominator and lowers the score.
                float affinity = mCostModel.affinityScore(m.nodeId, job.jobClass);
                if (affinity <= 0.0f) { affinity = 1.0f; }
                score = static_cast<float>(predicted.units) * queueWeight / affinity;
            }
            else
            {
                // Cold-start fallback: route by predicted cost × queue depth
                // (degrades to LeastLoaded-equivalent behaviour).
                score = static_cast<float>(predicted.units) * queueWeight;
            }

            if (score < bestScore)
            {
                bestScore = score;
                bestNode  = m.nodeId;
            }
        }

        if (bestScore == std::numeric_limits<float>::max())
        {
            return fat_p::unexpected(SelectError::NoneEligible);
        }

        return bestNode;
    }

private:
    const CostModel& mCostModel;
};

} // namespace balancer
