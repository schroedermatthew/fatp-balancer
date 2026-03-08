# fatp-balancer Test Suite Style Guide

**Status:** Active
**Applies to:** All test files in `tests/`
**Authority:** Subordinate to *fatp-balancer Development Guidelines*
**Version:** 1.0 (March 2026)

---

## Purpose

This guide ensures consistent, thorough test suites across all fatp-balancer components. Tests are the **executable specification** of the component — they document behavior, catch regressions, and prove correctness.

Tests must cover:
- Happy paths
- Failure paths (every `Expected` error code must be produced by at least one test)
- Fault injection scenarios (node failures, partition, slowdown)
- Priority and aging behavior
- Learning model convergence

---

## File Structure

### Implementation File (`test_Component.cpp`)

All tests use `FATP_TEST_CASE` with the `balancer::testing::componentns` namespace:

```cpp
/**
 * @file test_Component.cpp
 * @brief Comprehensive unit tests for Component.h
 */

/*
BALANCER_META:
  meta_version: 1
  component: ComponentName
  file_role: test
  path: tests/test_Component.cpp
  namespace: balancer::testing::componentns
  layer: Testing
  summary: Unit tests for ComponentName.
  api_stability: in_work
  related:
    headers:
      - include/balancer/Component.h
*/

#include <iostream>
#include <string>

#include "balancer/Component.h"
#include "FatPTest.h"

namespace balancer::testing::componentns
{

// ============================================================================
// Helper Types
// ============================================================================

// Define test helpers specific to this component

// ============================================================================
// Tests
// ============================================================================

FATP_TEST_CASE(basic_construction)
{
    Component c;
    FATP_ASSERT_TRUE(c.isEmpty(), "Should start empty");
    return true;
}

FATP_TEST_CASE(error_path)
{
    Component c;
    auto result = c.doFallibleThing();
    FATP_ASSERT_FALSE(result.hasValue(), "Should return error when not configured");
    return true;
}

} // namespace balancer::testing::componentns

// ============================================================================
// Public Interface
// ============================================================================

namespace balancer::testing
{

bool test_Component()
{
    FATP_PRINT_HEADER(COMPONENT NAME)

    TestRunner runner;

    FATP_RUN_TEST_NS(runner, componentns, basic_construction);
    FATP_RUN_TEST_NS(runner, componentns, error_path);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_Component() ? 0 : 1;
}
#endif
```

### Key Requirements

| Element | Requirement |
|---------|-------------|
| Namespace | `balancer::testing::componentns` (nested, not anonymous) |
| Test definition | `FATP_TEST_CASE(name)` macro |
| Test execution | `FATP_RUN_TEST_NS(runner, componentns, name)` macro |
| Return value | Every test returns `bool` (`return true;` on success) |
| BALANCER_META | Required on every test file |

---

## Test Suite Coverage Requirements

### Per-Component Coverage

Every component test suite must cover:

| Category | Requirement |
|----------|-------------|
| Construction/destruction | All constructors; RAII correctness |
| Happy paths | Every public method's normal case |
| Error paths | Every `Expected` error code produced at least once |
| Edge cases | Empty state, single element, boundary values |
| Copy/move semantics | If the component is copyable or movable |
| Thread safety | If the component is documented as thread-safe |

### Balancer-Specific Coverage Requirements

The following suites have mandatory scenarios beyond the standard list:

#### test_admission.cpp

- [ ] Global rate limit triggers `RateLimited`
- [ ] Per-priority throttling produces `PriorityRejected` for each affected band
- [ ] Critical and High bypass throttling when configured to do so
- [ ] Cluster saturation produces `ClusterSaturated` for Normal and below
- [ ] Holding queue fills and produces `HoldingQueueFull`
- [ ] `DeadlineUnachievable` when deadline has already passed on submit
- [ ] All three admission layers interact correctly when stacked

#### test_aging.cpp

- [ ] Job ages through each priority band on schedule
- [ ] Ceiling is respected — High does not age to Critical by default
- [ ] Deadline expiry fires `JobExpired` event at the correct time
- [ ] Jobs at ceiling do not generate spurious aging events
- [ ] Aging interacts correctly with cancel — aged job not re-queued if already cancelled

#### test_cost_model.cpp

- [ ] Cold start: `isWarm()` returns false before N observations
- [ ] Warm state reached after exactly N observations (configurable N)
- [ ] EMA converges toward a stable value given consistent observations
- [ ] Node throughput multiplier updates correctly from `onJobCompleted`
- [ ] Job class correction updates correctly
- [ ] DegradationCurve bucket for the observed queue depth is updated
- [ ] `predict()` returns 1.0 multiplier during cold start (neutral prediction)
- [ ] `predict()` produces lower values for a node known to be slow at a job class
- [ ] Serialization round-trip: `toJson()` then `fromJson()` produces identical predictions
- [ ] Warm-start: loaded model produces same predictions as the original before serialization

