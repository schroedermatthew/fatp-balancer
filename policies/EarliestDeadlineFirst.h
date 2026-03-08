#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: EarliestDeadlineFirst
  file_role: public_header
  path: policies/EarliestDeadlineFirst.h
  namespace: balancer
  layer: Policies
  summary: Routes deadline jobs to the node most likely to finish before the deadline.
  api_stability: in_work
  related:
    tests:
      - tests/test_policies.cpp
      - tests/test_EarliestDeadlineFirst_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file EarliestDeadlineFirst.h
 * @brief Earliest-deadline-first routing policy.
 *
 * For jobs WITH a deadline, routes to the eligible node that maximizes the
 * slack between the estimated finish time and the deadline:
 *
 *   estimatedFinish(job, node) = now + predictedCost(job, node) × (queueDepth(node) + 1)
 *   slack(job, node)           = deadline - estimatedFinish(job, node)
 *
 * The node with the highest slack is selected — it is the node most likely
 * to meet the deadline. A negative slack means the node will likely miss the
 * deadline; the policy still routes to the best available node, but the
 * Balancer's pre-flight check should have already rejected the job if no
 * node can plausibly meet the deadline.
 *
 * For jobs WITHOUT a deadline, the policy falls back to least-queue-depth
 * routing (equivalent to LeastLoaded without the CostModel multiplier).
 * The rationale: without a deadline, minimizing latency is secondary to
 * throughput, and queue depth is the simplest proxy for wait time.
 *
 * Relationship to other policies:
 * EarliestDeadlineFirst is a deadline-aware wrapper. For non-deadline work it
 * is less sophisticated than LeastLoaded (no cost model integration). The
 * recommended Composite configuration is: EarliestDeadlineFirst first (for
 * deadline jobs), LeastLoaded as fallback (for everything else).
 *
 * Ties broken by NodeId (lower wins, stable and deterministic).
 *
 * Thread-safe: stateless between calls.
 *
 * Complexity: O(N) per call.
 */

#include <chrono>
#include <limits>

#include "Expected.h"

#include "balancer/ISchedulingPolicy.h"

namespace balancer
{

/**
 * @brief Earliest-deadline-first routing policy.
 */
class EarliestDeadlineFirst final : public ISchedulingPolicy
{
public:
    EarliestDeadlineFirst() noexcept = default;

    /**
     * @brief Route to maximize slack for deadline jobs; least-loaded for others.
     *
     * For deadline jobs: selects the node with the highest
     * (deadline - estimatedFinish) slack. Slack may be negative if the
     * deadline is very tight — the policy still picks the best available node.
     *
     * For non-deadline jobs: selects the node with the lowest queue depth.
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

        if (job.deadline.has_value())
        {
            return selectForDeadlineJob(job, view, nodes);
        }

        return selectForNonDeadlineJob(job, nodes);
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "EarliestDeadlineFirst";
    }

private:
    [[nodiscard]] fat_p::Expected<NodeId, SelectError>
    selectForDeadlineJob(const Job& job,
                         const ClusterView& view,
                         std::span<const LoadMetrics> nodes) const
    {
        // Compute slack = deadline - estimatedFinish for each eligible node.
        // estimatedFinish = now + predictedCost × (queueDepth + 1).
        // Cost units are treated as microseconds for the purpose of this estimate.
        // This is a relative comparison — absolute units do not matter as long
        // as they are consistent across nodes.

        auto now = Clock::now();
        TimePoint deadline = *job.deadline;

        NodeId   bestNode;
        Duration bestSlack = Duration::min();
        bool     anyFound  = false;

        for (const auto& m : nodes)
        {
            if (!nodeAcceptsPriority(m.state, job.priority))
            {
                continue;
            }

            Cost predicted = view.predictCost(job, m.nodeId);
            auto totalWork = static_cast<int64_t>(predicted.units)
                           * static_cast<int64_t>(m.queueDepth + 1u);
            auto estimatedDuration = std::chrono::microseconds(totalWork);
            Duration slack         = deadline - (now + estimatedDuration);

            if (!anyFound || slack > bestSlack
                || (slack == bestSlack && m.nodeId < bestNode))
            {
                bestSlack = slack;
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

    [[nodiscard]] fat_p::Expected<NodeId, SelectError>
    selectForNonDeadlineJob(const Job& job,
                            std::span<const LoadMetrics> nodes) const
    {
        NodeId   bestNode;
        uint32_t bestDepth = std::numeric_limits<uint32_t>::max();
        bool     anyFound  = false;

        for (const auto& m : nodes)
        {
            if (!nodeAcceptsPriority(m.state, job.priority))
            {
                continue;
            }

            if (!anyFound || m.queueDepth < bestDepth
                || (m.queueDepth == bestDepth && m.nodeId < bestNode))
            {
                bestDepth = m.queueDepth;
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
};

} // namespace balancer
