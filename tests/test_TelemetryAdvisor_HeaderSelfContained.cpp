/*
BALANCER_META:
  meta_version: 1
  component: TelemetryAdvisor
  file_role: test
  path: tests/test_TelemetryAdvisor_HeaderSelfContained.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    Compile-isolation test for TelemetryAdvisor.h. Verifies the header is
    self-contained as the first and only project header in the translation unit.
  api_stability: internal
  related:
    headers:
      - include/balancer/TelemetryAdvisor.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

// test_TelemetryAdvisor_HeaderSelfContained.cpp
// Verifies that TelemetryAdvisor.h is self-contained: it compiles cleanly
// when included as the first and only project header.

#ifdef ENABLE_TEST_APPLICATION

#include "balancer/TelemetryAdvisor.h"

// Structural assertions: key types visible from the include.

static_assert(sizeof(balancer::TelemetryThresholds) > 0,
    "TelemetryThresholds must be a complete type");

static_assert(sizeof(balancer::TelemetryAdvisor) > 0,
    "TelemetryAdvisor must be a complete type");

int main()
{
    return 0;
}

#endif // ENABLE_TEST_APPLICATION
