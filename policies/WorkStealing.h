#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: WorkStealing
  file_role: public_header
  path: policies/WorkStealing.h
  namespace: balancer
  layer: Policies
  summary: Work-stealing-aware routing policy — routes to underloaded nodes to drain overloaded ones.
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
 * @file WorkStealing.h
 * @brief Work-stealing-aware scheduling policy.
 *
 * WorkStealing routes incoming jobs to nodes that are comparatively underloaded,
 * effectively pulling work away from overloaded nodes. The policy selects the
 * node minimising:
 *
 *   score(node, job) = predictedCost(job, node) × (queueDepth(node) + 1)
 *
 * This is the same score as ShortestJobFirst but with a steal bias: if any node
 * has a queue depth below mStealThreshold while another is above it, the policy
 * prioritises the underloaded node regardless of predicted cost. This mimics the
 * effect of a work-stealing scheduler: idle workers pull tasks from loaded ones.
 *
 * Steal bias rules (applied before score comparison):
 * 1. If a candidate node's queue depth < mStealThreshold and the current best
 *    node's queue depth >= mStealThreshold, the candidate wins unconditionally.
 * 2. Among candidates at the same threshold category (both below or both above),
 *    lowest score wins.
 *
 * Cold-start behaviour: during cold start, `predictedCost` returns the job's
 * raw estimatedCost, so the score collapses to estimatedCost × (depth + 1),
 * which is a reasonable proxy for time-to-start.
 *
 * @see ShortestJobFirst.h for the simpler score-only variant.
 * @see LeastLoaded.h for load-only routing without cost prediction.
 */

#include <cstdint>
#include <limits>
#include <string_view>

#include "Expected.h"

#include "balancer/ClusterView.h"
#include "balancer/ISchedulingPolicy.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"

namespace balancer
{

/**
 * @brief Work-stealing-aware routing policy.
 *
 * Construction:
 * @code
 * WorkStealing policy;                    // steal threshold = 4
 * WorkStealing policy{8};                 // steal threshold = 8
 * @endcode
 */
class WorkStealing final : public ISchedulingPolicy
{
public:
    /**
     * @brief Construct with configurable steal threshold.
     *
     * @param stealThreshold   Queue depth below which a node is considered
     *                         underloaded. Underloaded nodes are preferred
     *                         unconditionally over overloaded ones regardless
     *                         of predicted cost.
     */
    explicit WorkStealing(uint32_t stealThreshold = 4) noexcept
        : mStealThreshold(stealThreshold)
    {}

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "WorkStealing";
    }

    /**
     * @brief Select the node that best balances load using work-stealing logic.
     *
     * Eligible nodes: those whose state admits the job's priority (via
     * nodeAcceptsPriority). Among eligible nodes, the steal bias is applied
     * before the score comparison (see class documentation).
     *
     * @param job   Job to route. estimatedCost is used for score computation.
     * @param view  Immutable cluster snapshot.
     * @return      NodeId of the selected node, or NoEligibleNode if none exists.
     */
    [[nodiscard]] fat_p::Expected<NodeId, SelectError>
    selectNode(const Job& job, const ClusterView& view) const noexcept override
    {
        const NodeId kInvalid{0};
        NodeId bestId = kInvalid;
        double bestScore = std::numeric_limits<double>::max();
        bool bestIsUnderloaded = false;

        for (const LoadMetrics& m : view.nodes())
        {
            if (!nodeAcceptsPriority(m.state, job.priority))
            {
                continue;
            }

            const bool isUnderloaded = (m.queueDepth < mStealThreshold);
            const double predicted   = static_cast<double>(
                view.predictCost(job, m.nodeId).units);
            const double score = predicted * static_cast<double>(m.queueDepth + 1);

            // Steal bias: underloaded node beats any overloaded node.
            if (bestId == kInvalid)
            {
                bestId           = m.nodeId;
                bestScore        = score;
                bestIsUnderloaded = isUnderloaded;
                continue;
            }

            // Candidate is underloaded but current best is not — steal bias wins.
            if (isUnderloaded && !bestIsUnderloaded)
            {
                bestId            = m.nodeId;
                bestScore         = score;
                bestIsUnderloaded = true;
                continue;
            }

            // Current best is underloaded but candidate is not — keep best.
            if (!isUnderloaded && bestIsUnderloaded)
            {
                continue;
            }

            // Same category — lower score wins.
            if (score < bestScore)
            {
                bestId    = m.nodeId;
                bestScore = score;
                bestIsUnderloaded = isUnderloaded;
            }
        }

        if (bestId == kInvalid)
        {
            return fat_p::unexpected(SelectError::NoneEligible);
        }
        return bestId;
    }

private:
    uint32_t mStealThreshold;
};

} // namespace balancer
