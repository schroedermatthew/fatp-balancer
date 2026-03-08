/**
 * @file test_Job_HeaderSelfContained.cpp
 * @brief Compile-only: Job.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: Job
  file_role: test
  path: tests/test_Job_HeaderSelfContained.cpp
  namespace: balancer::testing::job
  layer: Testing
  summary: Self-contained compile test for Job.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/Job.h
*/

#include "balancer/Job.h"
int main() { return 0; }
