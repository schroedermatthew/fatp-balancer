/**
 * @file test_WeightedCapacity_HeaderSelfContained.cpp
 * @brief Compile-only header self-contained test for WeightedCapacity.h.
 */

/*
BALANCER_META:
  meta_version: 1
  component: WeightedCapacity
  file_role: test
  path: tests/test_WeightedCapacity_HeaderSelfContained.cpp
  namespace: balancer
  layer: Testing
  summary: Compile-only self-contained test for policies/WeightedCapacity.h.
  api_stability: in_work
  related:
    headers:
      - policies/WeightedCapacity.h
*/

#include "policies/WeightedCapacity.h"
#include "policies/WeightedCapacity.h"  // Validate idempotence

int main()
{
    return 0;
}
