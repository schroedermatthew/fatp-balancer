#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: SimulatedCluster
  file_role: public_header
  path: sim/SimulatedCluster.h
  namespace: balancer::sim
  layer: Sim
  summary: >\n    Manages a set of SimulatedNodes, exposes INode pointers for the Balancer,\n    captures TelemetrySnapshot for TelemetryAdvisor, provides tick() to drive\n    TelemetryAdvisor from a live ClusterMetrics snapshot, and provides\n    startSnapshotWriter() to periodically write TelemetrySnapshot JSON to disk\n    for the Python nn_advisor_harness inference loop.
  api_stability: in_work
  related:
    tests:
      - tests/test_balancer_core.cpp
      - tests/test_fault_scenarios.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file SimulatedCluster.h
 * @brief Manages a collection of SimulatedNodes.
 *
 * SimulatedCluster is the test harness entry point. It creates N SimulatedNodes,
 * starts their worker threads, and exposes INode* pointers for the Balancer.
 *
 * @warning Sim layer. Do not include from include/balancer/ or policies/.
 */

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "balancer/Balancer.h"
#include "balancer/INode.h"
#include "balancer/LoadMetrics.h"
#include "balancer/TelemetryAdvisor.h"
#include "sim/SimulatedNode.h"
#include "sim/TelemetrySnapshot.h"

namespace balancer::sim
{

namespace detail
{

/**
 * @brief Convert a TelemetrySnapshot into a ClusterMetrics for TelemetryAdvisor.
 *
 * Reconstructs the overloadedNodes count from overloadedFraction so that
 * knownNodes = activeNodes + unavailableNodes + overloadedNodes, matching
 * buildClusterMetrics() in Balancer. Without this, a cluster where all nodes
 * are Overloaded (not Failed) would produce a zero denominator and suppress
 * the alert.overload signal.
 */
inline balancer::ClusterMetrics toClusterMetrics(const TelemetrySnapshot& snap) noexcept
{
    balancer::ClusterMetrics cm;
    cm.activeNodes        = snap.activeNodes;
    cm.unavailableNodes   = snap.unavailableNodes;
    cm.totalSubmitted     = snap.totalSubmitted;
    cm.totalCompleted     = 0; // not tracked in snapshot
    cm.totalRejected      = snap.rejectedRateLimited
                          + snap.rejectedPriorityRejected
                          + snap.rejectedClusterSaturated;
    cm.meanP50LatencyUs   = snap.clusterP50LatencyUs;
    cm.maxP99LatencyUs    = snap.clusterP99LatencyUs;

    // Reconstruct overloadedNodes from overloadedFraction so the denominator
    // matches buildClusterMetrics(). overloadedFraction uses len(nodes) as its
    // denominator (physical node count, see captureSnapshot).
    const uint32_t physicalTotal = static_cast<uint32_t>(snap.nodes.size());
    const uint32_t overloadedNodes = (physicalTotal > 0)
        ? static_cast<uint32_t>(
              snap.overloadedFraction * static_cast<float>(physicalTotal) + 0.5f)
        : 0u;
    cm.overloadedNodes = overloadedNodes;
    cm.knownNodes      = cm.activeNodes + cm.unavailableNodes + overloadedNodes;

    return cm;
}

} // namespace detail

/**
 * @brief Configuration for a SimulatedCluster.
 *
 * Mirrors the fields of SimulatedNodeConfig plus a nodeCount field so tests
 * can describe the whole cluster in one struct.
 */
struct SimulatedClusterConfig
{
    uint32_t nodeCount         = 3;
    uint32_t workerCount       = 2;
    uint32_t overloadThreshold = 32;
    uint32_t recoverThreshold  = 16;
    uint32_t minJobDurationUs  = 0;
    uint32_t maxJobDurationUs  = 0;
};

/**
 * @brief Collection of SimulatedNodes forming a test cluster.
 */
class SimulatedCluster
{
public:
    /**
     * @brief Construct with an explicit node count and per-node config.
     *
     * Nodes are started immediately (RAII). @p start() is a no-op when called
     * after this constructor; @p stop() may be called before destruction.
     *
     * @param nodeCount     Number of nodes to create.
     * @param nodeConfig    Configuration applied to every node.
     */
    explicit SimulatedCluster(uint32_t nodeCount,
                               SimulatedNodeConfig nodeConfig = {})
    {
        mNodes.reserve(nodeCount);
        for (uint32_t i = 0; i < nodeCount; ++i)
        {
            mNodes.push_back(std::make_unique<SimulatedNode>(
                balancer::NodeId{i + 1}, nodeConfig));
        }
        start();
    }

