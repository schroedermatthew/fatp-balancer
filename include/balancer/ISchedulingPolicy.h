#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: ISchedulingPolicy
  file_role: public_header
  path: include/balancer/ISchedulingPolicy.h
  namespace: balancer
  layer: Interface
  summary: Scheduling strategy contract — all routing policies implement this interface.
  api_stability: in_work
  related:
    docs_search: "ISchedulingPolicy"
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
 * @file ISchedulingPolicy.h
 * @brief Scheduling strategy contract.
 *
 * All routing policies (RoundRobin, LeastLoaded, Composite, etc.) implement
 * ISchedulingPolicy. The Balancer calls selectNode() once per job on the
 * routing path.
 *
 * Contracts:
 * - Stateless between calls. All state lives in CostModel (via ClusterView)
 *   or in the balancer infrastructure, not in the policy object.
 * - Exception-free. Return Expected; never throw.
 * - Non-blocking. selectNode() must complete in bounded time.
 *
 * @see policies/ for implementations.
 */

#include <string_view>

#include "Expected.h"

#include "balancer/ClusterView.h"
#include "balancer/Job.h"

namespace balancer
{

/**
 * @brief Error returned when no eligible node can be found.
 */
enum class SelectError : uint8_t
{
    /// No nodes exist in the cluster.
    NoNodes,

    /// Nodes exist but none is eligible for this job's priority.
    NoneEligible,
};

/**
 * @brief Abstract scheduling policy interface.
 *
 * @note Policies are designed to be stateless. If a policy needs to maintain
 *       per-run counters (e.g., RoundRobin's cursor), that state is private
 *       to the policy object and must be accessed via an atomic or mutex —
 *       the Balancer may call selectNode() from multiple threads.
 */
class ISchedulingPolicy
{
public:
    virtual ~ISchedulingPolicy() = default;

    ISchedulingPolicy(const ISchedulingPolicy&)            = delete;
    ISchedulingPolicy& operator=(const ISchedulingPolicy&) = delete;
    ISchedulingPolicy(ISchedulingPolicy&&)                 = delete;
    ISchedulingPolicy& operator=(ISchedulingPolicy&&)      = delete;

    /**
     * @brief Select the best node for the given job.
     *
     * @param job     Job to route.
     * @param view    Immutable cluster snapshot. Do not cache across calls.
     * @return        NodeId of selected node, or SelectError if none eligible.
     */
    [[nodiscard]] virtual fat_p::Expected<NodeId, SelectError>
    selectNode(const Job& job, const ClusterView& view) const noexcept = 0;

    /**
     * @brief Human-readable name for diagnostics and logging.
     * @return Policy name. Must be stable (same value every call).
     */
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

protected:
    ISchedulingPolicy() = default;
};

} // namespace balancer
