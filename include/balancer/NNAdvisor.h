#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: NNAdvisor
  file_role: public_header
  path: include/balancer/NNAdvisor.h
  namespace: balancer
  layer: Core
  summary: >
    File-backed neural-network advisor: reads a JSON inference file written by
    an external NN process and drives FeatureSupervisor::applyChanges() with the
    resulting alert name. Peer to TelemetryAdvisor — both satisfy the Advisor
    concept and are interchangeable at the call site. Reads the file unconditionally
    on every evaluate() call; tick frequency (~15 fps) makes I/O cost negligible.
  api_stability: in_work
  related:
    docs_search: "NNAdvisor"
    tests:
      - tests/test_NNAdvisor_HeaderSelfContained.cpp
      - tests/test_nn_advisor.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file NNAdvisor.h
 * @brief File-backed NN inference advisor for FeatureSupervisor.
 *
 * NNAdvisor replaces TelemetryAdvisor's hard-coded threshold comparisons with
 * an external neural-network process. The NN reads cluster telemetry (e.g.
 * TelemetrySnapshot JSON), runs inference, and writes a small JSON file
 * declaring which alert should be active. NNAdvisor reads that file each tick
 * and calls FeatureSupervisor::applyChanges() with the result.
 *
 * ### Inference file schema
 *
 * The file must be a JSON object with an `"active_alert"` string field:
 *
 * ```json
 * { "active_alert": "alert.overload" }
 * ```
 *
 * Valid values for `"active_alert"`:
 * | Value                | Meaning                          |
 * |----------------------|----------------------------------|
 * | `""`                 | Healthy — no alert               |
 * | `"alert.none"`       | Healthy — no alert (synonym)     |
 * | `"alert.overload"`   | Cluster sustained-overload alert |
 * | `"alert.latency"`    | Cluster high-latency alert       |
 * | `"alert.node_fault"` | Node-fault alert                 |
 *
 * Any additional fields (e.g. `"confidence"`) are silently ignored, allowing
 * the NN process to write richer diagnostics without breaking the C++ side.
 *
 * ### Read-on-every-tick
 *
 * `evaluate()` reads and parses the inference file unconditionally on every
 * call. The tick loop runs at ~15 fps (66 ms per frame); a full file read of a
 * ~50-byte JSON file costs ~10–20 µs — well under 0.1% of the frame budget.
 * The simplicity of always reading outweighs any benefit from caching, and
 * avoids the sub-second mtime granularity hazard of stat-based gating.
 *
 * If the file does not exist, `evaluate()` returns an error and leaves the
 * supervisor in its current state. The next call retries.
 *
 * ### Comparison with TelemetryAdvisor
 *
 * | Property            | TelemetryAdvisor            | NNAdvisor                   |
 * |---------------------|-----------------------------|-----------------------------|
 * | Alert source        | In-process threshold logic  | External NN inference file  |
 * | `evaluate()` arg    | `const ClusterMetrics&`     | None (file is the input)    |
 * | I/O                 | None                        | File read + JSON parse      |
 * | Failure modes       | applyChanges rejection only | File I/O, JSON parse, bad alert |
 * | Reconfiguration     | Replace TelemetryThresholds | Replace NN model externally |
 *
 * Both classes satisfy the `Advisor` concept and are interchangeable at the
 * call site.
 *
 * ### Transferability
 *
 * This header has no dependency on any `sim/` header and may be included from
 * production code and WASM bindings.
 *
 * @see TelemetryAdvisor.h for the threshold-based alternative.
 * @see FeatureSupervisor.h for the feature graph and policy transition logic.
 * @see BalancerFeatures.h for the canonical alert name constants.
 */

#include <string>
#include <string_view>

#include "Expected.h"
#include "JsonLite.h"

#include "balancer/BalancerFeatures.h"
#include "balancer/FeatureSupervisor.h"

namespace balancer
{

// ============================================================================
// NNAdvisor
// ============================================================================

/**
 * @brief File-backed neural-network advisor.
 *
 * Reads a JSON inference file written by an external NN process and drives
 * FeatureSupervisor::applyChanges() with the resulting alert name.
 *
 * ## Intent
 *
 * Decouple alert determination from the C++ runtime entirely. The NN process
 * owns the alert signal; NNAdvisor is the thin boundary that translates the
 * file output into a FeatureSupervisor transition. NNAdvisor has no knowledge
 * of what the alerts mean — that contract lives in the FeatureManager graph
 * owned by FeatureSupervisor.
 *
 * ## Invariant
 *
 * After a successful `evaluate()` call, the supervisor's active alert matches
 * the alert in the inference file at the time of that call.
 *
 * ## Complexity model
 *
 * `evaluate()` performs one file read, one JSON parse, and one string
 * comparison per call — all bounded by the file size, which is expected to be
 * a few hundred bytes (~10–20 µs at the tick rates used in this project).
 * The `applyChanges()` call is O(F) in the number of features, a small
 * constant.
 *
 * ## Concurrency model
 *
 * NNAdvisor has no internal mutable state; all mutable state lives in the
 * FeatureSupervisor (which uses `MutexSynchronizationPolicy`). Concurrent
 * `evaluate()` calls are therefore safe as long as the supervisor is
 * thread-safe, but callers should avoid concurrent evaluation — the second
 * call is redundant and adds contention.
 *
 * ## Error model
 *
 * `evaluate()` returns `fat_p::Expected<void, std::string>` — the same type
 * as `TelemetryAdvisor::evaluate()`. Failure cases:
 * - File does not exist or cannot be opened.
 * - JSON parse error.
 * - `"active_alert"` field is absent.
 * - `"active_alert"` value is not a recognised alert constant.
 * - `applyChanges()` rejects the transition (graph constraint violation).
 *
 * On any error the supervisor remains in its prior state. The next call
 * retries the read from scratch.
 */
class NNAdvisor
{
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    /**
     * @brief Constructs an NNAdvisor wired to @p supervisor.
     *
     * @param supervisor        The FeatureSupervisor to drive. Must outlive
     *                          this object.
     * @param inferenceFilePath Path to the JSON inference file written by the
     *                          external NN process.
     */
    explicit NNAdvisor(FeatureSupervisor& supervisor,
                       std::string        inferenceFilePath) noexcept
        : mSupervisor(supervisor)
        , mInferenceFilePath(std::move(inferenceFilePath))
    {
    }