    /**
     * @brief Construct from a SimulatedClusterConfig.
     *
     * Nodes are started immediately. @p start() is idempotent.
     *
     * @param cfg  Cluster configuration.
     */
    explicit SimulatedCluster(const SimulatedClusterConfig& cfg)
    {
        SimulatedNodeConfig nc;
        nc.workerCount       = cfg.workerCount;
        nc.overloadThreshold = cfg.overloadThreshold;
        nc.recoverThreshold  = cfg.recoverThreshold;
        nc.minJobDurationUs  = cfg.minJobDurationUs;
        nc.maxJobDurationUs  = cfg.maxJobDurationUs;

        mNodes.reserve(cfg.nodeCount);
        for (uint32_t i = 0; i < cfg.nodeCount; ++i)
        {
            mNodes.push_back(std::make_unique<SimulatedNode>(
                balancer::NodeId{i + 1}, nc));
        }
        start();
    }

    ~SimulatedCluster()
    {
        stop();
    }

    /**
     * @brief Start all nodes. Idempotent — safe to call multiple times.
     */
    void start()
    {
        if (mStarted) { return; }
        mStarted = true;
        for (auto& node : mNodes)
        {
            node->start();
        }
    }

    /**
     * @brief Stop all nodes. Idempotent — safe to call multiple times.
     */
    void stop()
    {
        stopSnapshotWriter();
        if (!mStarted) { return; }
        mStarted = false;
        for (auto it = mNodes.rbegin(); it != mNodes.rend(); ++it)
        {
            (*it)->stop();
        }
    }

    // Non-copyable, non-movable — owns node lifetimes.
    SimulatedCluster(const SimulatedCluster&)            = delete;
    SimulatedCluster& operator=(const SimulatedCluster&) = delete;
    SimulatedCluster(SimulatedCluster&&)                 = delete;
    SimulatedCluster& operator=(SimulatedCluster&&)      = delete;

    /**
     * @brief Non-owning INode pointers for the Balancer.
     * @return Vector of raw INode*. Valid for the cluster's lifetime.
     */
    [[nodiscard]] std::vector<balancer::INode*> nodes() const
    {
        std::vector<balancer::INode*> result;
        result.reserve(mNodes.size());
        for (const auto& node : mNodes)
        {
            result.push_back(node.get());
        }
        return result;
    }

    /**
     * @brief Access a specific node by zero-based index.
     * @param index  Node index. Must be < nodeCount().
     * @return       Reference to the SimulatedNode.
     */
    [[nodiscard]] SimulatedNode& node(size_t index)
    {
        return *mNodes.at(index);
    }

    /// Number of nodes in the cluster.
    [[nodiscard]] size_t nodeCount() const noexcept { return mNodes.size(); }

