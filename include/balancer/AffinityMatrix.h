#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: AffinityMatrix
  file_role: public_header
  path: include/balancer/AffinityMatrix.h
  namespace: balancer
  layer: Learning
  summary: Node × JobClass affinity Tensor — EMA-learned per-cell cost multipliers backed by fat_p::RowMajorTensor, with JSON persistence and matrix-level warmth predicate.
  api_stability: in_work
  related:
    docs_search: "AffinityMatrix"
    tests:
      - tests/test_affinity_matrix.cpp
      - tests/test_AffinityMatrix_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file AffinityMatrix.h
 * @brief Node × JobClass affinity model backed by a fat_p Tensor (Phase 5).
 *
 * AffinityMatrix holds two 2-D `fat_p::RowMajorTensor` of shape
 * `[maxNodes × maxJobClasses]`:
 *
 *  - `mValues`       — EMA multiplier per (node, class) cell. Initialised to
 *                      1.0 (neutral). Updated via `update()` with the ratio
 *                      (observedCost / estimatedCost) for each completed job.
 *  - `mObservations` — Number of updates per cell. Used for warm-start gating.
 *
 * A cell with fewer than `warmThreshold` observations returns 1.0 from `get()`
 * (neutral; no correction applied). This prevents early noisy samples from
 * skewing routing before the model has collected meaningful signal.
 *
 * Index addressing: NodeId.value() → row; JobClass.value() → column. Indices
 * that exceed the tensor bounds are clamped to the last valid row or column.
 * Clamping instead of rejection is required because the learning model is
 * called from completion callbacks; a callback must never fail.
 *
 * Persistence: `save(JsonValue&)` and `load(const JsonValue&)` serialise the
 * full tensor state as flat JSON arrays for efficient file-level persistence.
 *
 * **Prediction formula (after Phase 5):**
 * @code
 * predict(job, node) =
 *     estimatedCost
 *   × nodeMultiplier(node)
 *   × affinityMultiplier(node, jobClass)   ← this component
 *   × degradationMultiplier(node, depth)
 * @endcode
 *
 * @note AffinityMatrix is not thread-safe by itself. CostModel owns it and
 *       protects all accesses with its own mutex.
 *
 * @see CostModel.h for the full prediction pipeline.
 */

#include <cstdint>
#include <string>

// FAT-P Domain
#include "JsonLite.h"
#include "Tensor.h"

// Balancer Core
#include "balancer/BalancerConfig.h"

namespace balancer
{

// ============================================================================
// AffinityMatrix
// ============================================================================

/**
 * @brief 2-D EMA learning matrix for node × JobClass cost multipliers.
 *
 * **Invariant:** Every cell in `mValues` is in [0, +∞). After at least
 * `warmThreshold` observations, `get()` returns the EMA value; before that
 * it returns 1.0 unconditionally.
 *
 * **Complexity model:**
 * - `get()` — O(1): single 2-D index multiply-add then array read.
 * - `update()` — O(1): same addressing plus one EMA multiply-add.
 * - `save()` / `load()` — O(maxNodes × maxJobClasses): full tensor scan.
 *
 * **Concurrency model:** Not internally thread-safe. All callers must hold
 * CostModel's mutex before calling any method.
 *
 * **Error model:** Bounds violations are clamped silently (see class doc).
 * JSON persistence errors return `false` without throwing.
 */
class AffinityMatrix
{
public:
    /**
     * @brief Construct with the given configuration.
     *
     * Allocates two tensors of shape [maxNodes × maxJobClasses]:
     * - `mValues`: initialised to 1.0 (neutral multiplier).
     * - `mObservations`: initialised to 0.
     *
     * @param config  AffinityMatrixConfig from BalancerConfig.costModel.affinity.
     */
    explicit AffinityMatrix(AffinityMatrixConfig config = {}) noexcept
        : mConfig(config)
        , mValues({config.maxNodes, config.maxJobClasses}, 1.0f)
        , mObservations({config.maxNodes, config.maxJobClasses}, 0u)
    {}

    // Non-copyable — the tensors are not cheap to copy and the AffinityMatrix
    // is identity-bound to the CostModel that owns it.
    AffinityMatrix(const AffinityMatrix&)            = delete;
    AffinityMatrix& operator=(const AffinityMatrix&) = delete;
    AffinityMatrix(AffinityMatrix&&)                 = delete;
    AffinityMatrix& operator=(AffinityMatrix&&)      = delete;

    // ---- Learning update ---------------------------------------------------

