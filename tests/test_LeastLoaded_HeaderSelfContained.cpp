/**
 * @file test_LeastLoaded_HeaderSelfContained.cpp
 * @brief Compile-only: LeastLoaded.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: LeastLoaded
  file_role: test
  path: tests/test_LeastLoaded_HeaderSelfContained.cpp
  namespace: balancer::testing::leastloaded
  layer: Testing
  summary: Self-contained compile test for LeastLoaded.h.
  api_stability: in_work
  related:
    headers:
      - policies/LeastLoaded.h
*/

#include "policies/LeastLoaded.h"
int main() { return 0; }
