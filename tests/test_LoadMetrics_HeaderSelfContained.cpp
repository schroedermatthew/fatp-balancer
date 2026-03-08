/**
 * @file test_LoadMetrics_HeaderSelfContained.cpp
 * @brief Compile-only: LoadMetrics.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: LoadMetrics
  file_role: test
  path: tests/test_LoadMetrics_HeaderSelfContained.cpp
  namespace: balancer::testing::loadmetrics
  layer: Testing
  summary: Self-contained compile test for LoadMetrics.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/LoadMetrics.h
*/

#include "balancer/LoadMetrics.h"
int main() { return 0; }
