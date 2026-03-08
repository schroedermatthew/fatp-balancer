/**
 * @file test_BalancerConfig_HeaderSelfContained.cpp
 * @brief Compile-only: BalancerConfig.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: BalancerConfig
  file_role: test
  path: tests/test_BalancerConfig_HeaderSelfContained.cpp
  namespace: balancer::testing::balancerconfig
  layer: Testing
  summary: Self-contained compile test for BalancerConfig.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/BalancerConfig.h
*/

#include "balancer/BalancerConfig.h"
int main() { return 0; }