#### test_node_fsm.cpp

- [ ] All valid state transitions are accepted
- [ ] All invalid transitions are rejected
- [ ] Priority admission table is correct for each state (see Development Guidelines §Node State Machine table)
- [ ] Draining accepts Critical only
- [ ] Failed accepts nothing
- [ ] Recovery transitions back to Idle, not Busy

#### test_balancer_core.cpp

- [ ] Jobs route to eligible nodes only (respects node state admission table)
- [ ] Strategy switch mid-run does not lose jobs
- [ ] CommandBuffer flush applies mutations atomically
- [ ] FrameAllocator bulk reset clears all per-frame state
- [ ] EntityNames bidirectional lookup works after insert and remove

#### test_fault_scenarios.cpp

- [ ] Node crash mid-job: orphaned jobs requeue to another node
- [ ] Node slow: model detects and routes away after sufficient observations
- [ ] Full partition (all nodes fail): Critical jobs queue in holding queue
- [ ] Recovery: model adjusts when previously slow node returns to normal speed
- [ ] Priority livelock: Normal jobs accumulate indefinitely if admission control misconfigured (this is a negative test — verify the failure mode is detectable, not that it doesn't happen)

---

## Special-Purpose Test Files

### Test Orchestrator

`test_balancer.cpp` aggregates all suites. Uses `RUN_AND_RECORD` pattern:

```cpp
#define RUN_AND_RECORD(test_func) results.push_back({#test_func, test_func()})

int main()
{
    std::vector<std::pair<std::string, bool>> results;

    RUN_AND_RECORD(balancer::testing::test_admission);
    RUN_AND_RECORD(balancer::testing::test_aging);
    RUN_AND_RECORD(balancer::testing::test_cost_model);
    RUN_AND_RECORD(balancer::testing::test_policies);
    RUN_AND_RECORD(balancer::testing::test_node_fsm);
    RUN_AND_RECORD(balancer::testing::test_balancer_core);
    RUN_AND_RECORD(balancer::testing::test_fault_scenarios);

    int failed = 0;
    for (auto& [name, passed] : results)
    {
        std::cout << (passed ? "PASS" : "FAIL") << "  " << name << "\n";
        if (!passed) ++failed;
    }
    return failed;
}
```

Does NOT use `FATP_TEST_CASE` or `FATP_RUN_TEST_NS`.

### Header Include Hygiene Tests

For each public header `X.h`, provide `tests/test_X_HeaderSelfContained.cpp`:

```cpp
/**
 * @file test_INode_HeaderSelfContained.cpp
 * @brief Compile-only header self-contained test for INode.h.
 */

#include "balancer/INode.h"
#include "balancer/INode.h"  // Validate idempotence

int main()
{
    return 0;
}
```

The first and only FAT-P header included is the target header. No other FAT-P headers. No FatPTest.h. This is a compile gate, not a behavior test. Treat failures as P0 hygiene regressions.

### Compile-Fail Contract Tests

Location: `tests/compile_fail/compile_fail_<Component>_<Reason>.cpp`

For components that enforce compile-time constraints (e.g., static Priority enum range, INode concept requirements), provide compile-fail tests:

```cpp
/**
 * @file compile_fail_Priority_OutOfRange.cpp
 * @brief Verifies that invalid Priority values are rejected at compile time.
 *
 * Expected failure: static_assert in Priority enum validation.
 */

#include "balancer/Job.h"

// Force instantiation of invalid priority
using BadPriority = balancer::Priority;
static_assert(static_cast<int>(BadPriority::Critical) == 0);  // Sanity: this should pass
// The actual compile-fail trigger goes here
```

---

## Assertion Macros

From `FatPTest.h`:

| Macro | Use When |
|-------|---------|
| `FATP_ASSERT_TRUE(cond, msg)` | Boolean condition is true |
| `FATP_ASSERT_FALSE(cond, msg)` | Boolean condition is false |
| `FATP_ASSERT_EQ(a, b, msg)` | Value equality (prefer over `ASSERT_TRUE(a==b)`) |
| `FATP_ASSERT_NE(a, b, msg)` | Value inequality |
| `FATP_ASSERT_LT(a, b, msg)` | Less than |
| `FATP_ASSERT_LE(a, b, msg)` | Less than or equal |
| `FATP_ASSERT_GT(a, b, msg)` | Greater than |
| `FATP_ASSERT_GE(a, b, msg)` | Greater than or equal |
| `FATP_ASSERT_CLOSE(a, b, msg)` | Floating-point, default tolerance |
| `FATP_ASSERT_CLOSE_EPS(a, b, eps, msg)` | Floating-point, custom tolerance |
| `FATP_ASSERT_NULLPTR(ptr, msg)` | Pointer is null |
| `FATP_ASSERT_NOT_NULLPTR(ptr, msg)` | Pointer is not null |
| `FATP_ASSERT_THROWS(expr, type, msg)` | Expression throws specific type |
| `FATP_ASSERT_NO_THROW(expr, msg)` | Expression does not throw |

**Intention over mechanism:** Use the macro that names the check, not the most generic one.

```cpp
// Good — macro name describes the check
FATP_ASSERT_EQ(model.getNodeMultiplier(nodeId), 1.0f, "Cold start multiplier should be neutral");
FATP_ASSERT_FALSE(model.isWarm(nodeId), "Model should be cold before N observations");

// Less clear — boolean hides the actual check
FATP_ASSERT_TRUE(model.getNodeMultiplier(nodeId) == 1.0f, "Cold start multiplier");
```

---

## Helper Types

### Job Builder

```cpp
// Convenience builder for test jobs
struct TestJobBuilder
{
    balancer::Job build() const
    {
        return balancer::Job{
            .id = balancer::JobId{nextId++},
            .priority = priority,
            .estimatedCost = estimatedCost,
            .jobClass = jobClass,
            .submitted = balancer::Clock::now(),
        };
    }

    TestJobBuilder& withPriority(balancer::Priority p) { priority = p; return *this; }
    TestJobBuilder& withCost(balancer::Cost c) { estimatedCost = c; return *this; }
    TestJobBuilder& withClass(balancer::JobClass c) { jobClass = c; return *this; }

    balancer::Priority priority = balancer::Priority::Normal;
    balancer::Cost estimatedCost{100};
    balancer::JobClass jobClass{0};
    inline static uint32_t nextId = 1;
};
```

### Completion Recorder

For verifying that completion callbacks fire correctly:

```cpp
struct CompletionRecorder
{
    void record(balancer::JobId id, balancer::Cost observed)
    {
        std::lock_guard lock(mMutex);
        completions.push_back({id, observed});
    }

    std::vector<std::pair<balancer::JobId, balancer::Cost>> completions;
    std::mutex mMutex;
};
```

### Deterministic Clock

For aging and deadline tests that must not depend on wall time:

```cpp
struct DeterministicClock
{
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::steady_clock::duration;

    TimePoint now() const { return mNow; }
    void advance(Duration d) { mNow += d; }

    TimePoint mNow = std::chrono::steady_clock::now();
};
```

---

## Test Naming

Test case names must be descriptive:

```
// Good
FATP_TEST_CASE(normal_job_rejected_when_cluster_saturated)
FATP_TEST_CASE(ema_converges_after_20_observations)
FATP_TEST_CASE(orphaned_jobs_requeue_on_node_failure)

// Bad
FATP_TEST_CASE(test7)
FATP_TEST_CASE(admission_test)
FATP_TEST_CASE(node_failure)
```

---

## Checklist Before Submitting Tests

### Structure
- [ ] `BALANCER_META` block present
- [ ] Named nested namespace `balancer::testing::componentns`
- [ ] Tests defined with `FATP_TEST_CASE(name)` macro
- [ ] Tests executed with `FATP_RUN_TEST_NS(runner, componentns, name)` macro
- [ ] Public interface in separate `balancer::testing` namespace block
- [ ] `main()` guarded by `ENABLE_TEST_APPLICATION`

### Coverage
- [ ] All public methods tested
- [ ] Every `Expected` error code produced by at least one test
- [ ] Edge cases covered (empty state, boundary values)
- [ ] Fault scenarios covered (for balancer-facing components)
- [ ] Thread-safety verified (for thread-safe components)
- [ ] Copy/move semantics verified (if applicable)

### Assertions
- [ ] Every assertion has a descriptive message
- [ ] Assertion macro matches the check (intention over mechanism)
- [ ] Floating-point comparisons use `FATP_ASSERT_CLOSE` or `FATP_ASSERT_CLOSE_EPS`

### Naming
- [ ] Test names are descriptive — behavior-oriented, not number-oriented
- [ ] Namespace matches component: `admission`, `aging`, `costmodel`, `nodefs`

---

*fatp-balancer Test Suite Style Guide v1.0 — March 2026*
*Adapted from Fat-P Test Suite Style Guide v2.3*
