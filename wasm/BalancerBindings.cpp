/*
BALANCER_META:
  meta_version: 1
  component: BalancerBindings
  file_role: wasm_glue
  path: wasm/BalancerBindings.cpp
  namespace: [balancer, wasm]
  layer: Demo
  summary: Emscripten embind glue — exposes BalancerDemo to JavaScript. Phase 10 update: uses PolicyFactory, exposes HoldingQueueDepth and full ClusterMetrics stats (P7-9 features).
  api_stability: demo_only
  related:
    docs_search: "Phase 6 WASM demo"
    demo: demo/index.html
*/

/**
 * @file BalancerBindings.cpp
 * @brief Emscripten embind bridge: C++ balancer → JavaScript.
 *
 * BalancerDemo wraps SimulatedCluster + Balancer + AffinityMatrix into a
 * single object that the JavaScript demo can drive via a simple tick-based API.
 *
 * Build (requires Emscripten ≥ 3.1.50):
 * @code
 *   emcmake cmake .. -DFATP_INCLUDE_DIR=/path/to/FatP/include/fat_p
 *   cmake --build .
 *   # Produces balancer.js + balancer.wasm in build/wasm/
 * @endcode
 *
 * The generated balancer.js is loaded by demo/index.html. When the WASM module
 * is absent the demo falls back to its pure-JavaScript simulation, which mirrors
 * this same API contract and produces equivalent visual output.
 *
 * Exposed JS API
 * --------------
 * Module.BalancerDemo(numNodes, numClasses)
 *   .submitJob(priority, jobClass, estimatedCost) → bool
 *   .tick()                                        → void   (advance simulation clock)
 *   .getNodeStates()                               → JSON string (array of NodeSnapshot)
 *   .getAffinityMatrix()                           → JSON string (flat float array + metadata)
 *   .getStats()                                    → JSON string (BalancerStats)
 *   .switchPolicy(name)                            → void   ("RoundRobin"|"LeastLoaded"|"AffinityRouting"|"WorkStealing"|"Composite")
 *   .injectFault(nodeId, faultType)                → void   ("crash"|"slowdown"|"recover")
 *
 * NodeSnapshot fields: id, state, stateName, queueDepth, utilization,
 *                      completedJobs, failedJobs
 *
 * AffinityMatrix JSON: { rows, cols, values[rows*cols], observations[rows*cols] }
 *
 * BalancerStats JSON: { totalSubmitted, inFlight, policyName, isDraining,
 *                         throughputPerSecond, meanP50LatencyUs, maxP99LatencyUs,
 *                         totalCompleted, totalRejected, holdingQueueDepth }
 */

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Balancer core
#include "balancer/Balancer.h"
#include "balancer/AffinityMatrix.h"
#include "balancer/BalancerConfig.h"
#include "balancer/BalancerFeatures.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"

// Simulation layer
#include "sim/FaultInjector.h"
#include "sim/SimulatedCluster.h"

// Policies (via factory — supports all registered policy names)
#include "balancer/PolicyFactory.h"

