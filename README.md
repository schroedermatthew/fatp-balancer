# fatp-balancer

A cluster load balancer built on [FAT-P](../FatP) demonstrating the concurrency, container, and domain stacks. Phase 10 complete.

**The transferability principle:** The balancer never knows it is talking to a simulation. `include/balancer/` and `policies/` implement the scheduling logic against the `INode` interface. `sim/` provides a thread-backed implementation for local testing. A production deployment implements `INode` with sockets, gRPC, or any other transport — the balancer code ships unchanged.

---

## Architecture

```
include/balancer/     ← Core contracts and scheduling logic (ships to production)
policies/             ← Scheduling strategy implementations (ships to production)
sim/                  ← Thread-backed simulation (test fixture, never ships)
wasm/                 ← Emscripten bindings for the browser demo
demo/                 ← Self-contained cluster visualization (index.html)
tests/                ← All test suites
guidelines/           ← Governance documents
```

**Layer dependency rule:** `include/balancer/` and `policies/` must never include `sim/`. The CMake transferability gate enforces this at configure time.

---

## Components

| Header | Layer | Role |
|--------|-------|------|
| `balancer/Job.h` | Interface | Job model — Priority, Cost, JobClass, SubmitError |
| `balancer/INode.h` | Interface | Transferable node contract |
| `balancer/ISchedulingPolicy.h` | Interface | Scheduling strategy interface |
| `balancer/LoadMetrics.h` | Interface | NodeState, admission table, per-node metrics |
| `balancer/ClusterView.h` | Core | Immutable cluster snapshot for policy decisions |
| `balancer/AdmissionControl.h` | Core | Three-layer admission gate |
| `balancer/BalancerConfig.h` | Core | Configuration for all subsystems |
| `balancer/AgingEngine.h` | Core | Priority aging, deadline expiry (Phase 3) |
| `balancer/AffinityMatrix.h` | Learning | Node × JobClass affinity Tensor — EMA-learned per-cell multipliers with JSON persistence (Phase 5) |
| `balancer/CostModel.h` | Learning | Per-node EMA + AffinityMatrix correction + DegradationCurve; JSON save/load (Phase 5) |
| `balancer/Balancer.h` | Core | Central coordinator with drain-swap-resume policy switching |
| `policies/RoundRobin.h` | Policies | Priority-filtered round-robin |
| `policies/LeastLoaded.h` | Policies | Cost-model-aware least-loaded routing |
| `policies/WeightedCapacity.h` | Policies | Routes to highest available weighted capacity |
| `policies/ShortestJobFirst.h` | Policies | Minimizes predicted time-to-completion for each job |
| `policies/EarliestDeadlineFirst.h` | Policies | Routes deadline jobs to node with maximum slack |
| `policies/Composite.h` | Policies | Chains policies in order; first success wins |
| `policies/WorkStealing.h` | Policies | Steal-bias routing — drains overloaded nodes (Phase 4) |
| `policies/AffinityRouting.h` | Policies | Routes by node × class affinity score; cold-start falls back to LeastLoaded (Phase 5) |
| `sim/FaultInjector.h` | Sim | FaultType enum and FaultConfig (Phase 4, extracted from SimulatedNode) |
| `sim/SimulatedNode.h` | Sim | INode backed by fat_p::ThreadPool with SlotMap cancel (Phase 4) |
| `sim/SimulatedCluster.h` | Sim | Manages N SimulatedNodes |
| `balancer/HoldingQueue.h` | Core | Bounded priority-ordered overflow store for jobs shed by admission control (Phase 9) |
| `balancer/PolicyFactory.h` | Core | `makePolicy(name)` → `unique_ptr<ISchedulingPolicy>` for all registered policy names (Phase 8) |
| `balancer/BalancerFeatures.h` | Core | `kFeature*`, `kAlert*`, `kPolicy*` string constants for FeatureManager integration (Phase 7) |
| `balancer/FeatureSupervisor.h` | Core | Alert-driven policy and feature transitions via FAT-P FeatureManager (Phase 7) |
| `balancer/TelemetryAdvisor.h` | Core | ClusterMetrics threshold evaluator — drives FeatureSupervisor with priority-ordered alerts (Phase 7) |
| `sim/TelemetrySnapshot.h` | Sim | JSON-serialisable cluster snapshot with per-node stats for the telemetry panel (Phase 7) |
| `wasm/BalancerBindings.cpp` | Demo | Emscripten embind glue — exposes BalancerDemo to JavaScript (Phase 6; updated Phase 10) |

