#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: FeatureSupervisor
  file_role: public_header
  path: include/balancer/FeatureSupervisor.h
  namespace: balancer
  layer: Core
  summary: >
    Owns a FeatureManager<MutexSynchronizationPolicy> and orchestrates alert-driven
    policy and feature transitions for a Balancer instance. The feature graph is built
    once at construction with MutuallyExclusive alert and policy groups connected by
    Entails edges that express all alert to policy and alert to admission/behavioral
    consequences. applyChanges() is the single entry point and delegates entirely to
    FeatureManager::replace(current, target). Policy switching, aging, and admission
    mode changes are wired to Balancer methods via a registered BatchObserver.
    kPolicyComposite is unsupported in Phase 11.
  api_stability: in_work
  related:
    docs_search: "FeatureSupervisor"
    tests:
      - tests/test_FeatureSupervisor_HeaderSelfContained.cpp
      - tests/test_feature_supervisor.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file FeatureSupervisor.h
 * @brief Alert-driven orchestration of FeatureManager, policy switching, and
 *        Balancer subsystem toggles.
 *
 * ## Responsibilities
 *
 * FeatureSupervisor is the single object responsible for translating alert
 * signals into coherent Balancer state changes. It owns a
 * `FeatureManager<MutexSynchronizationPolicy>` whose graph is fully built at
 * construction time and never mutated thereafter.
 *
 * ### Feature graph structure
 *
 * - **Alert group** (MutuallyExclusive): kAlertNone, kAlertOverload,
 *   kAlertLatency, kAlertNodeFault. Exactly one is active at a time.
 *   kAlertNone is the idle/healthy state enabled at construction.
 *
 * - **Policy group** (MutuallyExclusive): round_robin, least_loaded,
 *   affinity_routing, work_stealing, composite. Exactly one is active at a
 *   time. kPolicyComposite is registered but unsupported in Phase 11.
 *
 * - **Entails edges** (alert -> consequence):
 *   - kAlertNone      -> kPolicyRoundRobin
 *   - kAlertOverload  -> kPolicyWorkStealing, kAdmissionBulkShed
 *   - kAlertLatency   -> kPolicyRoundRobin, kAdmissionBulkShed, kFeatureAging
 *   - kAlertNodeFault -> kPolicyRoundRobin, kAdmissionStrict
 *
 * - **Behavioral features**: affinity_matrix, affinity_matrix_warm, aging,
 *   degradation_curve. Some are Entailed by alerts; others are toggled
 *   independently via setFeatureEnabled().
 *
 * ### Transition pattern
 *
 * applyChanges() calls FeatureManager::replace(current, target). The manager
 * handles the full disable-closure, ref-counted Entails cascade, and
 * enable-closure atomically. Shared Entails targets (e.g. kAdmissionBulkShed
 * Entailed by both kAlertOverload and kAlertLatency) are ref-counted and are
 * cascade-disabled only when no other desired-enabled feature still Entails
 * them, eliminating spurious observer toggles on alert-to-alert transitions.
 *
 * ### Observer wiring
 *
 * A single BatchObserver translates FeatureChange records into Balancer method
 * calls: setAgingEnabled, setDegradationEnabled, setShedBulk,
 * setAdmissionStrict, and switchPolicy. Policy switches fire only when
 * newState==true (the incoming policy enable).
 *
 * ### Thread safety
 *
 * FeatureManager uses `MutexSynchronizationPolicy`. applyChanges() and
 * updateAffinityWarm() are safe to call from any thread. The BatchObserver
 * callback is invoked outside the FeatureManager lock.
 *
 * ### Transferability
 *
 * This header does not include any `sim/` header. It may be included from
 * production code and WASM bindings.
 */

#include <memory>
#include <string>
#include <string_view>

#include "ConcurrencyPolicies.h"
#include "Expected.h"
#include "FeatureManager.h"

#include "balancer/AffinityMatrix.h"
#include "balancer/Balancer.h"
#include "balancer/BalancerFeatures.h"
#include "balancer/ISchedulingPolicy.h"
#include "balancer/PolicyFactory.h"