    /**
     * @brief Block until all nodes have empty queues and no active workers.
     *
     * Polls at 1 ms intervals. Useful in tests to ensure all jobs complete.
     *
     * @param timeout   Maximum wait time.
     * @return          true if drained within timeout, false otherwise.
     */
    [[nodiscard]] bool drainAll(
        std::chrono::milliseconds timeout = std::chrono::milliseconds{5000}) const
    {
        auto deadline = balancer::Clock::now() + timeout;
        while (balancer::Clock::now() < deadline)
        {
            bool allIdle = true;
            for (const auto& node : mNodes)
            {
                auto m = node->metrics();
                if (m.queueDepth > 0 ||
                    node->status() == balancer::NodeState::Busy)
                {
                    allIdle = false;
                    break;
                }
            }
            if (allIdle)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        return false;
    }

    /**
     * @brief Start a background thread that periodically writes a
     *        TelemetrySnapshot JSON file to @p path.
     *
     * The file is written atomically: the JSON is first written to
     * `path + ".tmp"`, then renamed to `path`.  Readers (e.g. the Python
     * nn_advisor_harness loop) therefore never observe a partial write.
     *
     * Calling this method a second time stops the previous writer before
     * starting the new one.  The writer is stopped automatically by stop()
     * and the destructor.
     *
     * @param balancer  The Balancer whose aggregate counts are included in the
     *                  snapshot.  Must outlive this call (and the writer thread).
     * @param path      Destination file path.
     * @param interval  Write interval. Default: 500 ms.
     */
    void startSnapshotWriter(
        const balancer::Balancer&       balancer,
        std::string                     path,
        std::chrono::milliseconds       interval = std::chrono::milliseconds{500})
    {
        stopSnapshotWriter();

        mSnapshotWriterRunning.store(true, std::memory_order_relaxed);
        mSnapshotWriterThread = std::thread(
            [this, &balancer, path = std::move(path), interval]()
            {
                while (mSnapshotWriterRunning.load(std::memory_order_relaxed))
                {
                    TelemetrySnapshot snap = captureSnapshot(balancer);
                    const std::string json = formatJson(snap, /*pretty=*/false);

                    // Atomic write: tmp → rename
                    const std::string tmpPath = path + ".tmp";
                    {
                        std::ofstream f(tmpPath,
                                        std::ios::out | std::ios::trunc);
                        if (f.is_open())
                        {
                            f << json;
                        }
                    }
                    std::rename(tmpPath.c_str(), path.c_str());

                    std::this_thread::sleep_for(interval);
                }
            });
    }

    /**
     * @brief Stop the snapshot writer thread if running.  Idempotent.
     *
     * Blocks until the writer thread exits. Called automatically by stop()
     * and the destructor.
     */
    void stopSnapshotWriter()
    {
        mSnapshotWriterRunning.store(false, std::memory_order_relaxed);
        if (mSnapshotWriterThread.joinable())
        {
            mSnapshotWriterThread.join();
        }
    }

    /**
     * @brief Sample cluster metrics and drive a TelemetryAdvisor in one call.
     *
     * Calls `balancer.clusterMetrics()` for the authoritative, richer snapshot
     * (includes `knownNodes`, `overloadedNodes`, latency, and throughput) and
     * passes it directly to `TelemetryAdvisor::evaluate()`.
     *
     * @param advisor   The TelemetryAdvisor to drive.
     * @param balancer  The Balancer whose aggregate counts are sampled.
     * @return          The result of TelemetryAdvisor::evaluate(). An error
     *                  means the feature-graph transition was rejected; the
     *                  cluster remains in its prior alert state.
     */
    [[nodiscard]] fat_p::Expected<void, std::string>
    tick(TelemetryAdvisor& advisor, const Balancer& balancer) const
    {
        return advisor.evaluate(balancer.clusterMetrics());
    }

    /**
     * @brief Capture a point-in-time TelemetrySnapshot for TelemetryAdvisor.
     *
     * Reads LoadMetrics from each node and aggregate counts from the Balancer.
     * The snapshot is self-consistent at the moment of capture; the Balancer
     * and nodes are not locked while TelemetryAdvisor processes it.
     *
     * Derived fields computed here:
     * - overloadedFraction: fraction of total nodes in Overloaded state.
     * - failedFraction: fraction of total nodes in Failed state.
     * - clusterP99LatencyUs: max p99 across all nodes (straggler-sensitive).
     * - clusterP50LatencyUs: mean p50 across nodes with at least one completed job.
     *
     * @param balancer  The Balancer whose aggregate counts are included.
     * @return          A value-type snapshot; safe to pass by copy.
     */
    [[nodiscard]] TelemetrySnapshot captureSnapshot(
        const balancer::Balancer& balancer) const
    {
        TelemetrySnapshot snap;
        snap.nodes.reserve(mNodes.size());

        uint32_t overloadedCount = 0;
        uint32_t failedCount     = 0;
        uint64_t p99Max          = 0;
        uint64_t p50Sum          = 0;
        uint32_t p50Contributors = 0;

        for (const auto& node : mNodes)
        {
            balancer::LoadMetrics m = node->metrics();

            NodeEntry entry;
            entry.nodeId       = m.nodeId.value();
            entry.state        = static_cast<uint8_t>(m.state);
            entry.utilization  = m.utilization;
            entry.queueDepth   = m.queueDepth;
            entry.p50LatencyUs = m.p50LatencyUs;
            entry.p99LatencyUs = m.p99LatencyUs;
            entry.completedJobs = m.completedJobs;
            entry.failedJobs    = m.failedJobs;
            snap.nodes.push_back(entry);

            if (m.state == balancer::NodeState::Idle ||
                m.state == balancer::NodeState::Busy)
            {
                ++snap.activeNodes;
            }
            else if (m.state == balancer::NodeState::Failed ||
                     m.state == balancer::NodeState::Offline)
            {
                ++snap.unavailableNodes;
            }

            if (m.state == balancer::NodeState::Overloaded) { ++overloadedCount; }
            if (m.state == balancer::NodeState::Failed)     { ++failedCount; }

            if (m.p99LatencyUs > p99Max) { p99Max = m.p99LatencyUs; }
            if (m.completedJobs > 0)
            {
                p50Sum += m.p50LatencyUs;
                ++p50Contributors;
            }
        }

        const uint32_t total = static_cast<uint32_t>(mNodes.size());
        if (total > 0)
        {
            snap.overloadedFraction = static_cast<float>(overloadedCount) /
                                       static_cast<float>(total);
            snap.failedFraction     = static_cast<float>(failedCount) /
                                       static_cast<float>(total);
        }

        snap.clusterP99LatencyUs = p99Max;
        snap.clusterP50LatencyUs = (p50Contributors > 0)
            ? (p50Sum / p50Contributors)
            : 0;

        snap.totalSubmitted  = balancer.totalSubmitted();
        snap.inFlightJobs    = balancer.inFlightCount();

        const auto& ac = balancer.admissionControl();
        snap.rejectedRateLimited =
            ac.rejectionCount(balancer::SubmitError::RateLimited);
        snap.rejectedPriorityRejected =
            ac.rejectionCount(balancer::SubmitError::PriorityRejected);
        snap.rejectedClusterSaturated =
            ac.rejectionCount(balancer::SubmitError::ClusterSaturated);

        return snap;
    }

private:
    std::vector<std::unique_ptr<SimulatedNode>> mNodes;
    bool mStarted = false;

    std::atomic<bool> mSnapshotWriterRunning{false};
    std::thread       mSnapshotWriterThread;
};

} // namespace balancer::sim