---

## Phase 10 Changes

- **Task A (TelemetryAdvisor latency alert):** Enabled `kAlertLatency` — uncommented the `meanP50LatencyUs >= latencyThresholdUs` guard in `TelemetryAdvisor::determineAlert()` now that DEBT-002 is resolved. Removed all DEBT-002 caveats from docs. Added 4 latency alert tests and 1 integration test.
- **Task B (SimulatedNode override wiring):** `executeJob()` now reads and applies `mPriorityOverrides` before running the job. `cancel()` also erases the override entry to prevent map growth. Added 2 tests.
- **Task C (WASM bindings update):** Replaced inline `switchPolicy()` if/else chain with `balancer::makePolicy()`. Added `Composite` to supported policy names. Exposed `getHoldingQueueDepth()`. Upgraded `getStats()` JSON with `throughputPerSecond`, `meanP50LatencyUs`, `maxP99LatencyUs`, `totalCompleted`, `totalRejected`, `holdingQueueDepth`. Added `Balancer::clusterMetrics()` public accessor.
- **Task D (README update):** This update.

## Phase 6 Changes

### Browser Demo — `demo/index.html`

A self-contained single-file browser application that runs entirely without a server.
Open `demo/index.html` directly in any modern browser.

The demo operates in two modes:

**JavaScript simulation mode (default):** A faithful JS port of the C++ balancer
logic runs in the browser. The simulation mirrors every semantic that matters for
visualisation — per-node EMA throughput multiplier, the Node×JobClass
`AffinityMatrix` with warm-start gating, admission priority rules, and all four
scheduling policies. Jobs complete asynchronously via `setTimeout`.

**WASM mode (optional):** When `demo/balancer.js` and `demo/balancer.wasm` are
present (built via the Emscripten target), the demo detects them and switches to
the compiled C++ backend automatically.

#### Visualisation panels

**Cluster grid** — One card per node showing:
- State badge (Idle / Busy / Overloaded / Failed etc.) with live colour
- Utilization bar (green → orange → red as queue fills)
- 12-slot queue indicator (each slot lights up per queued job)
- Node-level EMA throughput multiplier, p50 latency, job counts

**AffinityMatrix heatmap** — The live `balancer::AffinityMatrix` rendered as a
`[nodes × job-classes]` grid on a `<canvas>`:
- Blue cells: node is faster than estimated for this class (multiplier < 1.0)
- Grey cells: neutral or cold (fewer than 5 observations)
- Red cells: node is slower than estimated (multiplier > 1.0)
- Each cell shows the learned multiplier and observation count
- Faint annotation shows the hidden true multiplier so the user can verify convergence

**Event log** — Timestamped stream of submit / complete / fault / policy-switch events.

#### Interactive controls

- **Submit Job** — configure priority, job class, and estimated cost; submit one job
- **Burst ×10** — submit 10 random jobs in one click to accelerate learning
- **Auto rate** — slider from off to 10 jobs/second; drives continuous load
- **Policy switch** — live drain-swap-resume; the draining indicator appears in the header while in-flight jobs settle
- **Node fault injection** — click any node card to open a fault menu: Crash, Slowdown ×5, or Recover

#### Hidden true multipliers

Each (node, job-class) pair has a hidden true multiplier that determines how long
jobs actually take on that node for that class. These create a structured pattern
that `AffinityRouting` can learn and exploit. After ~30 observations per cell the
heatmap converges toward the true values, and `AffinityRouting` starts routing
α-class jobs preferentially to Node 0, β to Node 1, and so on.

```
          α       β       γ       δ
Node 0:   0.65   1.45    1.00   1.80    ← fast for α
Node 1:   1.30   0.60    1.05   0.85    ← fast for β
Node 2:   0.90   1.10    0.52   1.25    ← fast for γ
Node 3:   1.55   0.95    1.40   0.68    ← fast for δ
```

