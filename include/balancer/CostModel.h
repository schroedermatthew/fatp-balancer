#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: CostModel
  file_role: public_header
  path: include/balancer/CostModel.h
  namespace: [balancer, balancer::detail]
  layer: Learning
  summary: Online learning model — per-node EMA throughput multiplier, node×class affinity Tensor, per-depth DegradationCurve with runtime enable toggle, and JSON persistence.
  api_stability: in_work
  related:
    docs_search: "CostModel"
    tests:
      - tests/test_cost_model.cpp
      - tests/test_CostModel_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file CostModel.h
 * @brief Online learning model for job cost prediction.
 *
 * Phase 1 introduced per-node throughput multipliers via EMA. Phase 2 added a
 * second learning dimension: per-JobClass correction. Phase 3 added a third:
 * per-node DegradationCurve. Phase 5 replaces the nested hash-map per-class
 * correction with a fat_p Tensor-backed AffinityMatrix and adds JSON
 * persistence for the full model state.
 *
 * The full prediction formula is:
 *
 *   predict(job, node) =
 *       estimatedCost
 *     x nodeMultiplier(node)
 *     x affinityMultiplier(node, jobClass)
 *     x degradationMultiplier(node, queueDepthAtDispatch)
 *
 * nodeMultiplier captures overall node speed relative to estimates.
 * affinityMultiplier (Phase 5) captures estimation bias specific to a job
 * class on this node, stored in a fat_p::RowMajorTensor<float> for O(1) cell
 * access and block-level JSON serialisation.
 * degradationMultiplier captures the resource contention penalty when a node's
 * queue is deep.
 *
 * All corrections apply only after warm-up (configurable observation threshold).
 * During cold start, predict() returns the raw estimatedCost — the neutral
 * prediction.
 *
 * Internal helpers EMA, NodeLearningState, and DegradationCurve live in
 * balancer::detail. They are not part of the public API but are defined here
 * because CostModel is header-only.
 *
 * Persistence (Phase 5): save() and load() serialise the full learned state
 * to/from a JSON file via JsonLite. The format version is recorded in the
 * snapshot so future schema changes can be detected.
 *
 * Thread-safety: all public methods are thread-safe.
 *
 * @see CostModel_Companion_Guide.md for EMA design rationale.
 * @see AffinityMatrix.h for the Tensor-backed per-class affinity model.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

// FAT-P Containers
#include "FastHashMap.h"

// FAT-P Domain
#include "JsonLite.h"

// Balancer Interface
#include "balancer/AffinityMatrix.h"
#include "balancer/BalancerConfig.h"
#include "balancer/Job.h"

namespace balancer::detail
{

// ============================================================================
// EMA
// ============================================================================

/**
 * @brief Exponential moving average for a single tracked quantity.
 *
 * value = alpha * observation + (1 - alpha) * value
 *
 * EMA has O(1) update, O(1) read, and no history buffer. It naturally
 * discounts older observations, making it suitable for online learning
 * where node behavior can change over time.
 *
 * @note Internal to CostModel. Not part of the public balancer API.
 */
struct EMA
{
    float value = 1.0f; ///< Current estimate. Initialized to 1.0 (neutral).
    float alpha = 0.1f; ///< Learning rate. Range (0, 1].

    explicit constexpr EMA(float initialValue = 1.0f, float learningRate = 0.1f) noexcept
        : value(initialValue)
        , alpha(learningRate)
    {}

    /**
     * @brief Update with a new observation.
     * @param observation  The new measured value.
     */
    void update(float observation) noexcept
    {
        value = alpha * observation + (1.0f - alpha) * value;
    }

    /**
     * @brief Current estimate.
     * @return EMA value.
     */
    [[nodiscard]] float get() const noexcept { return value; }
};

// ============================================================================
// DegradationCurve
// ============================================================================

/**
 * @brief Queue-depth-aware correction curve for a single node (Phase 3).
 *
 * Jobs running on a node with a deep queue experience resource contention —
 * memory pressure, scheduler latency, cache thrashing — that increases their
 * actual execution time beyond what the raw estimatedCost predicts. The curve
 * models this effect by partitioning queue depth into fixed-width buckets and
 * learning an independent EMA multiplier per bucket.
 *
 * Cold bucket: a bucket with fewer than warmThreshold observations returns 1.0
 * (neutral, no correction). This prevents early noisy observations from
 * distorting routing decisions.
 *
 * @note Internal to CostModel. Not part of the public balancer API.
 */
struct DegradationCurve
{
    /// Hard upper bound on bucket count — prevents array overflow regardless
    /// of DegradationCurveConfig.bucketCount.
    static constexpr uint32_t kMaxBuckets = 32;