namespace balancer
{

// ============================================================================
// FeatureSupervisor
// ============================================================================

/**
 * @brief Orchestrates alert-driven transitions of scheduling policy and
 *        behavioral features for a `Balancer` instance.
 *
 * Owns a `FeatureManager<MutexSynchronizationPolicy>`. The feature graph is
 * built once in the constructor. Use `applyChanges()` to transition between
 * alert states, and `updateAffinityWarm()` to propagate AffinityMatrix
 * readiness.
 *
 * @note `kPolicyComposite` is not supported in Phase 11. No alert Entails it,
 *       and the BatchObserver skips switchPolicy for it.
 */
class FeatureSupervisor
{
public:
    using Manager = fat_p::feature::FeatureManager<fat_p::MutexSynchronizationPolicy>;

    // -------------------------------------------------------------------------
    // Construction / destruction
    // -------------------------------------------------------------------------

    /**
     * @brief Constructs a FeatureSupervisor wired to @p balancer.
     *
     * Builds the feature graph, registers the BatchObserver, and enables
     * `kAlertNone` as the initial alert state. The Entails edge
     * kAlertNone -> kPolicyRoundRobin enables RoundRobin transitively; the
     * BatchObserver fires and calls `balancer.switchPolicy(RoundRobin)`.
     *
     * @param balancer      The Balancer instance this supervisor governs.
     * @param affinityMatrix Optional AffinityMatrix used for warm-gate checks.
     *                       May be null; warm promotion is disabled when null.
     */
    explicit FeatureSupervisor(Balancer& balancer,
                               AffinityMatrix* affinityMatrix = nullptr)
        : mBalancer(balancer)
        , mAffinityMatrix(affinityMatrix)
        , mActiveAlert()
    {
        buildGraph();
        registerObserver();

        // Enable the idle alert state. The Entails edge kAlertNone ->
        // kPolicyRoundRobin cascades to enable RoundRobin. The BatchObserver
        // fires and wires the policy to the Balancer.
        (void)mManager.enable(std::string(features::kAlertNone));
    }

    ~FeatureSupervisor()
    {
        mManager.removeObserver(mObserverId);
    }

    FeatureSupervisor(const FeatureSupervisor&)            = delete;
    FeatureSupervisor& operator=(const FeatureSupervisor&) = delete;
    FeatureSupervisor(FeatureSupervisor&&)                 = delete;
    FeatureSupervisor& operator=(FeatureSupervisor&&)      = delete;

    // -------------------------------------------------------------------------
    // Primary API
    // -------------------------------------------------------------------------

    /**
     * @brief Transition to a new alert state.
     *
     * Delegates entirely to `FeatureManager::replace(current, target)`. The
     * manager handles the full disable-closure, ref-counted Entails cascade,
     * and enable-closure atomically. Passing an empty string is equivalent to
     * passing `kAlertNone` (idle state).
     *
     * @param alertName One of `kAlertOverload`, `kAlertLatency`,
     *                  `kAlertNodeFault`, `kAlertNone`, or `""` (idle state).
     * @return `{}` on success, or an error string describing the failure.
     */
    [[nodiscard]] fat_p::Expected<void, std::string>
    applyChanges(std::string_view alertName)
    {
        const std::string target = alertName.empty()
            ? std::string(features::kAlertNone)
            : std::string(alertName);

        const std::string current = mActiveAlert.empty()
            ? std::string(features::kAlertNone)
            : mActiveAlert;

        if (current == target) { return {}; }

        auto r = mManager.replace(current, target);
        if (!r) { return r; }

        // Both "" and kAlertNone are the idle state; normalise to "" so that
        // activeAlert() always returns "" when no active alert is set.
        mActiveAlert = (alertName.empty() || target == std::string(features::kAlertNone))
            ? "" : target;
        return {};
    }

