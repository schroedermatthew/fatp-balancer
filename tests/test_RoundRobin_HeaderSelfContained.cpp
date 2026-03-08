/**
 * @file test_RoundRobin_HeaderSelfContained.cpp
 * @brief Compile-only: RoundRobin.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: RoundRobin
  file_role: test
  path: tests/test_RoundRobin_HeaderSelfContained.cpp
  namespace: balancer::testing::roundrobin
  layer: Testing
  summary: Self-contained compile test for RoundRobin.h.
  api_stability: in_work
  related:
    headers:
      - policies/RoundRobin.h
*/

#include "policies/RoundRobin.h"
int main() { return 0; }