    struct Bucket
    {
        EMA      multiplier{1.0f}; ///< EMA of (observed/estimated) ratio at this depth band.
        uint32_t observations = 0; ///< Total updates received for this bucket.
    };

    std::array<Bucket, kMaxBuckets> buckets{};
    uint32_t bucketCount   = 8;
    uint32_t bucketSize    = 4;
    float    alpha         = 0.1f;
    uint32_t warmThreshold = 5;

    void configure(const DegradationCurveConfig& cfg) noexcept
    {
        bucketCount   = (cfg.bucketCount > 0 && cfg.bucketCount <= kMaxBuckets)
                            ? cfg.bucketCount : kMaxBuckets;
        bucketSize    = cfg.bucketSize > 0 ? cfg.bucketSize : 1;
        alpha         = cfg.alpha;
        warmThreshold = cfg.warmThreshold;
    }

    [[nodiscard]] uint32_t bucketIndex(uint32_t depth) const noexcept
    {
        uint32_t idx = depth / bucketSize;
        return idx < bucketCount ? idx : bucketCount - 1;
    }

    void update(uint32_t depth, float ratio) noexcept
    {
        uint32_t idx = bucketIndex(depth);
        buckets[idx].multiplier.alpha = alpha;
        buckets[idx].multiplier.update(ratio);
        ++buckets[idx].observations;
    }

    [[nodiscard]] float get(uint32_t depth) const noexcept
    {
        uint32_t idx = bucketIndex(depth);
        if (buckets[idx].observations < warmThreshold) { return 1.0f; }
        return buckets[idx].multiplier.get();
    }

    [[nodiscard]] uint32_t bucketObservations(uint32_t depth) const noexcept
    {
        return buckets[bucketIndex(depth)].observations;
    }
};

// ============================================================================
// NodeLearningState
// ============================================================================

/**
 * @brief Per-node learned state maintained by CostModel.
 *
 * Phase 1: throughputMultiplier — global per-node estimation correction.
 * Phase 3: degradation — queue-depth-aware DegradationCurve correction.
 * Phase 5: per-class correction is now owned by AffinityMatrix (a member of
 *           CostModel). NodeLearningState no longer carries per-class entries.
 *
 * @note Internal to CostModel. Not part of the public balancer API.
 */
struct NodeLearningState
{
    /// EMA of (observedCost / estimatedCost) ratio across all job classes.
    /// < 1.0: node is faster than estimates. > 1.0: node is slower.
    EMA throughputMultiplier{1.0f, 0.05f};

    /// Number of completed jobs observed across all classes.
    uint32_t observationCount = 0;

    /// Queue-depth-aware degradation correction (Phase 3).
    DegradationCurve degradation;
};

} // namespace balancer::detail

namespace balancer
{

// ============================================================================
// PersistError
// ============================================================================

/**
 * @brief Error codes returned by CostModel::save() and CostModel::load().
 */
enum class PersistError : uint8_t
{
    /// The file path could not be opened for writing (save) or reading (load).
    FileOpenFailed,

    /// The JSON is malformed or missing required fields.
    MalformedJson,

    /// The snapshot dimensions (affinity shape, bucket count) do not match
    /// the current model configuration.
    DimensionMismatch,
};

// ============================================================================
// CostModel
// ============================================================================

/**
 * @brief Online learning model for job cost prediction.
 *
 * Shared across all scheduling policies via ClusterView. Thread-safe.
 *
 * Configuration is supplied via CostModelConfig (defined in BalancerConfig.h).
 *
 * Usage pattern:
 * @code
 * // In a scheduling policy — predict before routing:
 * Cost predicted = costModel.predict(job, candidateNodeId);
 *
 * // In the job completion handler — teach the model:
 * costModel.update(completedJob);
 *
 * // Persist and restore across restarts:
 * auto r = costModel.save("model.json");
 * auto r2 = costModel.load("model.json");
 * @endcode
 */
class CostModel
{
public:
    explicit CostModel(CostModelConfig config = {}) noexcept
        : mConfig(config)
        , mAffinityMatrix(config.affinity)
    {}

