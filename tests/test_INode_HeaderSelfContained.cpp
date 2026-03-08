/**
 * @file test_INode_HeaderSelfContained.cpp
 * @brief Compile-only: INode.h must compile when included alone.
 */

/*
BALANCER_META:
  meta_version: 1
  component: INode
  file_role: test
  path: tests/test_INode_HeaderSelfContained.cpp
  namespace: balancer::testing::inode
  layer: Testing
  summary: Self-contained compile test for INode.h.
  api_stability: in_work
  related:
    headers:
      - include/balancer/INode.h
*/

#include "balancer/INode.h"
int main() { return 0; }
