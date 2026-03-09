# fatp-balancer

A production-grade cluster load balancer built as a structured demonstration of
the [FAT-P](../FatP) library. Implementation is by Claude; architecture direction,
governance, and review are human; and some design ideas — notably the neural
network advisor — originated with [Grok](https://x.ai) (ARA). Every header,
policy, test, and tool was written by Claude against that shared design space.

The project is intentionally comprehensive. It isn't a toy that uses one or two
FAT-P primitives — it exercises the library across its full surface: `Expected`
for typed error propagation throughout the public API, `FastHashMap` for the
cost model's hot predict path, `RowMajorTensor` backing the node × job-class
affinity matrix, `StrongId` for type-safe node and job identifiers, `ThreadPool`
and `SlotMap` powering the simulated node worker threads with O(1) cancel,
`FeatureManager` (with `Implies`, `Preempts`, and `MutuallyExclusive` edges)
driving live alert-triggered policy switching, and the full JSON stack for model
persistence and telemetry snapshots. The result is a codebase where FAT-P is
load-bearing at every layer, not incidental.

**The transferability principle:** The balancer never knows it is talking to a
simulation. `include/balancer/` and `policies/` implement the scheduling logic
against the `INode` interface. `sim/` provides a thread-backed implementation
for local testing. A production deployment substitutes its own `INode` —
sockets, gRPC, or any other transport — and the balancer code ships unchanged.

---

## Architecture

```
include/balancer/     ← Core contracts and scheduling logic (ships to production)
policies/             ← Scheduling strategy implementations (ships to production)
sim/                  ← Thread-backed simulation (test fixture, never ships)
wasm/                 ← Emscripten bindings for the browser demo
demo/                 ← Self-contained cluster visualization (index.html)
tests/                ← All test suites
tools/nn_advisor/     ← Python NN training harness + inference loop
guidelines/           ← Governance documents (Claude's documents — read before contributing)
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
| `balancer/AgingEngine.h` | Core | Priority aging, deadline expiry |
| `balancer/AffinityMatrix.h` | Learning | Node × JobClass `fat_p::RowMajorTensor` — EMA-learned per-cell multipliers with JSON persistence |
| `balancer/CostModel.h` | Learning | Per-node EMA + AffinityMatrix correction + DegradationCurve; `fat_p::FastHashMap` node state; JSON save/load |
| `balancer/HoldingQueue.h` | Core | Bounded priority-ordered overflow store for jobs shed by admission control |
| `balancer/PolicyFactory.h` | Core | `makePolicy(name)` → `fat_p::Expected<unique_ptr<ISchedulingPolicy>, ...>` |
| `balancer/BalancerFeatures.h` | Core | `kFeature*`, `kAlert*`, `kPolicy*` string constants for `fat_p::FeatureManager` |
| `balancer/FeatureSupervisor.h` | Core | Alert-driven policy and feature transitions via `fat_p::FeatureManager` with `Implies`/`Preempts`/`MutuallyExclusive` edges |
| `balancer/TelemetryAdvisor.h` | Core | ClusterMetrics threshold evaluator — drives FeatureSupervisor with priority-ordered alerts |
| `balancer/AdvisorConcept.h` | Core | C++20 `concept Advisor<T>` — constrains advisor query surface (`currentAlert`, `supervisor`) |
| `balancer/NNAdvisor.h` | Learning | File-backed inference advisor — reads `alert.json` written by the Python harness and calls `applyChanges()` |
| `balancer/Balancer.h` | Core | Central coordinator; `fat_p::Expected` error propagation throughout; drain-swap-resume policy switching |
| `policies/RoundRobin.h` | Policies | Priority-filtered round-robin |
| `policies/LeastLoaded.h` | Policies | Cost-model-aware least-loaded routing |
| `policies/WeightedCapacity.h` | Policies | Routes to highest available weighted capacity |
| `policies/ShortestJobFirst.h` | Policies | Minimizes predicted time-to-completion |
| `policies/EarliestDeadlineFirst.h` | Policies | Routes deadline jobs to node with maximum slack |
| `policies/Composite.h` | Policies | Chains policies in order; first success wins |
| `policies/WorkStealing.h` | Policies | Steal-bias routing — drains overloaded nodes |
| `policies/AffinityRouting.h` | Policies | Routes by node × class affinity score; cold-start falls back to LeastLoaded |
| `sim/FaultInjector.h` | Sim | FaultType enum and FaultConfig |
| `sim/SimulatedNode.h` | Sim | INode backed by `fat_p::ThreadPool` with `fat_p::SlotMap` cancel and priority override |
| `sim/SimulatedCluster.h` | Sim | Manages N SimulatedNodes; `tick()` drives TelemetryAdvisor; `startSnapshotWriter()` writes periodic JSON for the Python inference loop |
| `sim/TelemetrySnapshot.h` | Sim | JSON-serialisable cluster snapshot with per-node stats |

---

## Tools

### `tools/nn_advisor/` — Python NN training harness

Trains a scikit-learn `MLPClassifier` to predict the correct alert label from a
`TelemetrySnapshot` and writes the result to `alert.json` for `NNAdvisor` to
consume. The `determine_alert()` function mirrors `TelemetryAdvisor::determineAlert()`
exactly — same three-bucket denominator (`knownNodes = active + unavailable + overloaded`),
same priority ordering — so Python training labels are always consistent with
what the C++ advisor produces.

| Command | Description |
|---------|-------------|
| `generate` | Synthesise a labelled JSONL dataset from sampled cluster topologies (no C++ sim required) |
| `collect` | Label an existing directory of `sim::formatJson()` snapshot files |
| `train` | `StandardScaler → MLPClassifier(64×64)` pipeline; saves `.joblib` model |
| `infer` | One-shot: snapshot JSON → `alert.json` |
| `loop` | Watches snapshot file mtime; runs inference on change; writes `alert.json` for `NNAdvisor` |

```bash
python3 tools/nn_advisor/nn_advisor_harness.py generate --samples 20000 --out dataset.jsonl
python3 tools/nn_advisor/nn_advisor_harness.py train --dataset dataset.jsonl --model model.joblib
python3 tools/nn_advisor/nn_advisor_harness.py loop \
    --snapshot cluster_state.json --alert alert.json --model model.joblib
```

---

## Build

**Prerequisites:** CMake 3.20+, a C++20 compiler, FAT-P at a sibling path (`../FatP`).

```bash
# Linux / macOS
./build.sh

# Windows (PowerShell)
.\build.ps1

# Custom FAT-P location
FATP_INCLUDE_DIR=/path/to/FatP/include/fat_p ./build.sh

# With sanitizers
./build.sh --asan --tsan --ubsan
```

### Browser demo (no build required)

```bash
open demo/index.html        # macOS
xdg-open demo/index.html    # Linux
start demo/index.html       # Windows
```

### WASM build (optional, requires Emscripten ≥ 3.1.50)

```bash
cd wasm
emcmake cmake . -DFATP_INCLUDE_DIR=../../FatP/include/fat_p -DCMAKE_BUILD_TYPE=Release
cmake --build .
# balancer.js and balancer.wasm copied to demo/ automatically
```

---

## Tests (47 CTest targets: 17 functional suites + 1 orchestrator + 1 include-all + 28 HSC compile checks)

| Suite | File | Tests |
|-------|------|-------|
| Policies | `test_policies.cpp` | 43 — all 8 policies, Composite chains, admission table |
| Cost model | `test_cost_model.cpp` | 29 — EMA, cold start, predict/update, DegradationCurve |
| Aging engine | `test_aging.cpp` | 23 — priority aging, ceiling enforcement, deadline expiry, cancel |
| Feature supervisor | `test_feature_supervisor.cpp` | 18 — alert transitions, replace(), batchEnable/disable, graph validation |
| Holding queue | `test_holding_queue.cpp` | 18 — enqueue, dequeue, priority order, cancel, HoldingHandle bit |
| Node FSM | `test_node_fsm.cpp` | 17 — state transitions, priority admission table |
| NNAdvisor | `test_nn_advisor.cpp` | 17 — all alert types, idempotency, transition, error paths, accessors |
| Affinity matrix | `test_affinity_matrix.cpp` | 16 — cold start, EMA convergence, clamping, JSON round-trip, persistence |
| Admission | `test_admission.cpp` | 15 — all three layers, every error code |
| TelemetryAdvisor | `test_telemetry_advisor.cpp` | 14 — threshold logic, all four alert classes, latency alert |
| Telemetry integration | `test_telemetry_integration.cpp` | 13 — end-to-end snapshot → advisor → policy switch |
| Policy factory | `test_policy_factory.cpp` | 13 — all registered names, unknown name error |
| Fault scenarios | `test_fault_scenarios.cpp` | 13 — crash, slowdown, partition, cancel, drain-swap-resume |
| Debt regression | `test_phase8_debt.cpp` | 10 — regression coverage for resolved technical debt |
| Balancer core | `test_balancer_core.cpp` | 10 — submit → route → complete → learn end-to-end |
| Feature manager cascade | `test_feature_manager_cascade_probe.cpp` | 8 — Implies/Preempts/MutuallyExclusive graph semantics |
| Simulated node override | `test_simulated_node_override.cpp` | 2 — priority override read in executeJob() |
| Orchestrator | `test_balancer.cpp` | Links all functional suites into one binary |
| Include-all | `test_include_all_balancer_headers.cpp` | All 18 public headers in one TU |
| Self-contained (×28) | `test_*_HeaderSelfContained.cpp` | Each header compiles in isolation |

---

## WASM JavaScript API

`wasm/BalancerBindings.cpp` wraps the full C++ stack in an Emscripten `embind`
class. `tick()` drives `TelemetryAdvisor::evaluate()` automatically — alert-triggered
policy switches happen with no JS-side coordination, demonstrating the
transferability principle at the transport boundary.

| Method | Signature | Description |
|--------|-----------|-------------|
| constructor | `(numNodes, numClasses)` | Create cluster |
| `submitJob` | `(priority, jobClass, estimatedCost) → bool` | Submit one job |
| `tick` | `() → void` | Sample metrics and drive TelemetryAdvisor |
| `getNodeStates` | `() → JSON string` | Array of NodeSnapshot objects |
| `getAffinityMatrix` | `() → JSON string` | Flat float array + shape metadata |
| `getStats` | `() → JSON string` | `totalCompleted`, `totalRejected`, `throughputPerSecond`, `meanP50LatencyUs`, `maxP99LatencyUs`, `holdingQueueDepth`, `activeAlert` |
| `getActiveAlert` | `() → string` | Current alert string |
| `pushNNAlert` | `(alertName) → bool` | Validate and apply an alert directly (browser substitute for NNAdvisor — no filesystem in WASM) |
| `switchPolicy` | `(name) → void` | Drain-swap-resume policy switch |
| `injectFault` | `(nodeId, type) → void` | `"crash"` \| `"slowdown"` \| `"recover"` |
| `getHoldingQueueDepth` | `() → int` | Current depth of the holding queue |

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

⚠️ APIs are not yet stable.