    // Non-copyable — mutex and learned state are identity-bound.
    CostModel(const CostModel&)            = delete;
    CostModel& operator=(const CostModel&) = delete;
    CostModel(CostModel&&)                 = delete;
    CostModel& operator=(CostModel&&)      = delete;

    // ---- Prediction --------------------------------------------------------

    /**
     * @brief Predict the actual cost of executing job on the given node.
     *
     * Returns job.estimatedCost during cold start (fewer than warmThreshold
     * observations on this node) — the neutral prediction.
     *
     * After warm-up, applies three learned corrections compounded:
     * 1. nodeMultiplier: overall per-node speed relative to estimates.
     * 2. affinityMultiplier: per-JobClass estimation bias on this node (Phase 5).
     * 3. degradationMultiplier: resource contention at the current queue depth (Phase 3).
     *
     * The final prediction is clamped to [minMultiplier, maxMultiplier] x estimatedCost.
     *
     * @param job     Job to predict cost for.
     * @param nodeId  Candidate node to route to.
     * @return        Predicted actual cost on this node.
     */
    [[nodiscard]] Cost predict(const Job& job, NodeId nodeId) const noexcept
    {
        std::lock_guard lock(mMutex);

        auto* state = mNodeStates.find(nodeId.value());
        if (state == nullptr || !isWarmUnlocked(*state))
        {
            return job.estimatedCost; // cold start: trust the estimate
        }

        float multiplier = state->throughputMultiplier.get();

        // Per-(node, class) affinity correction (Phase 5).
        // AffinityMatrix guards its own warm threshold per cell.
        multiplier *= mAffinityMatrix.get(nodeId.value(), job.jobClass.value());

        // Depth-aware degradation multiplier (Phase 3).
        // Only applied when the DegradationCurve is enabled. The flag is
        // read under mMutex so no additional synchronisation is required.
        if (mDegradationEnabled.load(std::memory_order_relaxed))
        {
            multiplier *= state->degradation.get(job.queueDepthAtDispatch);
        }

        multiplier = clampf(multiplier, mConfig.minMultiplier, mConfig.maxMultiplier);

        auto scaled = static_cast<uint64_t>(
            static_cast<float>(job.estimatedCost.units) * multiplier + 0.5f);
        return Cost{scaled};
    }

    // ---- Update ------------------------------------------------------------

    /**
     * @brief Update the model from a completed job.
     *
     * Updates three learning dimensions:
     * 1. Node-level throughput multiplier.
     * 2. AffinityMatrix cell for (executedBy, jobClass).
     * 3. DegradationCurve bucket for queueDepthAtDispatch.
     *
     * No-op if observedCost is kUnknownCost or estimatedCost is zero.
     *
     * @param completedJob  Job with observedCost and executedBy filled.
     */
    void update(const Job& completedJob) noexcept
    {
        if (completedJob.estimatedCost.units == 0 ||
            completedJob.observedCost == kUnknownCost)
        {
            return;
        }

        float ratio = static_cast<float>(completedJob.observedCost.units)
                    / static_cast<float>(completedJob.estimatedCost.units);

        std::lock_guard lock(mMutex);

        auto& state = mNodeStates[completedJob.executedBy.value()];

        if (state.observationCount == 0)
        {
            state.degradation.configure(mConfig.degradation);
        }

        state.throughputMultiplier.alpha = mConfig.nodeAlpha;
        state.throughputMultiplier.update(ratio);
        ++state.observationCount;

        mAffinityMatrix.update(completedJob.executedBy.value(),
                               completedJob.jobClass.value(),
                               ratio);

        state.degradation.update(completedJob.queueDepthAtDispatch, ratio);
    }

    // ---- Warm state queries ------------------------------------------------

    /**
     * @brief Returns true if the node has enough observations to trust.
     */
    [[nodiscard]] bool isWarm(NodeId nodeId) const noexcept
    {
        std::lock_guard lock(mMutex);
        auto* state = mNodeStates.find(nodeId.value());
        return state != nullptr && isWarmUnlocked(*state);
    }

