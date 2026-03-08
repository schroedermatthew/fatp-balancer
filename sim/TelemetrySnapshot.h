#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: TelemetrySnapshot
  file_role: public_header
  path: sim/TelemetrySnapshot.h
  namespace: balancer::sim
  layer: Sim
  summary: >\n    POD snapshot of per-node and cluster metrics captured by SimulatedCluster for\n    TelemetryAdvisor evaluation. Includes FATP_JSON_DEFINE_TYPE_NON_INTRUSIVE\n    registration for both NodeEntry and TelemetrySnapshot, and a formatJson()\n    convenience function for diagnostic output and the demo JSON panel.
  api_stability: in_work
  related:
    docs_search: "TelemetrySnapshot"
    tests:
      - tests/test_TelemetrySnapshot_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file TelemetrySnapshot.h
 * @brief POD metric snapshot fed from SimulatedCluster to TelemetryAdvisor.
 *
 * TelemetryAdvisor evaluates a TelemetrySnapshot once per advisor tick to
 * decide which alert features (if any) to enable or disable. The snapshot is
 * a value type — cheap to copy and free of any dependency on the live cluster
 * or the balancer.
 *
 * Capture contract:
 * SimulatedCluster::captureSnapshot() reads LoadMetrics from each node plus
 * aggregate counts from the Balancer and packages them here. The snapshot is
 * consistent at the time of capture; no locking is held while TelemetryAdvisor
 * processes it.
 *
 * Design notes:
 * - All fields have zero-initialised defaults so the struct is valid as a
 *   default-constructed value (safe for testing stubs that only set some fields).
 * - NodeEntry::nodeId is a uint32_t (raw NodeId value) to keep the struct
 *   dependency-free: TelemetrySnapshot does not include balancer headers, only
 *   standard library headers.
 *
 * @warning Sim layer. Do not include from include/balancer/ or policies/.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "JsonLite.h"

namespace balancer::sim
{

// ============================================================================
// NodeEntry
// ============================================================================

/**
 * @brief Per-node metrics captured in a single TelemetrySnapshot.
 *
 * All latency values are in microseconds (matching LoadMetrics convention).
 */
struct NodeEntry
{
    /// Raw NodeId value (NodeId::value()). Zero means unused/invalid.
    uint32_t nodeId = 0;

    /// NodeState as uint8_t. Matches the NodeState enum (Offline=0, …, Failed=6).
    /// Stored as integer to avoid a dependency on LoadMetrics.h in this POD header.
    uint8_t  state = 0; ///< 0=Offline, 1=Initializing, 2=Idle, 3=Busy, 4=Overloaded, 5=Draining, 6=Failed, 7=Recovering

    /// Fraction of maximum capacity in use. Range [0.0, 1.0].
    float    utilization = 0.0f;

    /// Total jobs currently queued or executing across all priority bands.
    uint32_t queueDepth = 0;

    /// Rolling p50 latency in microseconds. 0 if no completed jobs yet.
    uint64_t p50LatencyUs = 0;

    /// Rolling p99 latency in microseconds. 0 if no completed jobs yet.
    uint64_t p99LatencyUs = 0;

    /// Total jobs completed by this node since it started.
    uint64_t completedJobs = 0;

    /// Total jobs that failed (node error) since this node started.
    uint64_t failedJobs = 0;
};

// ============================================================================
// TelemetrySnapshot
// ============================================================================

/**
 * @brief Cluster-wide metric snapshot for one TelemetryAdvisor evaluation tick.
 *
 * Captured by SimulatedCluster::captureSnapshot() and passed by value to
 * TelemetryAdvisor::evaluate(). The snapshot represents a single consistent
 * point-in-time view of the cluster; advisors do not access the live cluster.
 *
 * Thresholds in AdvisorConfig are calibrated for the default 500 ms advisor
 * tick interval. If the tick interval changes, thresholds must be re-tuned.
 */
struct TelemetrySnapshot
{
    // ---- Per-node metrics --------------------------------------------------

    /// One entry per node in the cluster. Order matches SimulatedCluster::nodes().
    std::vector<NodeEntry> nodes;

    // ---- Cluster aggregate metrics -----------------------------------------

    /// Number of nodes currently in Idle or Busy state.
    uint32_t activeNodes = 0;

    /// Number of nodes in Failed or Offline state.
    uint32_t unavailableNodes = 0;

    /// Total jobs submitted since the balancer started.
    uint64_t totalSubmitted = 0;

    /// Jobs currently in flight (submitted but not yet completed).
    uint32_t inFlightJobs = 0;

    /// Admission rejections attributed to RateLimited since balancer start.
    uint64_t rejectedRateLimited = 0;

