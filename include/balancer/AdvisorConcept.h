#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: AdvisorConcept
  file_role: public_header
  path: include/balancer/AdvisorConcept.h
  namespace: balancer
  layer: Core
  summary: >
    C++20 concept constraining advisor types that drive FeatureSupervisor.
    Advisor<T> checks the queryable state surface common to all advisor
    implementations: currentAlert() and supervisor(). evaluate() is
    intentionally not constrained — its argument list differs by design
    between TelemetryAdvisor (takes ClusterMetrics) and NNAdvisor (takes
    nothing), reflecting the different input sources of each implementation.
  api_stability: in_work
  related:
    docs_search: "AdvisorConcept"
    tests:
      - tests/test_AdvisorConcept_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file AdvisorConcept.h
 * @brief C++20 concept for FeatureSupervisor advisor types.
 *
 * ## What the concept checks
 *
 * `Advisor<T>` constrains the state-query surface that every advisor exposes:
 *
 * | Expression              | Required type                    |
 * |-------------------------|----------------------------------|
 * | `a.currentAlert()`      | convertible to `std::string_view`|
 * | `a.supervisor()`        | `const FeatureSupervisor&`       |
 *
 * Both `TelemetryAdvisor` and `NNAdvisor` satisfy this concept.
 *
 * ## Why evaluate() is excluded
 *
 * The two advisor implementations have fundamentally different `evaluate()`
 * signatures:
 *
 * | Type                | Signature                                            |
 * |---------------------|------------------------------------------------------|
 * | `TelemetryAdvisor`  | `evaluate(const ClusterMetrics&) -> Expected<…>`     |
 * | `NNAdvisor`         | `evaluate() -> Expected<…>`                          |
 *
 * This is intentional: the argument reflects the input source. Threshold
 * logic needs live metrics; file-backed NN inference already consumed them
 * externally. Forcing a uniform `evaluate()` arity would require one side to
 * carry dead weight.
 *
 * Code that calls `evaluate()` always knows which concrete advisor it holds —
 * either because it constructed it, or because it is visiting a `std::variant`
 * with an overloaded lambda. The concept is useful at template boundaries
 * where only the query surface is needed (loggers, monitors, test helpers).
 *
 * ## Usage
 *
 * ```cpp
 * // Constrain a template parameter
 * template <balancer::Advisor A>
 * void logCurrentAlert(const A& advisor) {
 *     std::cout << advisor.currentAlert() << '\n';
 * }
 *
 * // Static assertion on a concrete type
 * static_assert(balancer::Advisor<balancer::NNAdvisor>);
 * static_assert(balancer::Advisor<balancer::TelemetryAdvisor>);
 * ```
 *
 * @see TelemetryAdvisor.h
 * @see NNAdvisor.h
 */

#include <concepts>
#include <string_view>

#include "balancer/FeatureSupervisor.h"

namespace balancer
{

/**
 * @brief Constrains types that act as FeatureSupervisor advisors.
 *
 * A type `T` satisfies `Advisor` if:
 * - `t.currentAlert()` is callable on a const instance and its return type is
 *   convertible to `std::string_view`.
 * - `t.supervisor()` is callable on a const instance and returns a
 *   `const FeatureSupervisor&`.
 *
 * `evaluate()` is not part of the concept; see the file-level documentation
 * for the rationale.
 */
template <typename T>
concept Advisor =
    requires(const T& a)
    {
        { a.currentAlert() } -> std::convertible_to<std::string_view>;
        { a.supervisor()   } -> std::same_as<const FeatureSupervisor&>;
    };

} // namespace balancer