    /**
     * @brief Number of observations recorded for a node (all classes combined).
     */
    [[nodiscard]] uint32_t observationCount(NodeId nodeId) const noexcept
    {
        std::lock_guard lock(mMutex);
        auto* state = mNodeStates.find(nodeId.value());
        return state == nullptr ? 0 : state->observationCount;
    }

    /**
     * @brief Current clamped throughput multiplier for a node. 1.0 for cold nodes.
     */
    [[nodiscard]] float nodeMultiplier(NodeId nodeId) const noexcept
    {
        std::lock_guard lock(mMutex);
        auto* state = mNodeStates.find(nodeId.value());
        if (state == nullptr || !isWarmUnlocked(*state)) { return 1.0f; }
        return clampf(state->throughputMultiplier.get(),
                      mConfig.minMultiplier, mConfig.maxMultiplier);
    }

    /**
     * @brief Affinity multiplier for a (node, class) pair (Phase 5).
     *
     * Returns 1.0 if the cell has fewer than AffinityMatrixConfig.warmThreshold
     * observations or if the node is unknown.
     *
     * @param nodeId    Node to query.
     * @param jobClass  Job class to query.
     * @return          Learned affinity multiplier.
     */
    [[nodiscard]] float affinityScore(NodeId nodeId, JobClass jobClass) const noexcept
    {
        std::lock_guard lock(mMutex);
        return mAffinityMatrix.get(nodeId.value(), jobClass.value());
    }

    /**
     * @brief Number of observations for a (node, jobClass) pair (Phase 5).
     */
    [[nodiscard]] uint32_t affinityObservations(NodeId nodeId, JobClass jobClass) const noexcept
    {
        std::lock_guard lock(mMutex);
        return mAffinityMatrix.observations(nodeId.value(), jobClass.value());
    }

    /**
     * @brief Returns true if the (node, class) affinity cell is warm.
     */
    [[nodiscard]] bool isAffinityCellWarm(NodeId nodeId, JobClass jobClass) const noexcept
    {
        std::lock_guard lock(mMutex);
        return mAffinityMatrix.isCellWarm(nodeId.value(), jobClass.value());
    }

    /**
     * @brief DegradationCurve multiplier for a (node, queueDepth) pair (Phase 3).
     */
    [[nodiscard]] float degradationMultiplier(NodeId nodeId, uint32_t queueDepth) const noexcept
    {
        std::lock_guard lock(mMutex);
        auto* state = mNodeStates.find(nodeId.value());
        if (state == nullptr) { return 1.0f; }
        return state->degradation.get(queueDepth);
    }

    /**
     * @brief Number of observations in the degradation bucket containing queueDepth.
     */
    [[nodiscard]] uint32_t degradationBucketObservations(NodeId nodeId,
                                                          uint32_t queueDepth) const noexcept
    {
        std::lock_guard lock(mMutex);
        auto* state = mNodeStates.find(nodeId.value());
        if (state == nullptr) { return 0; }
        return state->degradation.bucketObservations(queueDepth);
    }

    // ---- Config access -----------------------------------------------------

    [[nodiscard]] const CostModelConfig& config() const noexcept { return mConfig; }

    // ---- Runtime toggles ---------------------------------------------------

    /**
     * @brief Enable or disable the DegradationCurve correction in predict().
     *
     * When disabled, the depth-aware degradation multiplier is not applied;
     * predict() behaves as if every depth bucket returns 1.0. Existing bucket
     * observations are preserved — re-enabling restores the learned curve.
     *
     * Intended for FeatureSupervisor to activate on `feature.degradation_curve`.
     *
     * Thread-safe (atomic store with release ordering; predict() reads with
     * relaxed ordering under the existing mMutex lock).
     *
     * @param enabled  true to apply degradation correction; false to suppress it.
     */
    void setDegradationEnabled(bool enabled) noexcept
    {
        mDegradationEnabled.store(enabled, std::memory_order_release);
    }

    /// Returns true if the DegradationCurve correction is currently applied.
    [[nodiscard]] bool isDegradationEnabled() const noexcept
    {
        return mDegradationEnabled.load(std::memory_order_acquire);
    }

    // ---- Persistence (Phase 5) ---------------------------------------------

