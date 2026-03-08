#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: RoundRobin
  file_role: public_header
  path: policies/RoundRobin.h
  namespace: balancer
  layer: Policies
  summary: Priority-filtered round-robin scheduling policy.
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
 * @file RoundRobin.h
 * @brief Priority-filtered round-robin scheduling policy.
 *
 * Distributes work evenly across eligible nodes by cycling a cursor through
 * the node list. Nodes that do not accept the job's priority are skipped.
 *
 * Thread-safe: the cursor is an atomic, so concurrent selectNode() calls
 * from multiple threads are safe and produce distinct selections.
 *
 * Complexity: O(N) per call (N = number of nodes). The cursor advances
 * modulo the total node count, not just eligible nodes, so distribution
 * may be uneven when many nodes are ineligible.
 *
 * Best used as a baseline or for homogeneous clusters with stable load.
 */

#include <atomic>
#include <cstdint>

#include "Expected.h"

#include "balancer/ISchedulingPolicy.h"

namespace balancer
{

/**
 * @brief Priority-filtered round-robin policy.
 */
class RoundRobin final : public ISchedulingPolicy
{
public:
    RoundRobin() noexcept = default;

    /**
     * @brief Select the next eligible node in round-robin order.
     *
     * Advances the shared cursor and scans forward for the next node
     * that accepts the job's priority. Wraps around at the end of the list.
     *
     * Returns NoneEligible if no node accepts the job's priority.
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

        const size_t count = nodes.size();

        // Atomically advance cursor and scan for an eligible node.
        // We try at most count nodes to avoid infinite loop when none eligible.
        uint32_t start = mCursor.fetch_add(1, std::memory_order_relaxed) % count;

        for (size_t i = 0; i < count; ++i)
        {
            const auto& m = nodes[(start + i) % count];
            if (nodeAcceptsPriority(m.state, job.priority))
            {
                return m.nodeId;
            }
        }

        return fat_p::unexpected(SelectError::NoneEligible);
    }

    [[nodiscard]] std::string_view name() const noexcept override
    {
        return "RoundRobin";
    }

private:
    mutable std::atomic<uint32_t> mCursor{0};
};

} // namespace balancer
