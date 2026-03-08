/**
 * @file test_ClusterView_HeaderSelfContained.cpp
 * @brief Compile-only: ClusterView.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: ClusterView
  file_role: test
  path: tests/test_ClusterView_HeaderSelfContained.cpp
  namespace: balancer::testing::clusterview
  layer: Testing
  summary: Self-contained compile test for ClusterView.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/ClusterView.h
*/

#include "balancer/ClusterView.h"
int main() { return 0; }
