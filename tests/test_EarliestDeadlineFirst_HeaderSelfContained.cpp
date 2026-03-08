/**
 * @file test_EarliestDeadlineFirst_HeaderSelfContained.cpp
 * @brief Compile-only header self-contained test for EarliestDeadlineFirst.h.
 */

/*
BALANCER_META:
  meta_version: 1
  component: EarliestDeadlineFirst
  file_role: test
  path: tests/test_EarliestDeadlineFirst_HeaderSelfContained.cpp
  namespace: balancer
  layer: Testing
  summary: Compile-only self-contained test for policies/EarliestDeadlineFirst.h.
  api_stability: in_work
  related:
    headers:
      - policies/EarliestDeadlineFirst.h
*/

#include "policies/EarliestDeadlineFirst.h"
#include "policies/EarliestDeadlineFirst.h"  // Validate idempotence

int main()
{
    return 0;
}
