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
    concept and are interchangeable at the call site. Stat-gated: re-reads the
    file only when its modification time changes, avoiding redundant applyChanges()
    calls and mutex contention on every tick.
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
 * ### Stat-gated reads
 *
 * `evaluate()` checks the file's modification time before reading. If the
 * time has not changed since the last successful read, `evaluate()` calls
 * `applyChanges()` with the cached alert name — no I/O occurs. This prevents
 * redundant file reads and superfluous FeatureSupervisor mutex acquisitions on
 * ticks where the NN has not yet produced new output.
 *
 * If the file does not exist on the first call, `evaluate()` returns an error
 * and leaves the supervisor in its current state. Subsequent calls retry.
 *
 * ### Comparison with TelemetryAdvisor
 *
 * | Property            | TelemetryAdvisor            | NNAdvisor                       |
 * |---------------------|-----------------------------|---------------------------------|
 * | Alert source        | In-process threshold logic  | External NN inference file      |
 * | `evaluate()` arg    | `const ClusterMetrics&`     | None (file is the input)        |
 * | I/O                 | None                        | `std::filesystem::status` + read|
 * | Failure modes       | applyChanges rejection only | File I/O, JSON parse, bad alert |
 * | Reconfiguration     | Replace TelemetryThresholds | Replace NN model externally     |
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

#include <filesystem>
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
 * the alert in the most recently read inference file. If the file has not
 * changed since the last read, the supervisor's alert is unchanged.
 *
 * ## Complexity model
 *
 * `evaluate()` is O(1) on a cache hit (one `std::filesystem::last_write_time`
 * call + one `applyChanges()` call). On a cache miss it additionally performs
 * one file read, one JSON parse, and one string comparison — all bounded by
 * the file size, which is expected to be a few hundred bytes.
 *
 * ## Concurrency model
 *
 * NNAdvisor is **not thread-safe**. It owns mutable stat-cache state
 * (`mLastWriteTime`, `mCachedAlert`) that is not protected by any lock.
 * Callers must serialise `evaluate()` calls externally, or instantiate one
 * NNAdvisor per thread. The supervised FeatureSupervisor uses
 * MutexSynchronizationPolicy and is safe to call from any thread.
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
 * On any error the supervisor remains in its prior state and the stat cache
 * is not updated (the next call will retry the read).
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
     * @brief Read the inference file (if updated) and apply the alert.
     *
     * Checks the file's modification time. If unchanged since the last
     * successful read, calls `applyChanges()` with the cached alert name
     * (no I/O). If the file has been updated, reads and parses it, validates
     * the alert name, updates the stat cache, and calls `applyChanges()`.
     *
     * The call is idempotent with respect to the supervisor: if the determined
     * alert matches the current active alert, `applyChanges()` returns
     * immediately without acquiring the feature graph lock.
     *
     * @return `{}` on success. A descriptive error string on any failure; the
     *         supervisor remains in its prior state and the stat cache is not
     *         updated.
     */
    [[nodiscard]] fat_p::Expected<void, std::string>
    evaluate()
    {
        namespace fs = std::filesystem;

        // ---- Stat the file --------------------------------------------------
        std::error_code ec;
        const fs::file_time_type mtime =
            fs::last_write_time(mInferenceFilePath, ec);

        if (ec)
        {
            return fat_p::unexpected(
                "NNAdvisor: cannot stat inference file '" +
                mInferenceFilePath + "': " + ec.message());
        }

        // ---- Cache hit: file unchanged since last read ----------------------
        if (mStatCacheValid && mtime == mLastWriteTime)
        {
            return mSupervisor.applyChanges(mCachedAlert);
        }

        // ---- Cache miss: read and parse the file ----------------------------
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
        auto result = mSupervisor.applyChanges(alertValue);
        if (!result)
        {
            // Do not update stat cache — retry the read next tick.
            return result;
        }

        // ---- Commit stat cache ----------------------------------------------
        mLastWriteTime  = mtime;
        mCachedAlert    = alertValue;
        mStatCacheValid = true;

        return {};
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

    FeatureSupervisor&              mSupervisor;
    std::string                     mInferenceFilePath;

    /// Modification time of the inference file at the last successful read.
    /// Only valid when mStatCacheValid is true.
    std::filesystem::file_time_type mLastWriteTime{};

    /// Alert name from the last successfully parsed inference file.
    std::string                     mCachedAlert;

    /// False until the first successful evaluate() call.
    bool                            mStatCacheValid{false};
};

} // namespace balancer
