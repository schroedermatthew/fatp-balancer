/**
 * @file test_AffinityMatrix_HeaderSelfContained.cpp
 * @brief Compile-only: AffinityMatrix.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: AffinityMatrix
  file_role: test
  path: tests/test_AffinityMatrix_HeaderSelfContained.cpp
  namespace: balancer::testing::affinity
  layer: Testing
  summary: Self-contained compile test for AffinityMatrix.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/AffinityMatrix.h
*/

#include "balancer/AffinityMatrix.h"
int main() { return 0; }
