/**
 * @file test_AdmissionControl_HeaderSelfContained.cpp
 * @brief Compile-only: AdmissionControl.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: AdmissionControl
  file_role: test
  path: tests/test_AdmissionControl_HeaderSelfContained.cpp
  namespace: balancer::testing::admissioncontrol
  layer: Testing
  summary: Self-contained compile test for AdmissionControl.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/AdmissionControl.h
*/

#include "balancer/AdmissionControl.h"
int main() { return 0; }
