/*
BALANCER_META:
  meta_version: 1
  component: HoldingQueue
  file_role: test
  path: tests/test_holding_queue.cpp
  namespace: balancer::testing
  layer: Testing
  summary: >
    Unit tests for HoldingQueue: capacity enforcement, Critical-before-High
    priority ordering, FIFO within priority, cancel, handle encoding, and the
    full integration path through Balancer (DEBT-005).
  api_stability: internal
  related:
    headers:
      - include/balancer/HoldingQueue.h
      - include/balancer/Balancer.h
      - sim/SimulatedCluster.h
  hygiene:
    pragma_once: false
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "FatPTest.h"

#include "balancer/HoldingQueue.h"
#include "balancer/BalancerConfig.h"
#include "policies/LeastLoaded.h"
#include "policies/RoundRobin.h"
#include "sim/SimulatedCluster.h"

using fat_p::testing::TestRunner;
using namespace std::chrono_literals;

namespace balancer::testing::hqns
{

// ---------------------------------------------------------------------------
// HoldingQueue unit tests (no Balancer/sim needed)
// ---------------------------------------------------------------------------

FATP_TEST_CASE(capacity_enforced)
{
    HoldingQueue hq{2};
    Job j;
    j.priority = Priority::High;
    JobCompletionCallback nop = [](Job, bool){};

    FATP_ASSERT_TRUE(hq.enqueue(j, nop).has_value(), "first enqueue ok");
    FATP_ASSERT_TRUE(hq.enqueue(j, nop).has_value(), "second enqueue ok");

    auto r = hq.enqueue(j, nop);
    FATP_ASSERT_TRUE(!r.has_value(), "third enqueue must fail");
    FATP_ASSERT_EQ(static_cast<int>(r.error()),
                   static_cast<int>(SubmitError::HoldingQueueFull),
                   "error must be HoldingQueueFull");
    return true;
}

FATP_TEST_CASE(critical_dequeued_before_high)
{
    HoldingQueue hq{8};
    JobCompletionCallback nop = [](Job, bool){};

    Job high;   high.priority   = Priority::High;
    Job crit;   crit.priority   = Priority::Critical;

    // Enqueue High first, then Critical.
    FATP_ASSERT_TRUE(hq.enqueue(high, nop).has_value(), "enqueue high");
    FATP_ASSERT_TRUE(hq.enqueue(crit, nop).has_value(), "enqueue critical");

    auto first = hq.tryDequeue();
    FATP_ASSERT_TRUE(first.has_value(), "first dequeue not empty");
    FATP_ASSERT_EQ(static_cast<int>(first->job.priority),
                   static_cast<int>(Priority::Critical),
                   "Critical must come out first");

    auto second = hq.tryDequeue();
    FATP_ASSERT_TRUE(second.has_value(), "second dequeue not empty");
    FATP_ASSERT_EQ(static_cast<int>(second->job.priority),
                   static_cast<int>(Priority::High),
                   "High comes out second");
    return true;
}

FATP_TEST_CASE(fifo_within_same_priority)
{
    HoldingQueue hq{8};
    JobCompletionCallback nop = [](Job, bool){};

    // Enqueue three High jobs; they should come out in insertion order.
    Job j1; j1.priority = Priority::High; j1.estimatedCost = Cost{1};
    Job j2; j2.priority = Priority::High; j2.estimatedCost = Cost{2};
    Job j3; j3.priority = Priority::High; j3.estimatedCost = Cost{3};

    (void)hq.enqueue(j1, nop);
    (void)hq.enqueue(j2, nop);
    (void)hq.enqueue(j3, nop);

    FATP_ASSERT_EQ(hq.tryDequeue()->job.estimatedCost.units, uint64_t{1}, "FIFO: j1 first");
    FATP_ASSERT_EQ(hq.tryDequeue()->job.estimatedCost.units, uint64_t{2}, "FIFO: j2 second");
    FATP_ASSERT_EQ(hq.tryDequeue()->job.estimatedCost.units, uint64_t{3}, "FIFO: j3 third");
    return true;
}

FATP_TEST_CASE(empty_dequeue_returns_nullopt)
{
    HoldingQueue hq{4};
    FATP_ASSERT_TRUE(!hq.tryDequeue().has_value(), "empty dequeue returns nullopt");
    return true;
}

FATP_TEST_CASE(cancel_removes_job)
{
    HoldingQueue hq{4};
    Job j; j.priority = Priority::High;
    JobCompletionCallback nop = [](Job, bool){};

    auto r = hq.enqueue(j, nop);
    FATP_ASSERT_TRUE(r.has_value(), "enqueue ok");
    uint64_t raw = r.value().value();

    FATP_ASSERT_TRUE(hq.cancel(raw), "cancel returns true");
    FATP_ASSERT_TRUE(hq.empty(), "queue empty after cancel");
    FATP_ASSERT_TRUE(!hq.cancel(raw), "second cancel returns false");
    return true;
}

FATP_TEST_CASE(cancel_wrong_handle_returns_false)
{
    HoldingQueue hq{4};
    FATP_ASSERT_TRUE(!hq.cancel(kHoldingHandleBit | 999), "bogus handle returns false");
    return true;
}

FATP_TEST_CASE(handle_bit63_always_set)
{
    HoldingQueue hq{8};
    Job j; j.priority = Priority::High;
    JobCompletionCallback nop = [](Job, bool){};

    for (int i = 0; i < 5; ++i)
    {
        auto r = hq.enqueue(j, nop);
        FATP_ASSERT_TRUE(r.has_value(), "enqueue ok");
        FATP_ASSERT_TRUE(isHoldingHandle(r.value().value()),
            "all handles must have bit 63 set");
    }
    return true;
}

FATP_TEST_CASE(handles_are_unique_across_enqueues)
{
    HoldingQueue hq{16};
    Job j; j.priority = Priority::Critical;
    JobCompletionCallback nop = [](Job, bool){};
    std::vector<uint64_t> handles;

    for (int i = 0; i < 8; ++i)
    {
        auto r = hq.enqueue(j, nop);
        FATP_ASSERT_TRUE(r.has_value(), "enqueue ok");
        handles.push_back(r.value().value());
    }

    // All handles must be distinct.
    for (size_t i = 0; i < handles.size(); ++i)
        for (size_t k = i + 1; k < handles.size(); ++k)
            FATP_ASSERT_TRUE(handles[i] != handles[k], "handles must be unique");
    return true;
}

FATP_TEST_CASE(size_and_empty_consistent)
{
    HoldingQueue hq{4};
    Job j; j.priority = Priority::High;
    JobCompletionCallback nop = [](Job, bool){};

    FATP_ASSERT_TRUE(hq.empty(), "initially empty");
    FATP_ASSERT_EQ(hq.size(), uint32_t{0}, "size 0");

    (void)hq.enqueue(j, nop);
    FATP_ASSERT_TRUE(!hq.empty(), "not empty after enqueue");
    FATP_ASSERT_EQ(hq.size(), uint32_t{1}, "size 1");

    (void)hq.tryDequeue();
    FATP_ASSERT_TRUE(hq.empty(), "empty after dequeue");
    return true;
}

// ---------------------------------------------------------------------------
// Integration tests: Balancer holds and drains jobs (DEBT-005)
// ---------------------------------------------------------------------------

// Builds a saturated 1-node, 1-worker cluster and returns the cluster + balancer.
// The blocking job holds the single worker for ~blockMs milliseconds.
struct SaturatedFixture
{
    sim::SimulatedCluster cluster;
    Balancer              balancer;

    explicit SaturatedFixture(uint32_t blockMs = 50)
        : cluster{[]{
              sim::SimulatedClusterConfig c;
              c.nodeCount         = 1;
              c.workerCount       = 1;
              c.overloadThreshold = 1;   // overloaded after 1 queued job
              c.recoverThreshold  = 0;
              c.minJobDurationUs  = 0;
              c.maxJobDurationUs  = 0;
              return c;
          }()}
        , balancer{cluster.nodes(), std::make_unique<LeastLoaded>()}
    {
        // Submit a blocking job to saturate the single worker.
        Job blocker;
        blocker.priority      = Priority::Normal;
        blocker.estimatedCost = Cost{1};
        blocker.payload       = [blockMs] {
            std::this_thread::sleep_for(
                std::chrono::milliseconds{blockMs});
        };
        (void)balancer.submit(blocker);
    }
};

FATP_TEST_CASE(high_priority_overflow_gets_holding_handle)
{
    // When the cluster is saturated, a High job must receive a holding handle
    // (bit 63 set) rather than being rejected.
    SaturatedFixture fix{100};

    // Wait for overloaded state.
    std::this_thread::sleep_for(5ms);

    Job overflow;
    overflow.priority      = Priority::High;
    overflow.estimatedCost = Cost{1};
    overflow.payload       = [] {};
    auto r = fix.balancer.submit(overflow);

    FATP_ASSERT_TRUE(r.has_value(), "High overflow must be accepted");
    FATP_ASSERT_TRUE(isHoldingHandle(r.value().value()),
        "returned handle must have bit 63 set");

    (void)fix.cluster.drainAll(2000ms);
    return true;
}

FATP_TEST_CASE(critical_priority_overflow_gets_holding_handle)
{
    SaturatedFixture fix{100};
    std::this_thread::sleep_for(5ms);

    Job overflow;
    overflow.priority      = Priority::Critical;
    overflow.estimatedCost = Cost{1};
    overflow.payload       = [] {};
    auto r = fix.balancer.submit(overflow);

    FATP_ASSERT_TRUE(r.has_value(), "Critical overflow must be accepted");
    FATP_ASSERT_TRUE(isHoldingHandle(r.value().value()),
        "returned handle must have bit 63 set");

    (void)fix.cluster.drainAll(2000ms);
    return true;
}

FATP_TEST_CASE(held_job_dispatched_after_cluster_recovers)
{
    // Submit a blocking job, then a High overflow. After the blocker finishes,
    // the drain loop must dispatch the held job. Its completion callback must fire.
    SaturatedFixture fix{80};
    std::this_thread::sleep_for(5ms);

    std::atomic<bool> heldCompleted{false};
    Job overflow;
    overflow.priority      = Priority::High;
    overflow.estimatedCost = Cost{1};
    overflow.payload       = [&heldCompleted] { heldCompleted.store(true); };
    auto r = fix.balancer.submit(overflow);

    FATP_ASSERT_TRUE(r.has_value(), "overflow must be accepted");

    // Wait up to 2s for the held job to complete.
    auto deadline = std::chrono::steady_clock::now() + 2000ms;
    while (!heldCompleted.load() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);

    FATP_ASSERT_TRUE(heldCompleted.load(),
        "held job must complete after cluster recovers");
    return true;
}

FATP_TEST_CASE(cancel_held_job_before_dispatch)
{
    SaturatedFixture fix{200};
    std::this_thread::sleep_for(5ms);

    std::atomic<bool> cbFired{false};
    Job overflow;
    overflow.priority      = Priority::High;
    overflow.estimatedCost = Cost{1};
    overflow.payload       = [&cbFired] { cbFired.store(true); };
    auto r = fix.balancer.submit(overflow);
    FATP_ASSERT_TRUE(r.has_value(), "enqueue ok");
    JobHandle hh = r.value();

    // Cancel while still held.
    auto cancelR = fix.balancer.cancel(hh);
    FATP_ASSERT_TRUE(cancelR.has_value(), "cancel of held job must succeed");

    // Let cluster drain; payload must NOT fire.
    (void)fix.cluster.drainAll(1000ms);
    std::this_thread::sleep_for(20ms);
    FATP_ASSERT_TRUE(!cbFired.load(), "cancelled job's payload must not fire");
    return true;
}

FATP_TEST_CASE(cancel_holding_handle_twice_returns_not_found)
{
    SaturatedFixture fix{200};
    std::this_thread::sleep_for(5ms);

    Job overflow;
    overflow.priority      = Priority::High;
    overflow.estimatedCost = Cost{1};
    overflow.payload       = [] {};
    auto r = fix.balancer.submit(overflow);
    FATP_ASSERT_TRUE(r.has_value(), "enqueue ok");
    JobHandle hh = r.value();

    (void)fix.balancer.cancel(hh);  // first cancel
    auto r2 = fix.balancer.cancel(hh);
    FATP_ASSERT_TRUE(!r2.has_value(), "second cancel must fail");
    FATP_ASSERT_EQ(static_cast<int>(r2.error()),
                   static_cast<int>(CancelError::NotFound),
                   "error must be NotFound");

    (void)fix.cluster.drainAll(500ms);
    return true;
}

FATP_TEST_CASE(multiple_held_jobs_all_dispatched)
{
    // 3 overflow jobs should all complete after the blocker finishes.
    SaturatedFixture fix{80};
    std::this_thread::sleep_for(5ms);

    constexpr int N = 3;
    std::atomic<int> completed{0};

    for (int i = 0; i < N; ++i)
    {
        Job j;
        j.priority      = Priority::High;
        j.estimatedCost = Cost{1};
        j.payload       = [&completed] { completed.fetch_add(1); };
        auto r = fix.balancer.submit(j);
        FATP_ASSERT_TRUE(r.has_value(), "overflow enqueue ok");
    }

    auto deadline = std::chrono::steady_clock::now() + 3000ms;
    while (completed.load() < N &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    FATP_ASSERT_EQ(completed.load(), N, "all held jobs must eventually complete");
    return true;
}

FATP_TEST_CASE(holding_queue_full_returns_error)
{
    // Set holdingQueueCapacity = 1 so the second overflow is rejected.
    sim::SimulatedClusterConfig cc;
    cc.nodeCount         = 1;
    cc.workerCount       = 1;
    cc.overloadThreshold = 1;
    cc.recoverThreshold  = 0;
    cc.minJobDurationUs  = 0;
    cc.maxJobDurationUs  = 0;
    sim::SimulatedCluster cluster{cc};

    BalancerConfig bcfg;
    bcfg.admission.holdingQueueCapacity = 1;

    Balancer balancer{cluster.nodes(), std::make_unique<LeastLoaded>(), bcfg};

    // Saturate the worker.
    Job blocker;
    blocker.priority      = Priority::Normal;
    blocker.estimatedCost = Cost{1};
    blocker.payload       = [] { std::this_thread::sleep_for(200ms); };
    (void)balancer.submit(blocker);
    std::this_thread::sleep_for(5ms);

    Job j; j.priority = Priority::High; j.estimatedCost = Cost{1}; j.payload = []{};

    auto r1 = balancer.submit(j);  // fills the one slot
    FATP_ASSERT_TRUE(r1.has_value(), "first overflow accepted");

    auto r2 = balancer.submit(j);  // queue full
    FATP_ASSERT_TRUE(!r2.has_value(), "second overflow must fail");
    FATP_ASSERT_EQ(static_cast<int>(r2.error()),
                   static_cast<int>(SubmitError::HoldingQueueFull),
                   "error must be HoldingQueueFull");

    (void)cluster.drainAll(1000ms);
    return true;
}

FATP_TEST_CASE(normal_priority_not_held_when_saturated)
{
    // Normal-priority jobs must NOT be held; they should be rejected with
    // ClusterSaturated when all nodes are overloaded.
    SaturatedFixture fix{200};
    std::this_thread::sleep_for(5ms);

    Job normal;
    normal.priority      = Priority::Normal;
    normal.estimatedCost = Cost{1};
    normal.payload       = [] {};
    auto r = fix.balancer.submit(normal);

    FATP_ASSERT_TRUE(!r.has_value(), "Normal must be rejected when saturated");
    FATP_ASSERT_EQ(static_cast<int>(r.error()),
                   static_cast<int>(SubmitError::ClusterSaturated),
                   "error must be ClusterSaturated");

    (void)fix.cluster.drainAll(500ms);
    return true;
}

FATP_TEST_CASE(held_job_with_past_deadline_dropped_by_drain_loop)
{
    // A High overflow job whose deadline expires while sitting in the holding
    // queue must be dropped by the drain loop (not dispatched).
    // We use drainInterval=30ms and set deadline=now+1ms so the job passes
    // submit() pre-flight but has expired by the time the drain loop fires.
    sim::SimulatedClusterConfig cc;
    cc.nodeCount         = 1;
    cc.workerCount       = 1;
    cc.overloadThreshold = 1;
    cc.recoverThreshold  = 0;
    cc.minJobDurationUs  = 0;
    cc.maxJobDurationUs  = 0;
    sim::SimulatedCluster cluster{cc};

    BalancerConfig bcfg;
    bcfg.holdingQueue.drainInterval = std::chrono::milliseconds{30};

    Balancer balancer{cluster.nodes(), std::make_unique<LeastLoaded>(), bcfg};

    // Saturate the single worker.
    Job blocker;
    blocker.priority      = Priority::Normal;
    blocker.estimatedCost = Cost{1};
    blocker.payload       = [] { std::this_thread::sleep_for(500ms); };
    (void)balancer.submit(blocker);
    std::this_thread::sleep_for(5ms);  // wait for node to become Overloaded

    // Submit High overflow with a deadline 2ms from now — passes pre-flight
    // but will expire by the time the 30ms drain interval fires.
    Job overflow;
    overflow.priority      = Priority::High;
    overflow.estimatedCost = Cost{1};
    overflow.payload       = [] {};
    overflow.deadline      = Clock::now() + std::chrono::milliseconds{2};

    auto r = balancer.submit(overflow);
    bool accepted = r.has_value() && isHoldingHandle(r.value().value());

    // Wait for drain loop to process (max 300ms).
    auto waitEnd = std::chrono::steady_clock::now() + 300ms;
    while (balancer.admissionControl().holdingQueueDepth() > 0 &&
           std::chrono::steady_clock::now() < waitEnd)
        std::this_thread::sleep_for(5ms);

    uint32_t finalDepth = balancer.admissionControl().holdingQueueDepth();
    (void)cluster.drainAll(1000ms);

    FATP_ASSERT_TRUE(accepted, "High overflow must be accepted into holding queue");
    FATP_ASSERT_EQ(finalDepth, uint32_t{0},
        "holding queue depth must reach 0 after deadline-expired job is dropped");
    return true;
}

} // namespace balancer::testing::hqns

namespace balancer::testing
{

bool test_holding_queue()
{
    FATP_PRINT_HEADER(HOLDING QUEUE)
    TestRunner runner;

    FATP_RUN_TEST_NS(runner, hqns, capacity_enforced);
    FATP_RUN_TEST_NS(runner, hqns, critical_dequeued_before_high);
    FATP_RUN_TEST_NS(runner, hqns, fifo_within_same_priority);
    FATP_RUN_TEST_NS(runner, hqns, empty_dequeue_returns_nullopt);
    FATP_RUN_TEST_NS(runner, hqns, cancel_removes_job);
    FATP_RUN_TEST_NS(runner, hqns, cancel_wrong_handle_returns_false);
    FATP_RUN_TEST_NS(runner, hqns, handle_bit63_always_set);
    FATP_RUN_TEST_NS(runner, hqns, handles_are_unique_across_enqueues);
    FATP_RUN_TEST_NS(runner, hqns, size_and_empty_consistent);
    FATP_RUN_TEST_NS(runner, hqns, high_priority_overflow_gets_holding_handle);
    FATP_RUN_TEST_NS(runner, hqns, critical_priority_overflow_gets_holding_handle);
    FATP_RUN_TEST_NS(runner, hqns, held_job_dispatched_after_cluster_recovers);
    FATP_RUN_TEST_NS(runner, hqns, cancel_held_job_before_dispatch);
    FATP_RUN_TEST_NS(runner, hqns, cancel_holding_handle_twice_returns_not_found);
    FATP_RUN_TEST_NS(runner, hqns, multiple_held_jobs_all_dispatched);
    FATP_RUN_TEST_NS(runner, hqns, holding_queue_full_returns_error);
    FATP_RUN_TEST_NS(runner, hqns, normal_priority_not_held_when_saturated);
    FATP_RUN_TEST_NS(runner, hqns, held_job_with_past_deadline_dropped_by_drain_loop);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main() { return balancer::testing::test_holding_queue() ? 0 : 1; }
#endif
