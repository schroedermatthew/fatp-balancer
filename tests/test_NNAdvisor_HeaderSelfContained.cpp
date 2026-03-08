/*
BALANCER_META:
  meta_version: 1
  component: NNAdvisor
  file_role: test
  path: tests/test_NNAdvisor_HeaderSelfContained.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    Compile-isolation test for NNAdvisor.h. Verifies the header is
    self-contained as the first and only project header in the translation unit.
  api_stability: internal
  related:
    headers:
      - include/balancer/NNAdvisor.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

// test_NNAdvisor_HeaderSelfContained.cpp
// Verifies that NNAdvisor.h is self-contained: it compiles cleanly
// when included as the first and only project header.

#ifdef ENABLE_TEST_APPLICATION

#include "balancer/NNAdvisor.h"

// Structural assertions: key types visible from the include.

static_assert(sizeof(balancer::NNAdvisor) > 0,
    "NNAdvisor must be a complete type");

int main()
{
    return 0;
}

#endif // ENABLE_TEST_APPLICATION
