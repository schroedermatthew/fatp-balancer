/*
BALANCER_META:
  meta_version: 1
  component: TelemetrySnapshot
  file_role: test
  path: tests/test_TelemetrySnapshot_HeaderSelfContained.cpp
  namespace: balancer::testing
  layer: Testing
  summary: Self-contained include and structural verification for TelemetrySnapshot.h.
  api_stability: internal
  related:
    headers:
      - sim/TelemetrySnapshot.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

// Intentionally the only sim header. If this compiles in isolation,
// TelemetrySnapshot.h has no undeclared dependencies.
#include "sim/TelemetrySnapshot.h"

#include "FatPTest.h"

using fat_p::testing::TestRunner;

namespace balancer::testing::telemetryns
{

FATP_TEST_CASE(default_constructed_snapshot_is_zero_safe)
{
    // A default-constructed TelemetrySnapshot must be valid — all numeric
    // fields zero, nodes vector empty.
    balancer::sim::TelemetrySnapshot snap;
    FATP_ASSERT_TRUE(snap.nodes.empty(),              "nodes must be empty by default");
    FATP_ASSERT_EQ(snap.activeNodes,       uint32_t(0), "activeNodes must be 0");
    FATP_ASSERT_EQ(snap.unavailableNodes,  uint32_t(0), "unavailableNodes must be 0");
    FATP_ASSERT_EQ(snap.totalSubmitted,    uint64_t(0), "totalSubmitted must be 0");
    FATP_ASSERT_EQ(snap.inFlightJobs,      uint32_t(0), "inFlightJobs must be 0");
    FATP_ASSERT_EQ(snap.overloadedFraction, 0.0f,       "overloadedFraction must be 0");
    FATP_ASSERT_EQ(snap.failedFraction,     0.0f,       "failedFraction must be 0");
    FATP_ASSERT_EQ(snap.clusterP99LatencyUs, uint64_t(0), "clusterP99LatencyUs must be 0");
    FATP_ASSERT_EQ(snap.clusterP50LatencyUs, uint64_t(0), "clusterP50LatencyUs must be 0");
    return true;
}

FATP_TEST_CASE(node_entry_default_is_zero_safe)
{
    balancer::sim::NodeEntry entry;
    FATP_ASSERT_EQ(entry.nodeId,       uint32_t(0), "nodeId must be 0");
    FATP_ASSERT_EQ(entry.state,        uint8_t(0),  "state must be 0 (Offline)");
    FATP_ASSERT_EQ(entry.utilization,  0.0f,        "utilization must be 0");
    FATP_ASSERT_EQ(entry.queueDepth,   uint32_t(0), "queueDepth must be 0");
    FATP_ASSERT_EQ(entry.p50LatencyUs, uint64_t(0), "p50LatencyUs must be 0");
    FATP_ASSERT_EQ(entry.p99LatencyUs, uint64_t(0), "p99LatencyUs must be 0");
    FATP_ASSERT_EQ(entry.completedJobs, uint64_t(0), "completedJobs must be 0");
    FATP_ASSERT_EQ(entry.failedJobs,    uint64_t(0), "failedJobs must be 0");
    return true;
}

FATP_TEST_CASE(snapshot_is_copyable_and_movable)
{
    // TelemetrySnapshot is passed by value to TelemetryAdvisor::evaluate().
    // Verify it copies and moves correctly.
    balancer::sim::TelemetrySnapshot snap;
    snap.activeNodes = 3;
    snap.overloadedFraction = 0.67f;
    snap.nodes.push_back({1, 4, 0.9f, 20, 1000, 5000, 100, 0});

    // Copy
    balancer::sim::TelemetrySnapshot copy = snap;
    FATP_ASSERT_EQ(copy.activeNodes,       uint32_t(3),   "copied activeNodes");
    FATP_ASSERT_EQ(copy.nodes.size(),      size_t(1),     "copied nodes size");
    FATP_ASSERT_EQ(copy.nodes[0].nodeId,   uint32_t(1),   "copied node nodeId");
    FATP_ASSERT_EQ(copy.nodes[0].state,    uint8_t(4),    "copied node state (Overloaded)");
    FATP_ASSERT_EQ(copy.nodes[0].queueDepth, uint32_t(20), "copied node queueDepth");

    // Move
    balancer::sim::TelemetrySnapshot moved = std::move(copy);
    FATP_ASSERT_EQ(moved.activeNodes,    uint32_t(3), "moved activeNodes");
    FATP_ASSERT_EQ(moved.nodes.size(),   size_t(1),   "moved nodes size");
    return true;
}

FATP_TEST_CASE(snapshot_fields_can_be_set_for_advisor_stubs)
{
    // Verify that all fields needed by TelemetryAdvisor can be set without
    // linking to any balancer headers.
    balancer::sim::TelemetrySnapshot snap;
    snap.activeNodes            = 5;
    snap.unavailableNodes       = 1;
    snap.totalSubmitted         = 10000;
    snap.inFlightJobs           = 42;
    snap.rejectedRateLimited    = 5;
    snap.rejectedPriorityRejected = 12;
    snap.rejectedClusterSaturated = 3;
    snap.overloadedFraction     = 0.4f;
    snap.failedFraction         = 0.2f;
    snap.clusterP99LatencyUs    = 95000;
    snap.clusterP50LatencyUs    = 12000;

    FATP_ASSERT_EQ(snap.activeNodes,        uint32_t(5),     "activeNodes");
    FATP_ASSERT_EQ(snap.unavailableNodes,   uint32_t(1),     "unavailableNodes");
    FATP_ASSERT_EQ(snap.totalSubmitted,     uint64_t(10000), "totalSubmitted");
    FATP_ASSERT_EQ(snap.inFlightJobs,       uint32_t(42),    "inFlightJobs");
    FATP_ASSERT_EQ(snap.clusterP99LatencyUs, uint64_t(95000), "clusterP99");
    return true;
}

} // namespace balancer::testing::telemetryns

namespace balancer::testing
{

bool test_TelemetrySnapshot()
{
    FATP_PRINT_HEADER(TELEMETRY SNAPSHOT)

    TestRunner runner;

    FATP_RUN_TEST_NS(runner, telemetryns, default_constructed_snapshot_is_zero_safe);
    FATP_RUN_TEST_NS(runner, telemetryns, node_entry_default_is_zero_safe);
    FATP_RUN_TEST_NS(runner, telemetryns, snapshot_is_copyable_and_movable);
    FATP_RUN_TEST_NS(runner, telemetryns, snapshot_fields_can_be_set_for_advisor_stubs);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_TelemetrySnapshot() ? 0 : 1;
}
#endif