    /**
     * @brief Update the (nodeIdx, classIdx) cell with a new ratio observation.
     *
     * Applies the EMA formula: `value = alpha * ratio + (1 - alpha) * value`.
     * Out-of-bounds indices are clamped to the tensor edge.
     *
     * @param nodeIdx   Row index — NodeId.value().
     * @param classIdx  Column index — JobClass.value().
     * @param ratio     Observed / estimated cost ratio for this completion.
     */
    void update(uint32_t nodeIdx, uint32_t classIdx, float ratio) noexcept
    {
        nodeIdx  = clampRow(nodeIdx);
        classIdx = clampCol(classIdx);

        float current = mValues(nodeIdx, classIdx);
        mValues(nodeIdx, classIdx) =
            mConfig.alpha * ratio + (1.0f - mConfig.alpha) * current;
        ++mObservations(nodeIdx, classIdx);
    }

    // ---- Prediction query --------------------------------------------------

    /**
     * @brief Return the learned affinity multiplier for (nodeIdx, classIdx).
     *
     * Returns 1.0 if the cell has fewer than warmThreshold observations
     * (cold cell: neutral prediction, no correction applied).
     * Out-of-bounds indices are clamped.
     *
     * @param nodeIdx   Row index — NodeId.value().
     * @param classIdx  Column index — JobClass.value().
     * @return          EMA multiplier. 1.0 = class performs as globally expected
     *                  on this node.
     */
    [[nodiscard]] float get(uint32_t nodeIdx, uint32_t classIdx) const noexcept
    {
        nodeIdx  = clampRow(nodeIdx);
        classIdx = clampCol(classIdx);
        if (mObservations(nodeIdx, classIdx) < mConfig.warmThreshold)
        {
            return 1.0f;
        }
        return mValues(nodeIdx, classIdx);
    }

    /**
     * @brief Number of observations for (nodeIdx, classIdx).
     *
     * @param nodeIdx   Row index — NodeId.value().
     * @param classIdx  Column index — JobClass.value().
     * @return          Observation count. 0 before any updates.
     */
    [[nodiscard]] uint32_t observations(uint32_t nodeIdx, uint32_t classIdx) const noexcept
    {
        nodeIdx  = clampRow(nodeIdx);
        classIdx = clampCol(classIdx);
        return mObservations(nodeIdx, classIdx);
    }

    /**
     * @brief Return true if (nodeIdx, classIdx) has at least warmThreshold observations.
     *
     * @param nodeIdx   Row index.
     * @param classIdx  Column index.
     * @return          true if the cell's EMA is trusted.
     */
    [[nodiscard]] bool isCellWarm(uint32_t nodeIdx, uint32_t classIdx) const noexcept
    {
        return observations(nodeIdx, classIdx) >= mConfig.warmThreshold;
    }

    /**
     * @brief Return true if at least `fraction` of all cells are warm.
     *
     * A cell is warm when it has accumulated at least `warmThreshold`
     * observations. This is the readiness predicate used by FeatureSupervisor
     * to set `feature.affinity_matrix_warm`: AffinityRouting is only
     * meaningful when enough of the matrix has collected signal.
     *
     * The fraction threshold (what share of total cells must be warm) belongs
     * in the caller's configuration — typically AdvisorConfig — not hardcoded
     * here. This method only applies the check; it does not own the threshold.
     *
     * @param fraction  Required warm fraction in [0.0, 1.0]. A value of 0.0
     *                  returns true for any non-empty matrix. A value of 1.0
     *                  requires every cell to be warm.
     * @return          true if the warm cell count divided by total cell count
     *                  is >= fraction; false for an empty matrix.
     */
    [[nodiscard]] bool isMatrixWarm(float fraction) const noexcept
    {
        const uint32_t total = mConfig.maxNodes * mConfig.maxJobClasses;
        if (total == 0) { return false; }

        uint32_t warm = 0;
        for (uint32_t r = 0; r < mConfig.maxNodes; ++r)
        {
            for (uint32_t c = 0; c < mConfig.maxJobClasses; ++c)
            {
                if (mObservations(r, c) >= mConfig.warmThreshold) { ++warm; }
            }
        }

        return static_cast<float>(warm) / static_cast<float>(total) >= fraction;
    }

    // ---- Configuration access ----------------------------------------------

    [[nodiscard]] const AffinityMatrixConfig& config() const noexcept { return mConfig; }

    // ---- Reset (for testing) -----------------------------------------------

    /**
     * @brief Reset all learned state to neutral. Primarily for testing.
     */
    void reset() noexcept
    {
        mValues.fill(1.0f);
        mObservations.fill(0u);
    }

    // ---- Persistence -------------------------------------------------------