### WASM Bindings — `wasm/BalancerBindings.cpp`

Emscripten embind wrapper around a `SimulatedCluster` + `Balancer` pair.
Exposes the full balancer API to JavaScript with zero adaptation cost on the C++ side — demonstrating the transferability principle at the transport boundary.

Exposed JavaScript API (mirrors the `BalancerDemo` class):

| Method | Signature | Description |
|--------|-----------|-------------|
| constructor | `(numNodes, numClasses)` | Create cluster |
| `submitJob` | `(priority, jobClass, estimatedCost) → bool` | Submit one job |
| `tick` | `() → void` | Advance clock (no-op for threaded backend) |
| `getNodeStates` | `() → JSON string` | Array of NodeSnapshot objects |
| `getAffinityMatrix` | `() → JSON string` | Flat float array + shape metadata |
| `getStats` | `() → JSON string` | Balancer-level counters |
| `switchPolicy` | `(name) → void` | Drain-swap-resume policy switch |
| `injectFault` | `(nodeId, type) → void` | "crash" \| "slowdown" \| "recover" |

### WASM Build — `wasm/CMakeLists.txt`

Standalone CMake project for the Emscripten target. Not included in the native
test build (incompatible toolchains). Run from the `wasm/` directory:

```bash
cd wasm
emcmake cmake . -DFATP_INCLUDE_DIR=/path/to/FatP/include/fat_p \
                -DCMAKE_BUILD_TYPE=Release
cmake --build .
# Copies balancer.js + balancer.wasm to demo/
```

---

## Phase 5 Changes

### AffinityMatrix — Node × JobClass Tensor

Phase 5 replaces the nested `std::unordered_map<uint32_t, ClassLearningEntry>`
per-class correction that lived inside each `NodeLearningState` with a dedicated
`AffinityMatrix` header backed by a `fat_p::RowMajorTensor<float>` of shape
`[maxNodes × maxJobClasses]`.

Each cell stores an EMA-learned multiplier representing how much the actual cost
deviates from the estimate when a given job class runs on a given node. Tensor
storage gives O(1) cell access (single multiply-add index arithmetic vs. hash
chain traversal) and enables block-level JSON serialisation — the entire affinity
state writes as two flat arrays rather than a recursive object tree.

Cells with fewer than `AffinityMatrixConfig.warmThreshold` observations return
1.0 (neutral). Out-of-bounds NodeId or JobClass values are clamped to the last
valid row or column so that completion callbacks never fail.

### CostModel — FastHashMap Migration

`CostModel::mNodeStates` is migrated from `std::unordered_map` to
`fat_p::FastHashMap<uint32_t, NodeLearningState>`. Open-addressing with SIMD
group probing reduces collision overhead on the predict() hot path compared to
chaining.

`NodeLearningState` is simplified: the per-class `classEntries` map is removed
(absorbed into `AffinityMatrix`) leaving only the node-level throughput EMA
and the DegradationCurve.

### CostModel — JSON Persistence

`save(path)` serialises the full model state — per-node EMA values, degradation
bucket observations, and the affinity Tensor — to a JSON file. `load(path)`
restores the snapshot with dimension validation. If any field is malformed or
the affinity shape does not match the current configuration, `load()` rolls back
to cold-start state and returns a typed `PersistError`.

The API returns `fat_p::Expected<void, PersistError>` — callers handle
`FileOpenFailed`, `MalformedJson`, and `DimensionMismatch` without exceptions.

### AffinityRouting Policy

`policies/AffinityRouting.h` adds a scheduling policy that prefers the node
with the highest learned affinity for the incoming job's class. It scores each
candidate as `predictedCost / affinityScore`, so a node that historically runs
this class efficiently (low multiplier) receives a lower score and higher
preference. When no candidate has a warm affinity cell, the policy falls back
to routing by predicted cost alone — identical to LeastLoaded.

---

## Build

**Prerequisites:** CMake 3.20+, a C++20 compiler, FAT-P at a sibling path (`../FatP`).

```bash
# Linux / macOS
./build.sh

# Windows (PowerShell)
.\build.ps1

# Windows (cmd)
build.bat

# Custom FAT-P location
FATP_INCLUDE_DIR=/path/to/FatP/include/fat_p ./build.sh

# With sanitizers
./build.sh --asan --tsan --ubsan
```

