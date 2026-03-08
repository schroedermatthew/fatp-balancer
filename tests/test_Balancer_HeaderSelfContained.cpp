/**
 * @file test_Balancer_HeaderSelfContained.cpp
 * @brief Compile-only: Balancer.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: Balancer
  file_role: test
  path: tests/test_Balancer_HeaderSelfContained.cpp
  namespace: balancer::testing::balancer
  layer: Testing
  summary: Self-contained compile test for Balancer.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/Balancer.h
*/

#include "balancer/Balancer.h"
int main() { return 0; }