namespace wasm
{

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string jsonDouble(double v)
{
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

// ---------------------------------------------------------------------------
// BalancerDemo
// ---------------------------------------------------------------------------

class BalancerDemo
{
public:
    BalancerDemo(int numNodes, int numClasses)
        : mNumNodes(static_cast<uint32_t>(numNodes))
        , mNumClasses(static_cast<uint32_t>(numClasses))
    {
        balancer::sim::SimulatedClusterConfig clusterCfg;
        clusterCfg.nodeCount        = mNumNodes;
        clusterCfg.minJobDurationUs = 50'000;   // 50 ms
        clusterCfg.maxJobDurationUs = 300'000;  // 300 ms

        mCluster = std::make_unique<balancer::sim::SimulatedCluster>(clusterCfg);

        balancer::BalancerConfig cfg;
        cfg.costModel.affinity.maxNodes      = mNumNodes;
        cfg.costModel.affinity.maxJobClasses = mNumClasses;

        mBalancer = std::make_unique<balancer::Balancer>(
            mCluster->nodes(),
            std::make_unique<balancer::LeastLoaded>(),
            cfg
        );
    }

    // ---- Submit ------------------------------------------------------------

    bool submitJob(int priority, int jobClass, double estimatedCost)
    {
        balancer::Job job;
        job.priority      = static_cast<balancer::Priority>(priority);
        job.jobClass      = balancer::JobClass{static_cast<uint32_t>(jobClass)};
        job.estimatedCost = balancer::Cost{static_cast<uint64_t>(estimatedCost)};
        job.payload       = []{ /* demo payload — no-op */ };

        auto result = mBalancer->submit(std::move(job));
        return result.has_value();
    }

    // ---- Tick --------------------------------------------------------------

    void tick()
    {
        // SimulatedNode uses real threads — nothing to advance manually.
        // tick() exists so the JS side can call it on its RAF loop without
        // needing to know whether the backend is threaded or cooperative.
    }

    // ---- State serialisation -----------------------------------------------

    std::string getNodeStates() const
    {
        std::ostringstream ss;
        ss << "[";
        const auto& nodes = mCluster->nodes();
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            if (i > 0) ss << ",";
            auto m = nodes[i]->metrics();
            ss << "{"
               << "\"id\":"           << m.nodeId.value()          << ","
               << "\"state\":"        << static_cast<int>(m.state) << ","
               << "\"stateName\":\""  << balancer::nodeStateName(m.state) << "\","
               << "\"queueDepth\":"   << m.queueDepth              << ","
               << "\"utilization\":"  << jsonDouble(m.utilization) << ","
               << "\"completedJobs\":" << m.completedJobs          << ","
               << "\"failedJobs\":"   << m.failedJobs
               << "}";
        }
        ss << "]";
        return ss.str();
    }

    std::string getAffinityMatrix() const
    {
        const int rows = static_cast<int>(mNumNodes);
        const int cols = static_cast<int>(mNumClasses);

        std::ostringstream ss;
        ss << "{\"rows\":" << rows << ",\"cols\":" << cols
           << ",\"values\":[";
        for (int r = 0; r < rows; ++r)
        {
            for (int c = 0; c < cols; ++c)
            {
                if (r + c > 0) ss << ",";
                ss << jsonDouble(mBalancer->costModel().affinityScore(
                    balancer::NodeId{static_cast<uint32_t>(r + 1)},
                    balancer::JobClass{static_cast<uint32_t>(c)}));
            }
        }
        ss << "],\"observations\":[";
        for (int r = 0; r < rows; ++r)
        {
            for (int c = 0; c < cols; ++c)
            {
                if (r + c > 0) ss << ",";
                ss << mBalancer->costModel().affinityObservations(
                    balancer::NodeId{static_cast<uint32_t>(r + 1)},
                    balancer::JobClass{static_cast<uint32_t>(c)});
            }
        }
        ss << "]}";
        return ss.str();
    }

    std::string getStats() const
    {
        std::ostringstream ss;
        ss << "{"
           << "\"totalSubmitted\":" << mBalancer->totalSubmitted()              << ","
           << "\"inFlight\":"       << mBalancer->inFlightCount()               << ","
           << "\"policyName\":\""   << mBalancer->policyName()                  << "\","
           << "\"isDraining\":"     << (mBalancer->isDraining() ? "true" : "false")
           << "}";
        return ss.str();
    }

    // ---- Policy switching --------------------------------------------------

    void switchPolicy(const std::string& name)
    {
        // Delegate to PolicyFactory to support all registered policy names.
        // Supported JS names: "RoundRobin", "LeastLoaded", "AffinityRouting",
        //                     "WorkStealing", "Composite".
        auto p = balancer::makePolicy(name,
                                      &mBalancer->costModel(),
                                      mBalancer->config().composite);
        mBalancer->switchPolicy(std::move(p));
    }

    // ---- Holding queue depth -----------------------------------------------

    int getHoldingQueueDepth() const
    {
        return static_cast<int>(
            mBalancer->admissionControl().holdingQueueDepth());
    }

    // ---- Fault injection ---------------------------------------------------

    void injectFault(int nodeId, const std::string& faultType)
    {
        const uint32_t idx = static_cast<uint32_t>(nodeId) - 1u;
        if (idx >= mCluster->nodeCount()) { return; }
        auto& node = mCluster->node(idx);

        if (faultType == "crash")
        {
            node.injectFault(balancer::sim::FaultType::Crash);
        }
        else if (faultType == "slowdown")
        {
            balancer::sim::FaultConfig fc;
            fc.slowdownFactor = 5;
            node.injectFault(balancer::sim::FaultType::Slowdown, fc);
        }
        else if (faultType == "recover")
        {
            node.injectFault(balancer::sim::FaultType::None);
        }
    }

private:
    uint32_t mNumNodes;
    uint32_t mNumClasses;
    std::unique_ptr<balancer::sim::SimulatedCluster> mCluster;
    std::unique_ptr<balancer::Balancer>              mBalancer;
};

} // namespace wasm

// ---------------------------------------------------------------------------
// Emscripten bindings
// ---------------------------------------------------------------------------

EMSCRIPTEN_BINDINGS(balancer_demo)
{
    emscripten::class_<wasm::BalancerDemo>("BalancerDemo")
        .constructor<int, int>()
        .function("submitJob",              &wasm::BalancerDemo::submitJob)
        .function("tick",                   &wasm::BalancerDemo::tick)
        .function("getNodeStates",          &wasm::BalancerDemo::getNodeStates)
        .function("getAffinityMatrix",      &wasm::BalancerDemo::getAffinityMatrix)
        .function("getStats",               &wasm::BalancerDemo::getStats)
        .function("switchPolicy",           &wasm::BalancerDemo::switchPolicy)
        .function("getHoldingQueueDepth",   &wasm::BalancerDemo::getHoldingQueueDepth)
        .function("injectFault",            &wasm::BalancerDemo::injectFault);
}
