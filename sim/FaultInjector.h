#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: FaultInjector
  file_role: public_header
  path: sim/FaultInjector.h
  namespace: balancer::sim
  layer: Sim
  summary: Fault types and configuration for SimulatedNode fault injection.
  api_stability: in_work
  related:
    tests:
      - tests/test_fault_scenarios.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file FaultInjector.h
 * @brief Fault types and configuration for SimulatedNode fault injection.
 *
 * FaultInjector was embedded in SimulatedNode.h in Phase 3. Phase 4 extracts it
 * into its own header to allow test code to reference fault types without
 * including the full node implementation.
 *
 * Fault types:
 * - None:      Clears any active fault and begins recovery.
 * - Crash:     Node transitions immediately to Failed. Queued jobs are orphaned.
 * - Slowdown:  Execution delays are multiplied by mSlowdownFactor (default 10x).
 * - Partition: Node stops accepting new submits (simulates network partition).
 *              In-flight jobs continue to completion; no new jobs enter.
 *
 * @warning Sim layer. Do not include from include/balancer/ or policies/.
 */

#include <cstdint>

namespace balancer::sim
{

// ============================================================================
// FaultType
// ============================================================================

/**
 * @brief Fault modes injectable into a SimulatedNode.
 *
 * Pass FaultType::None to injectFault() to clear any active fault and begin
 * recovery. All other values engage the corresponding fault mode immediately.
 */
enum class FaultType
{
    None,       ///< No fault — normal operation. Clears any active fault when injected.
    Crash,      ///< Node transitions to Failed immediately. Orphans all queued jobs.
    Slowdown,   ///< Execution delays multiplied by FaultConfig::slowdownFactor.
    Partition,  ///< Node stops accepting new submits. In-flight jobs complete normally.
};

// ============================================================================
// FaultConfig
// ============================================================================

/**
 * @brief Parameters governing fault behaviour.
 *
 * Applied when a fault is injected via SimulatedNode::injectFault().
 * Changing the config after injection takes effect on the next worker
 * iteration; it does not retroactively affect in-flight jobs.
 */
struct FaultConfig
{
    /// Delay multiplier applied to job execution time under Slowdown.
    /// Must be >= 1. Values < 1 are clamped to 1 at fault injection time.
    uint32_t slowdownFactor = 10;
};

} // namespace balancer::sim