    NNAdvisor(const NNAdvisor&)            = delete;
    NNAdvisor& operator=(const NNAdvisor&) = delete;
    NNAdvisor(NNAdvisor&&)                 = delete;
    NNAdvisor& operator=(NNAdvisor&&)      = delete;

    // -------------------------------------------------------------------------
    // Primary API
    // -------------------------------------------------------------------------

    /**
     * @brief Read the inference file and apply the alert to the supervisor.
     *
     * Reads and parses the inference file on every call, validates the
     * `"active_alert"` value, and calls `applyChanges()`. The call is
     * idempotent with respect to the supervisor: if the file's alert matches
     * the current active alert, `applyChanges()` returns immediately.
     *
     * @return `{}` on success. A descriptive error string on any failure; the
     *         supervisor remains in its prior state.
     */
    [[nodiscard]] fat_p::Expected<void, std::string>
    evaluate()
    {
        // ---- Read and parse the file ----------------------------------------
        fat_p::JsonValue root;
        try
        {
            root = fat_p::load_json_from_file(mInferenceFilePath);
        }
        catch (const std::exception& e)
        {
            return fat_p::unexpected(
                std::string("NNAdvisor: failed to read/parse inference file '") +
                mInferenceFilePath + "': " + e.what());
        }

        if (!root.is_object())
        {
            return fat_p::unexpected(
                "NNAdvisor: inference file root must be a JSON object");
        }

        const auto& obj = std::get<fat_p::JsonObject>(root);
        const auto  it  = obj.find("active_alert");

        if (it == obj.end())
        {
            return fat_p::unexpected(
                "NNAdvisor: inference file missing required field 'active_alert'");
        }

        if (!it->second.is_string())
        {
            return fat_p::unexpected(
                "NNAdvisor: 'active_alert' must be a string");
        }

        const std::string& alertValue = std::get<std::string>(it->second);

        // ---- Validate the alert name ----------------------------------------
        if (!isKnownAlert(alertValue))
        {
            return fat_p::unexpected(
                "NNAdvisor: unknown alert name '" + alertValue +
                "'. Valid values: \"\", \"" +
                std::string(features::kAlertNone)      + "\", \"" +
                std::string(features::kAlertOverload)  + "\", \"" +
                std::string(features::kAlertLatency)   + "\", \"" +
                std::string(features::kAlertNodeFault) + "\"");
        }

        // ---- Apply to supervisor --------------------------------------------
        return mSupervisor.applyChanges(alertValue);
    }

    // -------------------------------------------------------------------------
    // Query
    // -------------------------------------------------------------------------

    /**
     * @brief Returns the alert name that was active after the most recent
     *        successful `evaluate()` call, or `""` if no alert is active.
     *
     * Delegates to the supervisor — this is the ground-truth alert state.
     */
    [[nodiscard]] std::string_view currentAlert() const noexcept
    {
        return mSupervisor.activeAlert();
    }

    /// Returns the path to the inference file this advisor is watching.
    [[nodiscard]] const std::string& inferenceFilePath() const noexcept
    {
        return mInferenceFilePath;
    }

    /// Returns a reference to the supervised FeatureSupervisor.
    [[nodiscard]] const FeatureSupervisor& supervisor() const noexcept
    {
        return mSupervisor;
    }

private:
    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    /**
     * @brief Returns true if @p name is a recognised alert constant.
     *
     * Accepts the empty string (healthy/none) and all four `kAlert*` constants.
     * Any other value is rejected before it reaches `applyChanges()`.
     */
    [[nodiscard]] static bool isKnownAlert(const std::string& name) noexcept
    {
        return name.empty()
            || name == features::kAlertNone
            || name == features::kAlertOverload
            || name == features::kAlertLatency
            || name == features::kAlertNodeFault;
    }

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------

    FeatureSupervisor&  mSupervisor;
    std::string         mInferenceFilePath;
};

} // namespace balancer

#include "balancer/AdvisorConcept.h"
static_assert(balancer::Advisor<balancer::NNAdvisor>,
    "NNAdvisor must satisfy the Advisor concept");
