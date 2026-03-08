/**
 * @file test_ISchedulingPolicy_HeaderSelfContained.cpp
 * @brief Compile-only: ISchedulingPolicy.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: ISchedulingPolicy
  file_role: test
  path: tests/test_ISchedulingPolicy_HeaderSelfContained.cpp
  namespace: balancer::testing::ischedulingpolicy
  layer: Testing
  summary: Self-contained compile test for ISchedulingPolicy.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/ISchedulingPolicy.h
*/

#include "balancer/ISchedulingPolicy.h"
int main() { return 0; }