    /**
     * @brief Serialise the full model state to a JSON file.
     *
     * Output format (top-level keys):
     * - `"version"` — integer snapshot format version (currently 1).
     * - `"nodes"` — object keyed by NodeId string; per-node EMA and degradation.
     * - `"affinity"` — AffinityMatrix snapshot (shape + flat value/observation arrays).
     *
     * @param path  Destination file path.
     * @return      Success or PersistError.
     */
    [[nodiscard]] fat_p::Expected<void, PersistError>
    save(const std::string& path) const noexcept
    {
        using namespace fat_p;
        try
        {
            std::ofstream out(path);
            if (!out.is_open()) { return fat_p::unexpected(PersistError::FileOpenFailed); }

            JsonObject root;
            root["version"] = JsonValue{static_cast<int64_t>(1)};

            JsonObject nodesObj;
            JsonValue  affinityVal;

            {
                std::lock_guard lock(mMutex);

                for (auto it = mNodeStates.begin(); it != mNodeStates.end(); ++it)
                {
                    const auto& [nodeKey, state] = *it;
                    JsonObject ns;
                    ns["throughputMultiplier"] =
                        JsonValue{static_cast<double>(state.throughputMultiplier.get())};
                    ns["observationCount"] =
                        JsonValue{static_cast<int64_t>(state.observationCount)};

                    JsonArray buckets;
                    for (uint32_t b = 0; b < state.degradation.bucketCount; ++b)
                    {
                        JsonObject bkt;
                        bkt["value"] = JsonValue{
                            static_cast<double>(
                                state.degradation.buckets[b].multiplier.get())};
                        bkt["observations"] = JsonValue{
                            static_cast<int64_t>(
                                state.degradation.buckets[b].observations)};
                        buckets.emplace_back(std::move(bkt));
                    }
                    ns["degradationBuckets"] = JsonValue{std::move(buckets)};
                    nodesObj[std::to_string(nodeKey)] = JsonValue{std::move(ns)};
                }

                mAffinityMatrix.save(affinityVal);
            }

            root["nodes"]    = JsonValue{std::move(nodesObj)};
            root["affinity"] = std::move(affinityVal);

            out << to_json_string(JsonValue{std::move(root)}, /*pretty=*/true);
            return {};
        }
        catch (...)
        {
            return fat_p::unexpected(PersistError::MalformedJson);
        }
    }