    /**
     * @brief Directly enable or disable a named feature without an alert.
     *
     * Intended for behavioral features such as `kFeatureDegradationCurve`
     * that are not Entailed by any alert and must be set independently.
     * Do not use this to enable policy or alert features — use applyChanges().
     *
     * @return `{}` on success, or an error string.
     */
    [[nodiscard]] fat_p::Expected<void, std::string>
    setFeatureEnabled(std::string_view feature, bool enabled)
    {
        const std::string name(feature);
        if (enabled) { return mManager.enable(name); }
        return mManager.disable(name);
    }

    /**
     * @brief Check AffinityMatrix warmth and promote / demote
     *        `kFeatureAffinityMatrixWarm` accordingly.
     *
     * Should be called periodically (e.g. from TelemetryAdvisor or the
     * SimulatedCluster tick). No-op if no AffinityMatrix was provided at
     * construction.
     *
     * @param fraction Fraction of cells that must be warm to promote the flag
     *                 (default: 0.5).
     */
    void updateAffinityWarm(float fraction = 0.5f)
    {
        if (mAffinityMatrix == nullptr) { return; }

        const bool warm    = mAffinityMatrix->isMatrixWarm(fraction);
        const bool current = mManager.isEnabled(
            std::string(features::kFeatureAffinityMatrixWarm));

        if (warm && !current)
        {
            (void)mManager.enable(
                std::string(features::kFeatureAffinityMatrixWarm));
        }
        else if (!warm && current)
        {
            (void)mManager.disable(
                std::string(features::kFeatureAffinityMatrixWarm));
        }
    }

    // -------------------------------------------------------------------------
    // Query
    // -------------------------------------------------------------------------

    /// Returns true if the named feature is currently enabled in the manager.
    [[nodiscard]] bool isEnabled(std::string_view feature) const
    {
        return mManager.isEnabled(std::string(feature));
    }

    /// Returns the active alert name, or `""` if in the idle state (kAlertNone).
    [[nodiscard]] std::string_view activeAlert() const noexcept
    {
        return mActiveAlert;
    }

    /**
     * @brief Returns the active policy feature name by querying the manager.
     *
     * Scans the policy group in a stable priority order and returns the first
     * enabled policy. Falls back to `kPolicyRoundRobin` if none is found
     * (indicates a graph invariant violation).
     */
    [[nodiscard]] std::string_view activePolicy() const noexcept
    {
        using namespace features;
        static constexpr std::string_view kPolicies[] = {
            kPolicyRoundRobin, kPolicyLeastLoaded, kPolicyWorkStealing,
            kPolicyAffinity,   kPolicyComposite,
        };
        for (std::string_view p : kPolicies)
        {
            if (mManager.isEnabled(std::string(p))) { return p; }
        }
        return kPolicyRoundRobin;
    }

    /// Exposes the underlying manager for inspection or testing.
    [[nodiscard]] const Manager& manager() const noexcept { return mManager; }

private:
    // -------------------------------------------------------------------------
    // Graph construction
    // -------------------------------------------------------------------------