    /// Admission rejections attributed to PriorityRejected since balancer start.
    uint64_t rejectedPriorityRejected = 0;

    /// Admission rejections attributed to ClusterSaturated since balancer start.
    uint64_t rejectedClusterSaturated = 0;

    // ---- Derived / advisory convenience fields ----------------------------

    /// Fraction of nodes currently in Overloaded state. Range [0.0, 1.0].
    /// Computed by captureSnapshot(); saves advisors from iterating NodeEntry.
    float overloadedFraction = 0.0f;

    /// Fraction of nodes currently in Failed state. Range [0.0, 1.0].
    float failedFraction = 0.0f;

    /// Cluster-wide p99 latency in microseconds (max p99 across all nodes).
    /// A single outlier node drives this up, making it sensitive to stragglers.
    uint64_t clusterP99LatencyUs = 0;

    /// Cluster-wide p50 latency in microseconds (mean p50 across active nodes).
    /// 0 if no active nodes have completed any jobs.
    uint64_t clusterP50LatencyUs = 0;
};

} // namespace balancer::sim

namespace balancer::sim
{

/**
 * @brief Serialize a TelemetrySnapshot to a JSON string.
 *
 * Produces a self-contained JSON object suitable for logging, the WASM demo
 * JSON panel, and diagnostic tooling. Builds a fat_p::JsonObject manually;
 * the FATP_JSON_DEFINE_TYPE_NON_INTRUSIVE macro is not used here because it
 * references fat_p::json_detail internals that are not visible outside the
 * fat_p namespace.
 *
 * @param snap    Snapshot to serialize.
 * @param pretty  true = indented, false = compact (single line).
 * @return        JSON string. Always valid — never throws.
 */
[[nodiscard]] inline std::string formatJson(const TelemetrySnapshot& snap,
                                            bool pretty = true)
{
    fat_p::JsonArray nodeArr;
    nodeArr.reserve(snap.nodes.size());
    for (const auto& n : snap.nodes)
    {
        fat_p::JsonObject node;
        node["nodeId"]       = fat_p::JsonValue{static_cast<int64_t>(n.nodeId)};
        node["state"]        = fat_p::JsonValue{static_cast<int64_t>(n.state)};
        node["utilization"]  = fat_p::JsonValue{static_cast<double>(n.utilization)};
        node["queueDepth"]   = fat_p::JsonValue{static_cast<int64_t>(n.queueDepth)};
        node["p50LatencyUs"] = fat_p::JsonValue{static_cast<int64_t>(n.p50LatencyUs)};
        node["p99LatencyUs"] = fat_p::JsonValue{static_cast<int64_t>(n.p99LatencyUs)};
        node["completedJobs"] = fat_p::JsonValue{static_cast<int64_t>(n.completedJobs)};
        node["failedJobs"]   = fat_p::JsonValue{static_cast<int64_t>(n.failedJobs)};
        nodeArr.push_back(fat_p::JsonValue{std::move(node)});
    }

    fat_p::JsonObject obj;
    obj["nodes"]                     = fat_p::JsonValue{std::move(nodeArr)};
    obj["activeNodes"]               = fat_p::JsonValue{static_cast<int64_t>(snap.activeNodes)};
    obj["unavailableNodes"]          = fat_p::JsonValue{static_cast<int64_t>(snap.unavailableNodes)};
    obj["totalSubmitted"]            = fat_p::JsonValue{static_cast<int64_t>(snap.totalSubmitted)};
    obj["inFlightJobs"]              = fat_p::JsonValue{static_cast<int64_t>(snap.inFlightJobs)};
    obj["rejectedRateLimited"]       = fat_p::JsonValue{static_cast<int64_t>(snap.rejectedRateLimited)};
    obj["rejectedPriorityRejected"]  = fat_p::JsonValue{static_cast<int64_t>(snap.rejectedPriorityRejected)};
    obj["rejectedClusterSaturated"]  = fat_p::JsonValue{static_cast<int64_t>(snap.rejectedClusterSaturated)};
    obj["overloadedFraction"]        = fat_p::JsonValue{static_cast<double>(snap.overloadedFraction)};
    obj["failedFraction"]            = fat_p::JsonValue{static_cast<double>(snap.failedFraction)};
    obj["clusterP99LatencyUs"]       = fat_p::JsonValue{static_cast<int64_t>(snap.clusterP99LatencyUs)};
    obj["clusterP50LatencyUs"]       = fat_p::JsonValue{static_cast<int64_t>(snap.clusterP50LatencyUs)};

    return fat_p::to_json_string(fat_p::JsonValue{std::move(obj)}, pretty);
}

} // namespace balancer::sim