    /**
     * @brief Restore model state from a JSON file previously written by save().
     *
     * On failure the model is reset to cold-start state and the error code is
     * returned. Partial loads are never applied.
     *
     * @param path  Source file path.
     * @return      Success or PersistError.
     */
    [[nodiscard]] fat_p::Expected<void, PersistError>
    load(const std::string& path) noexcept
    {
        using namespace fat_p;

        // RAII guard: reset the model on any failure path (early return or
        // exception) so callers always see a consistent cold-start state.
        // Set committed=true just before the success return to suppress cleanup.
        bool committed = false;
        struct ScopeReset
        {
            bool&       committed;
            CostModel*  self;
            ~ScopeReset()
            {
                if (!committed)
                {
                    std::lock_guard lock(self->mMutex);
                    self->mNodeStates.clear();
                    self->mAffinityMatrix.reset();
                }
            }
        } guard{committed, this};

        try
        {
            std::ifstream in(path);
            if (!in.is_open()) { return fat_p::unexpected(PersistError::FileOpenFailed); }

            std::string content((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
            JsonValue root = parse_json(content);
            if (!root.is_object()) { return fat_p::unexpected(PersistError::MalformedJson); }

            const auto& rootObj = std::get<JsonObject>(root);

            // 1. Parse affinity onto a temporary (validates dimensions).
            auto affinityIt = rootObj.find("affinity");
            if (affinityIt == rootObj.end()) { return fat_p::unexpected(PersistError::MalformedJson); }

            AffinityMatrix tmpAffinity(mConfig.affinity);
            if (!tmpAffinity.load(affinityIt->second))
            {
                return fat_p::unexpected(PersistError::DimensionMismatch);
            }

            // 2. Parse node states.
            auto nodesIt = rootObj.find("nodes");
            if (nodesIt == rootObj.end() || !nodesIt->second.is_object())
            {
                return fat_p::unexpected(PersistError::MalformedJson);
            }

            FastHashMap<uint32_t, detail::NodeLearningState> tmpNodes;
            const auto& nodesObj = std::get<JsonObject>(nodesIt->second);

            for (const auto& [keyStr, nodeVal] : nodesObj)
            {
                if (!nodeVal.is_object()) { return fat_p::unexpected(PersistError::MalformedJson); }
                const auto& ns = std::get<JsonObject>(nodeVal);

                detail::NodeLearningState state;
                state.degradation.configure(mConfig.degradation);

                auto tmIt = ns.find("throughputMultiplier");
                auto ocIt = ns.find("observationCount");
                if (tmIt == ns.end() || ocIt == ns.end())
                {
                    return fat_p::unexpected(PersistError::MalformedJson);
                }

                if (tmIt->second.is_int())
                {
                    state.throughputMultiplier.value =
                        static_cast<float>(std::get<int64_t>(tmIt->second));
                }
                else if (std::holds_alternative<double>(tmIt->second))
                {
                    state.throughputMultiplier.value =
                        static_cast<float>(std::get<double>(tmIt->second));
                }
                if (ocIt->second.is_int())
                {
                    state.observationCount =
                        static_cast<uint32_t>(std::get<int64_t>(ocIt->second));
                }

                auto dbIt = ns.find("degradationBuckets");
                if (dbIt != ns.end() && dbIt->second.is_array())
                {
                    const auto& bkts = std::get<JsonArray>(dbIt->second);
                    uint32_t b = 0;
                    for (const auto& bktVal : bkts)
                    {
                        if (b >= detail::DegradationCurve::kMaxBuckets) { break; }
                        if (!bktVal.is_object()) { continue; }
                        const auto& bktObj = std::get<JsonObject>(bktVal);
                        auto vIt = bktObj.find("value");
                        auto oIt = bktObj.find("observations");
                        if (vIt != bktObj.end() && vIt->second.is_int())
                        {
                            state.degradation.buckets[b].multiplier.value =
                                static_cast<float>(std::get<int64_t>(vIt->second));
                        }
                        else if (vIt != bktObj.end() && std::holds_alternative<double>(vIt->second))
                        {
                            state.degradation.buckets[b].multiplier.value =
                                static_cast<float>(std::get<double>(vIt->second));
                        }
                        if (oIt != bktObj.end() && oIt->second.is_int())
                        {
                            state.degradation.buckets[b].observations =
                                static_cast<uint32_t>(std::get<int64_t>(oIt->second));
                        }
                        ++b;
                    }
                }

                tmpNodes[static_cast<uint32_t>(std::stoul(keyStr))] = std::move(state);
            }

            // 3. Commit under the lock — both structures atomically from caller's view.
            {
                std::lock_guard lock(mMutex);
                mNodeStates.clear();
                for (auto it = tmpNodes.begin(); it != tmpNodes.end(); ++it)
                {
                    const auto& [k, v] = *it;
                    mNodeStates[k] = v;
                }

                mAffinityMatrix.reset();
                JsonValue affinitySnapshot;
                tmpAffinity.save(affinitySnapshot);
                mAffinityMatrix.load(affinitySnapshot);
            }

            committed = true;
            return {};
        }
        catch (...)
        {
            return fat_p::unexpected(PersistError::MalformedJson);
        }
    }

    // ---- Reset (for testing) -----------------------------------------------

    /**
     * @brief Reset all learned state. Primarily for testing.
     */
    void reset() noexcept
    {
        std::lock_guard lock(mMutex);
        mNodeStates.clear();
        mAffinityMatrix.reset();
    }

private:
    [[nodiscard]] bool isWarmUnlocked(const detail::NodeLearningState& s) const noexcept
    {
        return s.observationCount >= mConfig.warmThreshold;
    }

    [[nodiscard]] static float clampf(float v, float lo, float hi) noexcept
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    CostModelConfig mConfig;

    // Runtime toggle: when false, the DegradationCurve multiplier is skipped
    // in predict(). Default true — degradation correction was always applied
    // in prior phases, so the default preserves existing behaviour.
    std::atomic<bool> mDegradationEnabled{true};

    // NodeId uint32_t value -> learning state.
    // Migrated from std::unordered_map to fat_p::FastHashMap (Phase 5):
    // open-addressing gives better cache behaviour on the predict() hot path.
    fat_p::FastHashMap<uint32_t, detail::NodeLearningState> mNodeStates;

    // Tensor-backed per-(node, class) affinity model (Phase 5).
    AffinityMatrix mAffinityMatrix;

    mutable std::mutex mMutex;
};

} // namespace balancer
