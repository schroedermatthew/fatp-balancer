/**
 * @file test_ShortestJobFirst_HeaderSelfContained.cpp
 * @brief Compile-only header self-contained test for ShortestJobFirst.h.
 */

/*
BALANCER_META:
  meta_version: 1
  component: ShortestJobFirst
  file_role: test
  path: tests/test_ShortestJobFirst_HeaderSelfContained.cpp
  namespace: balancer
  layer: Testing
  summary: Compile-only self-contained test for policies/ShortestJobFirst.h.
  api_stability: in_work
  related:
    headers:
      - policies/ShortestJobFirst.h
*/

#include "policies/ShortestJobFirst.h"
#include "policies/ShortestJobFirst.h"  // Validate idempotence

int main()
{
    return 0;
}
