/*
BALANCER_META:
  meta_version: 1
  component: HoldingQueue
  file_role: test
  path: tests/test_HoldingQueue_HeaderSelfContained.cpp
  namespace: balancer::testing
  layer: Testing
  summary: Self-contained include verification for HoldingQueue.h.
  api_stability: internal
  related:
    headers:
      - include/balancer/HoldingQueue.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

#include "balancer/HoldingQueue.h"

#include "FatPTest.h"

using fat_p::testing::TestRunner;

namespace balancer::testing::hqhscns
{

FATP_TEST_CASE(holding_queue_header_self_contained)
{
    HoldingQueue hq{16};
    FATP_ASSERT_EQ(hq.capacity(), uint32_t{16}, "capacity");
    FATP_ASSERT_TRUE(hq.empty(), "initially empty");
    FATP_ASSERT_EQ(hq.size(), uint32_t{0}, "size zero");

    FATP_ASSERT_TRUE( isHoldingHandle(kHoldingHandleBit),     "kHoldingHandleBit is holding");
    FATP_ASSERT_TRUE(!isHoldingHandle(0x7FFFFFFFFFFFFFFFull),  "bit63-clear is not holding");
    return true;
}

FATP_TEST_CASE(enqueue_and_dequeue_round_trip)
{
    HoldingQueue hq{4};

    Job j;
    j.priority = Priority::High;
    bool cbFired = false;
    JobCompletionCallback cb = [&cbFired](Job, bool) { cbFired = true; };

    auto r = hq.enqueue(j, cb);
    FATP_ASSERT_TRUE(r.has_value(), "enqueue succeeds");
    FATP_ASSERT_TRUE(isHoldingHandle(r.value().value()), "handle has bit63");
    FATP_ASSERT_EQ(hq.size(), uint32_t{1}, "size after enqueue");

    auto entry = hq.tryDequeue();
    FATP_ASSERT_TRUE(entry.has_value(), "dequeue returns entry");
    FATP_ASSERT_EQ(hq.size(), uint32_t{0}, "empty after dequeue");
    FATP_ASSERT_TRUE(hq.empty(), "queue is empty after dequeue");

    // Fire the callback to verify it is intact.
    entry->onDone(j, true);
    FATP_ASSERT_TRUE(cbFired, "callback fired from entry");
    return true;
}

} // namespace balancer::testing::hqhscns

namespace balancer::testing
{

bool test_HoldingQueue_HeaderSelfContained()
{
    FATP_PRINT_HEADER(HOLDING QUEUE HSC)
    TestRunner runner;
    FATP_RUN_TEST_NS(runner, hqhscns, holding_queue_header_self_contained);
    FATP_RUN_TEST_NS(runner, hqhscns, enqueue_and_dequeue_round_trip);
    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main() { return balancer::testing::test_HoldingQueue_HeaderSelfContained() ? 0 : 1; }
#endif
