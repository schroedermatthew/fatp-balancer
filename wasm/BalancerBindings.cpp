/*
BALANCER_META:
  meta_version: 1
  component: BalancerBindings
  file_role: wasm_glue
  path: wasm/BalancerBindings.cpp
  namespace: [balancer, wasm]
  layer: Demo
  summary: >
    Emscripten embind glue — exposes BalancerDemo to JavaScript. Phase 10+
    update: wires FeatureSupervisor + TelemetryAdvisor into tick(), exposes
    getActiveAlert() and pushNNAlert() for browser-side NN integration,
    completes getStats() with throughput/latency/completed/rejected fields.
  api_stability: demo_only
  related:
    docs_search: "Phase 6 WASM demo"
    demo: demo/index.html
*/

/**
 * @file BalancerBindings.cpp
 * @brief Emscripten embind bridge: C++ balancer → JavaScript.
 *
 * BalancerDemo wraps SimulatedCluster + Balancer + FeatureSupervisor +
 * TelemetryAdvisor into a single object that the JavaScript demo can drive
 * via a simple tick-based API.
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
 *   .tick()                                        → void
 *   .getNodeStates()                               → JSON string (array of NodeSnapshot)
 *   .getAffinityMatrix()                           → JSON string (flat float array + metadata)
 *   .getStats()                                    → JSON string (BalancerStats)
 *   .switchPolicy(name)                            → void
 *   .injectFault(nodeId, faultType)                → void
 *   .getActiveAlert()                              → string
 *   .pushNNAlert(alertName)                        → bool
 *
 * NodeSnapshot fields: id, state, stateName, queueDepth, utilization,
 *                      completedJobs, failedJobs
 *
 * AffinityMatrix JSON: { rows, cols, values[rows*cols], observations[rows*cols] }
 *
 * BalancerStats JSON: { totalSubmitted, totalCompleted, totalRejected,
 *                       inFlight, holdingQueueDepth, policyName, isDraining,
 *                       throughputPerSecond, meanP50LatencyUs, maxP99LatencyUs,
 *                       activeAlert }
 *
 * pushNNAlert(alertName):
 *   Allows a browser-side NN model (or user UI) to push an alert directly to
 *   the FeatureSupervisor, bypassing TelemetryAdvisor threshold logic. Valid
 *   alertName values: "", "alert.none", "alert.overload", "alert.latency",
 *   "alert.node_fault". Returns true on success, false if the name is unknown
 *   or the feature graph rejects the transition.
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
#include "balancer/FeatureSupervisor.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"
#include "balancer/TelemetryAdvisor.h"

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

        mSupervisor = std::make_unique<balancer::FeatureSupervisor>(*mBalancer);
        mAdvisor    = std::make_unique<balancer::TelemetryAdvisor>(*mSupervisor);
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
        // Sample live cluster metrics and run TelemetryAdvisor. Any alert
        // transition is applied to the FeatureSupervisor feature graph, which
        // may trigger a policy switch (e.g. overload → WorkStealing).
        const balancer::ClusterMetrics cm = mBalancer->clusterMetrics();
        (void)mAdvisor->evaluate(cm);
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
               << "\"id\":"            << m.nodeId.value()          << ","
               << "\"state\":"         << static_cast<int>(m.state) << ","
               << "\"stateName\":\""   << balancer::nodeStateName(m.state) << "\","
               << "\"queueDepth\":"    << m.queueDepth              << ","
               << "\"utilization\":"   << jsonDouble(m.utilization) << ","
               << "\"completedJobs\":" << m.completedJobs           << ","
               << "\"failedJobs\":"    << m.failedJobs
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
        const balancer::ClusterMetrics cm = mBalancer->clusterMetrics();
        std::ostringstream ss;
        ss << "{"
           << "\"totalSubmitted\":"     << cm.totalSubmitted                           << ","
           << "\"totalCompleted\":"     << cm.totalCompleted                           << ","
           << "\"totalRejected\":"      << cm.totalRejected                            << ","
           << "\"inFlight\":"           << mBalancer->inFlightCount()                  << ","
           << "\"holdingQueueDepth\":"  << mBalancer->admissionControl()
                                                    .holdingQueueDepth()               << ","
           << "\"policyName\":\""       << mBalancer->policyName()                     << "\","
           << "\"isDraining\":"         << (mBalancer->isDraining() ? "true" : "false")<< ","
           << "\"throughputPerSecond\":" << jsonDouble(cm.throughputPerSecond)          << ","
           << "\"meanP50LatencyUs\":"   << cm.meanP50LatencyUs                        << ","
           << "\"maxP99LatencyUs\":"    << cm.maxP99LatencyUs                         << ","
           << "\"activeAlert\":\""      << mSupervisor->activeAlert()                  << "\""
           << "}";
        return ss.str();
    }

    // ---- Policy switching --------------------------------------------------

    void switchPolicy(const std::string& name)
    {
        auto p = balancer::makePolicy(name,
                                      &mBalancer->costModel(),
                                      mBalancer->config().composite);
        mBalancer->switchPolicy(std::move(p));
    }

    // ---- Alert query -------------------------------------------------------

    std::string getActiveAlert() const
    {
        return std::string(mSupervisor->activeAlert());
    }

    // ---- NN alert push -----------------------------------------------------

    /**
     * @brief Push an alert from a browser-side NN model directly to the
     *        FeatureSupervisor, bypassing TelemetryAdvisor threshold logic.
     *
     * This is the WASM-appropriate alternative to NNAdvisor: the JS side
     * runs inference (or presents a UI control) and pushes the result here.
     * The C++ side validates the name and applies the feature-graph transition.
     *
     * @return true on success, false if the name is unknown or the transition
     *         is rejected by the feature graph.
     */
    bool pushNNAlert(const std::string& alertName)
    {
        auto result = mSupervisor->applyChanges(alertName);
        return result.has_value();
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
    std::unique_ptr<balancer::FeatureSupervisor>     mSupervisor;
    std::unique_ptr<balancer::TelemetryAdvisor>      mAdvisor;
};

} // namespace wasm

// ---------------------------------------------------------------------------
// Emscripten bindings
// ---------------------------------------------------------------------------

EMSCRIPTEN_BINDINGS(balancer_demo)
{
    emscripten::class_<wasm::BalancerDemo>("BalancerDemo")
        .constructor<int, int>()
        .function("submitJob",            &wasm::BalancerDemo::submitJob)
        .function("tick",                 &wasm::BalancerDemo::tick)
        .function("getNodeStates",        &wasm::BalancerDemo::getNodeStates)
        .function("getAffinityMatrix",    &wasm::BalancerDemo::getAffinityMatrix)
        .function("getStats",             &wasm::BalancerDemo::getStats)
        .function("switchPolicy",         &wasm::BalancerDemo::switchPolicy)
        .function("injectFault",          &wasm::BalancerDemo::injectFault)
        .function("getActiveAlert",       &wasm::BalancerDemo::getActiveAlert)
        .function("pushNNAlert",          &wasm::BalancerDemo::pushNNAlert);
}