    void buildGraph()
    {
        using namespace features;
        using R = fat_p::feature::FeatureRelationship;

        // Register every feature node.
        static constexpr std::string_view kAllFeatures[] = {
            kPolicyRoundRobin, kPolicyLeastLoaded, kPolicyAffinity,
            kPolicyWorkStealing, kPolicyComposite,
            kFeatureAffinityMatrix, kFeatureAffinityMatrixWarm,
            kFeatureAging, kFeatureDegradationCurve,
            kAdmissionBulkShed, kAdmissionStrict,
            kAlertNone, kAlertOverload, kAlertLatency, kAlertNodeFault,
        };
        for (std::string_view sv : kAllFeatures)
        {
            (void)mManager.addFeature(std::string(sv));
        }

        // Alert group: exactly one alert active at a time.
        (void)mManager.addMutuallyExclusiveGroup(
            "alert_group",
            {
                std::string(kAlertNone),
                std::string(kAlertOverload),
                std::string(kAlertLatency),
                std::string(kAlertNodeFault),
            });

        // Policy group: exactly one policy active at a time.
        (void)mManager.addMutuallyExclusiveGroup(
            "policy_group",
            {
                std::string(kPolicyRoundRobin),
                std::string(kPolicyLeastLoaded),
                std::string(kPolicyAffinity),
                std::string(kPolicyWorkStealing),
                std::string(kPolicyComposite),
            });

        // Entails edges: alert -> policy + admission + behavioral consequences.
        // The graph owns all semantics; applyChanges() requires no knowledge of
        // which features each alert implies.
        (void)mManager.addRelationship(
            std::string(kAlertNone),      R::Entails, std::string(kPolicyRoundRobin));

        (void)mManager.addRelationship(
            std::string(kAlertOverload),  R::Entails, std::string(kPolicyWorkStealing));
        (void)mManager.addRelationship(
            std::string(kAlertOverload),  R::Entails, std::string(kAdmissionBulkShed));

        (void)mManager.addRelationship(
            std::string(kAlertLatency),   R::Entails, std::string(kPolicyRoundRobin));
        (void)mManager.addRelationship(
            std::string(kAlertLatency),   R::Entails, std::string(kAdmissionBulkShed));
        (void)mManager.addRelationship(
            std::string(kAlertLatency),   R::Entails, std::string(kFeatureAging));

        (void)mManager.addRelationship(
            std::string(kAlertNodeFault), R::Entails, std::string(kPolicyRoundRobin));
        (void)mManager.addRelationship(
            std::string(kAlertNodeFault), R::Entails, std::string(kAdmissionStrict));
    }

    // -------------------------------------------------------------------------
    // Observer wiring
    // -------------------------------------------------------------------------

    void registerObserver()
    {
        using namespace features;

        mObserverId = mManager.addBatchObserver(
            [this](const std::string& /*requested*/,
                   const std::vector<fat_p::feature::FeatureChange>& changes,
                   bool committed)
            {
                if (!committed) { return; }

                for (const auto& ch : changes)
                {
                    if (ch.name == std::string(kFeatureAging))
                    {
                        mBalancer.setAgingEnabled(ch.newState);
                    }
                    else if (ch.name == std::string(kFeatureDegradationCurve))
                    {
                        mBalancer.setDegradationEnabled(ch.newState);
                    }
                    else if (ch.name == std::string(kAdmissionBulkShed))
                    {
                        mBalancer.admissionControl().setShedBulk(ch.newState);
                    }
                    else if (ch.name == std::string(kAdmissionStrict))
                    {
                        mBalancer.admissionControl().setAdmissionStrict(ch.newState);
                    }
                    else if (isPolicyFeature(ch.name) && ch.newState)
                    {
                        // Fire switchPolicy only on the incoming policy enable.
                        // The outgoing-policy disable fires with newState=false;
                        // ignore it. kPolicyComposite is unsupported: skip it.
                        if (ch.name != std::string(kPolicyComposite))
                        {
                            mBalancer.switchPolicy(makePolicyFromFeatureName(ch.name));
                        }
                    }
                }
            });
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    /// Returns true if @p name belongs to the policy MutuallyExclusive group.
    [[nodiscard]] static bool isPolicyFeature(const std::string& name) noexcept
    {
        using namespace features;
        return name == kPolicyRoundRobin   ||
               name == kPolicyLeastLoaded  ||
               name == kPolicyWorkStealing ||
               name == kPolicyAffinity     ||
               name == kPolicyComposite;
    }

    /**
     * @brief Instantiate the scheduling policy for @p policyName.
     *
     * Delegates to `balancer::makePolicy()` from PolicyFactory.h.
     * AffinityRouting receives the Balancer's live CostModel reference.
     */
    [[nodiscard]] std::unique_ptr<ISchedulingPolicy>
    makePolicyFromFeatureName(const std::string& policyName) const
    {
        return balancer::makePolicy(
            policyName,
            &mBalancer.costModel(),
            mBalancer.config().composite);
    }

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------

    Balancer&                       mBalancer;
    AffinityMatrix*                 mAffinityMatrix;
    mutable Manager                 mManager;
    std::string                     mActiveAlert;
    fat_p::feature::ObserverId      mObserverId{0};
};

} // namespace balancer
