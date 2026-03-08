/**
 * @file test_CostModel_HeaderSelfContained.cpp
 * @brief Compile-only: CostModel.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: CostModel
  file_role: test
  path: tests/test_CostModel_HeaderSelfContained.cpp
  namespace: balancer::testing::costmodel
  layer: Testing
  summary: Self-contained compile test for CostModel.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/CostModel.h
*/

#include "balancer/CostModel.h"
int main() { return 0; }
