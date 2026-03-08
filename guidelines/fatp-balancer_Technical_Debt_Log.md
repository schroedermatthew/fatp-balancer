# fatp-balancer Technical Debt Log

This document records known design gaps, hazards, and unresolved items that
are out of scope for the current phase but must not be forgotten. Items are
tagged with the phase in which they were identified and a severity.

Severity levels:
- **BLOCKING** — must be resolved before the affected feature can be considered
  correct under concurrent or production use
- **IMPORTANT** — real correctness or usability gap; should be resolved in the
  next phase that touches the affected subsystem
- **MINOR** — quality or completeness gap; address when convenient

---

## Phase 7 Debt (identified during Step 6.3 / ChatGPT independent audit)

---

### DEBT-001 — Balancer::switchPolicy() has a submit/drain race

**Severity:** BLOCKING (for concurrent policy switching under load)

**Subsystem:** `include/balancer/Balancer.h`

**Description:**
`switchPolicy()` drains by spinning until `mSubmittedCount == 0`, then swaps
the policy pointer under `mPolicyMutex`. However, a submit thread can pass the
`mDraining` check *before* the switch begins and only increment
`mSubmittedCount` *after* policy selection — after the switch thread has
already observed a zero count and swapped. The new policy can therefore
dispatch a job that was selected by the old policy.

A second hazard: two concurrent `switchPolicy()` calls are not serialized.
Both can observe the cluster drained and proceed to swap, creating a window
where a job may be admitted between two overlapping swaps.

**Recommended fix:**
Add a dedicated `mSwitchMutex` (or a generation counter) that serializes all
`switchPolicy()` calls. Move the drain-check-and-swap into a single
critical section.

**Phase to fix:** Phase 8

---

### DEBT-002 — ClusterMetrics is only partially populated

**Severity:** IMPORTANT

**Subsystem:** `include/balancer/Balancer.h`, `include/balancer/LoadMetrics.h`

**Description:**
`ClusterMetrics` declares fields including `throughputPerSecond`,
`meanP50LatencyUs`, `maxP99LatencyUs`, and `totalRejected`. Only a subset
(`activeNodes`, `unavailableNodes`, `totalSubmitted`) is populated by
`Balancer::buildClusterMetrics()`. The unpopulated fields return zero and
silently mislead any caller that reads them.

**Recommended fix:**
Either populate all declared fields in `buildClusterMetrics()`, or remove
undeclared fields from the struct and document which fields are available.
`TelemetryAdvisor` depends on rejection counts and latency aggregates; these
must be correct before the advisor is enabled in production.

**Phase to fix:** Phase 8

---

### DEBT-003 — No policy factory in core; string-to-policy mapping lives only in WASM bindings

**Severity:** IMPORTANT

**Subsystem:** `policies/`, `include/balancer/ISchedulingPolicy.h`

**Description:**
`Balancer::switchPolicy()` takes `std::unique_ptr<ISchedulingPolicy>`. The
only string-to-policy mapping in the repository is in `wasm/BalancerBindings.cpp`.
`FeatureSupervisor` needs to instantiate policies by name (`kPolicyRoundRobin`,
`kPolicyWorkStealing`, etc.) without depending on the WASM layer.

**Recommended fix:**
Add a `PolicyFactory` free function or static method in a new
`include/balancer/PolicyFactory.h` header that maps `std::string_view` to a
freshly constructed `unique_ptr<ISchedulingPolicy>`. The WASM bindings can
delegate to it.

**Phase to fix:** Phase 8 (needed for FeatureSupervisor policy switching)

**Note for Phase 7:** `FeatureSupervisor` will carry its own policy
instantiation internally for now, accepting the duplication. This is the
approved short-term workaround; the factory is tracked here for cleanup.

---

### DEBT-004 — policy.composite is underspecified in the Phase 7 feature map

**Severity:** IMPORTANT

**Subsystem:** `policies/Composite.h`, `include/balancer/BalancerFeatures.h`

**Description:**
`Composite` requires an explicit `std::vector<std::unique_ptr<ISchedulingPolicy>>`
at construction. `kPolicyComposite` is a valid feature constant, but there is
no canonical definition of what child policies comprise the composite in the
Phase 7 context. A policy name alone is not enough to instantiate `Composite`.

**Recommended fix:**
Define a `CompositeConfig` in `BalancerConfig.h` (or a new
`CompositePolicy.h`) that specifies the default child chain for production
use. Until then, `FeatureSupervisor` should treat `kPolicyComposite` as
unsupported and return an error if it is requested.

**Phase to fix:** Phase 8

---

### DEBT-005 — Holding queue is caller-responsibility with no implementation

**Severity:** MINOR (existing debt, not new)

**Subsystem:** `include/balancer/AdmissionControl.h`

**Description:**
`AdmissionControl` documents that when a job is `HoldForRetry`, the caller is
responsible for placing it in a holding queue. No holding queue implementation
exists in `Balancer` or anywhere else in the repository. High-priority overflow
jobs that receive `HoldForRetry` are silently dropped by current callers.

**Recommended fix:**
Either implement a holding queue in `Balancer` (bounded, priority-aware) or
change the contract so `HoldForRetry` is never returned without a queue to
back it.

**Phase to fix:** Phase 9 or later

---

### DEBT-006 — AgingEngine AgedEvent has no INode::reprioritise() to forward to

**Severity:** MINOR

**Subsystem:** `include/balancer/Balancer.h`, `include/balancer/INode.h`

**Description:**
The `AgingEngine` background loop in `Balancer` handles `AgedEvent` (priority
promotion) by logging and discarding — `INode` has no `reprioritise()` API.
The architectural gap is documented in `Balancer.h` inline comments. Jobs that
age past a priority threshold have their promotion silently acknowledged but
not applied at the node level.

**Recommended fix:**
Add `reprioritise(JobHandle, Priority)` to `INode` and implement it in
`SimulatedNode`. Wire the `AgedEvent` handler in `Balancer::agingLoop()` to
call it.

**Phase to fix:** Phase 8

---

## Resolved items

| Item | Resolution | Phase |
|------|-----------|-------|
| AgingEngine not integrated into Balancer | Fully wired in Step 6.2 — background thread, track/untrack, ExpiredEvent cancellation | 7 |
| CostModel has no degradation toggle | `setDegradationEnabled()` added in Step 6.2 | 7 |
| AdmissionControl has no strictness/shed flags | `setShedBulk()` and `setAdmissionStrict()` added in Step 6.2 | 7 |
| AdmissionControl not accessible from FeatureSupervisor | `admissionControl()` accessor added to Balancer in Step 6.2 | 7 |
| fromJson as per-tick actuation (wrong primitive) | Confirmed by Q2 probe. FeatureSupervisor uses persistent manager + incremental batchDisable/enable | 7 |
| Disable does not cascade-disable implied policy | Confirmed and documented by probe test. Correct transition pattern proven | 7 |
| Preempts+Implies fails on active target | Confirmed and documented by probe test | 7 |
