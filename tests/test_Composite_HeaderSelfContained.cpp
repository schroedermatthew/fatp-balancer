/**
 * @file test_Composite_HeaderSelfContained.cpp
 * @brief Compile-only header self-contained test for Composite.h.
 */

/*
BALANCER_META:
  meta_version: 1
  component: Composite
  file_role: test
  path: tests/test_Composite_HeaderSelfContained.cpp
  namespace: balancer
  layer: Testing
  summary: Compile-only self-contained test for policies/Composite.h.
  api_stability: in_work
  related:
    headers:
      - policies/Composite.h
*/

#include "policies/Composite.h"
#include "policies/Composite.h"  // Validate idempotence

int main()
{
    return 0;
}
