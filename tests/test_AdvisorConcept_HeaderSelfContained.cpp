/*
BALANCER_META:
  meta_version: 1
  component: AdvisorConcept
  file_role: test
  path: tests/test_AdvisorConcept_HeaderSelfContained.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    Compile-isolation and correctness test for AdvisorConcept.h. Verifies the
    header is self-contained and that TelemetryAdvisor and NNAdvisor both
    satisfy the Advisor concept, and that a type missing currentAlert() or
    supervisor() does not.
  api_stability: internal
  related:
    headers:
      - include/balancer/AdvisorConcept.h
      - include/balancer/TelemetryAdvisor.h
      - include/balancer/NNAdvisor.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

// test_AdvisorConcept_HeaderSelfContained.cpp
// Verifies AdvisorConcept.h is self-contained and that the concept correctly
// accepts conforming types and rejects non-conforming ones.

#ifdef ENABLE_TEST_APPLICATION

#include "balancer/AdvisorConcept.h"
#include "balancer/NNAdvisor.h"
#include "balancer/TelemetryAdvisor.h"

// Both concrete advisor types must satisfy the concept.
static_assert(balancer::Advisor<balancer::TelemetryAdvisor>,
    "TelemetryAdvisor must satisfy Advisor");

static_assert(balancer::Advisor<balancer::NNAdvisor>,
    "NNAdvisor must satisfy Advisor");

// A type with no matching interface must not satisfy the concept.
namespace
{
struct NotAnAdvisor {};
} // anonymous namespace

static_assert(!balancer::Advisor<NotAnAdvisor>,
    "NotAnAdvisor must not satisfy Advisor");

// A type with only currentAlert() but missing supervisor() must not satisfy.
namespace
{
struct MissingSupervisor
{
    [[nodiscard]] std::string_view currentAlert() const noexcept { return ""; }
};
} // anonymous namespace

static_assert(!balancer::Advisor<MissingSupervisor>,
    "MissingSupervisor must not satisfy Advisor");

// A type with only supervisor() but missing currentAlert() must not satisfy.
namespace
{
struct MissingCurrentAlert
{
    [[nodiscard]] const balancer::FeatureSupervisor& supervisor() const noexcept;
};
} // anonymous namespace

static_assert(!balancer::Advisor<MissingCurrentAlert>,
    "MissingCurrentAlert must not satisfy Advisor");

int main()
{
    return 0;
}

#endif // ENABLE_TEST_APPLICATION
