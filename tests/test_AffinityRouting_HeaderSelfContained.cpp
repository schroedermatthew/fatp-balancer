/**
 * @file test_AffinityRouting_HeaderSelfContained.cpp
 * @brief Compile-only: AffinityRouting.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: AffinityRouting
  file_role: test
  path: tests/test_AffinityRouting_HeaderSelfContained.cpp
  namespace: balancer::testing::affinityrouting
  layer: Testing
  summary: Self-contained compile test for AffinityRouting.h.
  api_stability: in_work
  related:
    headers:
      - policies/AffinityRouting.h
*/

#include "AffinityRouting.h"
int main() { return 0; }