### Phase 6 demo (no build required)

```bash
# Open directly in any modern browser — no server needed:
open demo/index.html        # macOS
xdg-open demo/index.html    # Linux
start demo/index.html       # Windows
```

### Phase 6 WASM build (optional, requires Emscripten ≥ 3.1.50)

```bash
cd wasm
emcmake cmake . -DFATP_INCLUDE_DIR=../../FatP/include/fat_p -DCMAKE_BUILD_TYPE=Release
cmake --build .
# balancer.js and balancer.wasm copied to demo/ automatically
open ../demo/index.html
```

---

## Tests (Phase 10 — 20 functional suites, 27 HSC/compile-check files, 1 orchestrator)

| Suite | File | Coverage |
|-------|------|----------|
| Include-all | `test_include_all_balancer_headers.cpp` | All 19 public headers in one TU |
| Self-contained (×18) | `test_*_HeaderSelfContained.cpp` | Each header compiles alone |
| Cost model | `test_cost_model.cpp` | EMA, cold start, predict/update, per-class correction, DegradationCurve |
| Aging engine | `test_aging.cpp` | Priority aging, ceiling enforcement, deadline expiry, cancel interaction |
| Policies | `test_policies.cpp` | All 7 policies, Composite chains, admission table |
| Admission | `test_admission.cpp` | All three layers, every error code |
| Node FSM | `test_node_fsm.cpp` | State transitions, priority admission table |
| Balancer core | `test_balancer_core.cpp` | End-to-end: submit → route → complete → learn |
| Fault scenarios | `test_fault_scenarios.cpp` | Crash, slowdown, partition, cancel, drain-swap-resume, jitter |
| Affinity matrix | `test_affinity_matrix.cpp` | Cold start, EMA convergence, clamping, JSON round-trip, file persistence, error paths |
| Orchestrator | `test_balancer.cpp` | All suites linked into one binary |

---

## Node State and Priority Admission

| State | Critical | High | Normal | Low | Bulk |
|-------|----------|------|--------|-----|------|
| Idle | ✓ | ✓ | ✓ | ✓ | ✓ |
| Busy | ✓ | ✓ | ✓ | ✓ | ✓ |
| Overloaded | ✓ | ✓ | — | — | — |
| Draining | ✓ | — | — | — | — |
| Recovering | ✓ | ✓ | — | — | — |
| Failed | — | — | — | — | — |
| Offline | — | — | — | — | — |

---

## Roadmap

| Phase | Scope |
|-------|-------|
| **1** ✓ | Core interfaces, RoundRobin, LeastLoaded, online cost model, admission control |
| **2** ✓ | Priority queues, per-JobClass correction, WeightedCapacity, ShortestJobFirst, EarliestDeadlineFirst, Composite |
| **3** ✓ | AgingEngine, deadline expiry, DegradationCurve, queueDepthAtDispatch |
| **4** ✓ | FaultInjector, WorkStealing, FAT-P ThreadPool migration, SlotMap cancel, drain-swap-resume |
| **5** ✓ | Node×JobClass affinity Tensor, FastHashMap migration, model persistence (JsonLite), AffinityRouting |
| **6** ✓ | WASM demo — cluster visualization, live learning heatmap |
| **7** ✓ | FAT-P FeatureManager integration — BalancerFeatures constants, FeatureSupervisor, TelemetryAdvisor, TelemetrySnapshot, SimulatedCluster::tick() |
| **8** ✓ | Debt resolution: latency metrics pipeline (DEBT-002), adm. control API (DEBT-001), PolicyFactory (DEBT-003), HoldingQueue API (DEBT-004), reprioritise() (DEBT-006) |
| **9** ✓ | Debt resolution: HoldingQueue implementation (DEBT-005) — bounded priority-ordered overflow store |
| **10** ✓ | Latency alert enabled, SimulatedNode override wiring, WASM update (PolicyFactory + full ClusterMetrics stats), README refresh |

---

## Guidelines

All governance documents are in `guidelines/`. They are Claude's documents — read before contributing.

⚠️ APIs are not yet stable.
