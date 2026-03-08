/**
 * @file test_include_all_balancer_headers.cpp
 * @brief Include-all hygiene gate — all public balancer headers in one translation unit.
 *
 * Verifies that no two headers in the balancer or policies layers define the
 * same name at global scope, and that all headers compile cleanly together.
 *
 * If this file fails to compile, there is an ODR violation or name collision
 * somewhere in the public interface.
 */

/*
BALANCER_META:
  meta_version: 1
  component: fatp-balancer
  file_role: test
  path: tests/test_include_all_balancer_headers.cpp
  namespace: balancer::testing
  layer: Testing
  summary: Include-all compile-time hygiene gate for all public balancer and policy headers.
  api_stability: in_work
*/

// Balancer interface headers
#include "balancer/AdmissionControl.h"
#include "balancer/AffinityMatrix.h"
#include "balancer/AgingEngine.h"
#include "balancer/Balancer.h"
#include "balancer/BalancerConfig.h"
#include "balancer/BalancerFeatures.h"
#include "balancer/ClusterView.h"
#include "balancer/CostModel.h"
#include "balancer/INode.h"
#include "balancer/ISchedulingPolicy.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"

// Policy headers
#include "AffinityRouting.h"
#include "Composite.h"
#include "EarliestDeadlineFirst.h"
#include "LeastLoaded.h"
#include "RoundRobin.h"
#include "ShortestJobFirst.h"
#include "WeightedCapacity.h"
#include "WorkStealing.h"

int main()
{
    return 0;
}