    /**
     * @brief Serialise the full affinity state into a JsonValue object.
     *
     * Output format:
     * @code{.json}
     * {
     *   "maxNodes": 64,
     *   "maxJobClasses": 16,
     *   "warmThreshold": 5,
     *   "alpha": 0.1,
     *   "values": [ 1.0, 1.0, ... ],         // flat row-major array
     *   "observations": [ 0, 0, ... ]         // flat row-major array
     * }
     * @endcode
     *
     * The flat arrays have length maxNodes × maxJobClasses. Row r, column c
     * maps to index r * maxJobClasses + c.
     *
     * @param j  Output JsonValue. Overwritten with an object node.
     */
    void save(fat_p::JsonValue& j) const
    {
        using namespace fat_p;
        JsonObject obj;
        obj["maxNodes"]      = JsonValue{static_cast<int64_t>(mConfig.maxNodes)};
        obj["maxJobClasses"] = JsonValue{static_cast<int64_t>(mConfig.maxJobClasses)};
        obj["warmThreshold"] = JsonValue{static_cast<int64_t>(mConfig.warmThreshold)};
        obj["alpha"]         = JsonValue{static_cast<double>(mConfig.alpha)};

        JsonArray vals;
        JsonArray obs;
        vals.reserve(mConfig.maxNodes * mConfig.maxJobClasses);
        obs.reserve(mConfig.maxNodes * mConfig.maxJobClasses);

        for (uint32_t r = 0; r < mConfig.maxNodes; ++r)
        {
            for (uint32_t c = 0; c < mConfig.maxJobClasses; ++c)
            {
                vals.emplace_back(static_cast<double>(mValues(r, c)));
                obs.emplace_back(static_cast<int64_t>(mObservations(r, c)));
            }
        }

        obj["values"]       = JsonValue{std::move(vals)};
        obj["observations"] = JsonValue{std::move(obs)};
        j = JsonValue{std::move(obj)};
    }

    /**
     * @brief Restore affinity state from a JsonValue previously written by `save()`.
     *
     * The saved maxNodes and maxJobClasses must exactly match the current
     * configuration. If they do not, or if the JSON structure is malformed,
     * `load()` returns false and leaves the matrix in a neutral (reset) state.
     *
     * @param j  JsonValue object produced by a prior `save()` call.
     * @return   true on success; false if the snapshot is incompatible or malformed.
     */
    bool load(const fat_p::JsonValue& j)
    {
        using namespace fat_p;

        if (!j.is_object()) { return false; }
        const auto& obj = std::get<JsonObject>(j);

        auto readU32 = [&](const std::string& key, uint32_t& out) -> bool
        {
            auto it = obj.find(key);
            if (it == obj.end() || !it->second.is_int()) { return false; }
            out = static_cast<uint32_t>(std::get<int64_t>(it->second));
            return true;
        };

        uint32_t savedNodes = 0, savedClasses = 0;
        if (!readU32("maxNodes", savedNodes) || !readU32("maxJobClasses", savedClasses))
        {
            return false;
        }
        if (savedNodes != mConfig.maxNodes || savedClasses != mConfig.maxJobClasses)
        {
            return false; // dimension mismatch — cannot restore into this matrix
        }

        auto valIt = obj.find("values");
        auto obsIt = obj.find("observations");
        if (valIt == obj.end() || !valIt->second.is_array()) { return false; }
        if (obsIt == obj.end() || !obsIt->second.is_array()) { return false; }

        const auto& vals = std::get<JsonArray>(valIt->second);
        const auto& obs  = std::get<JsonArray>(obsIt->second);
        const size_t expected = static_cast<size_t>(mConfig.maxNodes) * mConfig.maxJobClasses;
        if (vals.size() != expected || obs.size() != expected) { return false; }

        reset(); // start neutral; overwrite below

        for (uint32_t r = 0; r < mConfig.maxNodes; ++r)
        {
            for (uint32_t c = 0; c < mConfig.maxJobClasses; ++c)
            {
                size_t idx = static_cast<size_t>(r) * mConfig.maxJobClasses + c;
                const auto& vj = vals[idx];
                const auto& oj = obs[idx];

                float v = 1.0f;
                // JsonLite serializes whole-number doubles (e.g., 1.0) as integer
                // literals without a decimal point, so the parser produces int64_t,
                // not double. Check is_int() first to avoid std::bad_variant_access.
                if (vj.is_int())
                {
                    v = static_cast<float>(std::get<int64_t>(vj));
                }
                else if (std::holds_alternative<double>(vj))
                {
                    v = static_cast<float>(std::get<double>(vj));
                }

                uint32_t o = 0;
                if (oj.is_int())
                {
                    o = static_cast<uint32_t>(std::get<int64_t>(oj));
                }

                mValues(r, c)       = v;
                mObservations(r, c) = o;
            }
        }

        return true;
    }

private:
    [[nodiscard]] uint32_t clampRow(uint32_t r) const noexcept
    {
        return r < mConfig.maxNodes ? r : mConfig.maxNodes - 1u;
    }
    [[nodiscard]] uint32_t clampCol(uint32_t c) const noexcept
    {
        return c < mConfig.maxJobClasses ? c : mConfig.maxJobClasses - 1u;
    }

    AffinityMatrixConfig                             mConfig;
    fat_p::RowMajorTensor<float>                     mValues;
    fat_p::RowMajorTensor<uint32_t>                  mObservations;
};

} // namespace balancer
