# fatp-balancer Development Guidelines

## Document Governance

This is the **authoritative** fatp-balancer guideline document.

The documents below form the governance set (the demerit ledger is a record, not a governance document):

| Document | Role | Authority |
|----------|------|-----------|
| **Development Guidelines** (this) | Normative rules, AI behavior, code standards, maintainer guidance | HIGHEST — this document wins |
| **Teaching Documents Style Guide** | All teaching docs: Overviews, User Manuals, Companion Guides, Case Studies, Foundations, Handbooks, Pattern Guides, Design Notes, Benchmark Results | PRIMARY for all documentation |
| **Test Suite Style Guide** | Test structure, coverage, assertions | PRIMARY for test code |
| **Benchmark Code Style Guide** | Benchmark methodology, statistics | PRIMARY for benchmark code |
| **CI Workflow Style Guide** | GitHub Actions workflows, job matrix, gating | PRIMARY for CI workflows |
| **Meta Header Guidelines** | `BALANCER_META` schema, placement, and linking rules | NORMATIVE for `BALANCER_META` blocks |
| **Systemic Hygiene Policy** | Header composability, ODR safety, namespace collision prevention | NORMATIVE for header correctness |

**Precedence rules:**
- Development Guidelines override all other documents
- Overlap between documents is intentional
- Each document must be standalone
- No document assumes another has been read

**Which document do I write?**

| Question | Document |
|----------|----------|
| "Should I use this component?" | Overview |
| "How do I use this component?" | User Manual |
| "Why is it designed this way?" | Companion Guide |
| "Why did this fail, and how do I fix it?" | Case Study |
| "What background do I need?" | Foundations |
| "What discipline should teams adopt?" | Handbook |
| "How do I apply this pattern?" | Pattern Guide |
| "What decision did we make?" | Design Note |
| "How does this perform?" | Benchmark Results |
| "How do I test this component?" | Test Suite Style Guide |
| "How do I write a benchmark?" | Benchmark Code Style Guide |
| "How do I write or modify CI workflows?" | CI Workflow Style Guide |
| "How do I add or update `BALANCER_META`?" | Meta Header Guidelines |
| "Can these headers be included together?" | Systemic Hygiene Policy |
| "Is this code/test/doc compliant?" | Development Guidelines |

---

## 1. Project Design Principles

### 1.1 Core Technical Requirements

| Requirement | Specification |
|-------------|---------------|
| **C++ Standard** | C++20 minimum |
| **Architecture** | Header-only for `include/balancer/` and `policies/`; compiled for `sim/` and `app/` |
| **Dependencies** | FAT-P (required), std, SDL2 (WASM/visual demo only) |
| **Weight** | Lightweight core; simulation layer may carry more |
| **Target Domain** | Distributed systems, cluster scheduling, real-time resource management |

#### 1.1.1 C++ Standard Policy

**Minimum required standard:** C++20.

All fatp-balancer components require C++20. There is no C++17 compatibility layer.

All C++ standard and library feature detection must live in:
- `CppFeatureDetection.h` — C++ language/library feature detection
- `PlatformDetection.h` — Compiler, OS, hardware detection

Other headers may not probe `__cplusplus` or `_MSVC_LANG` directly.

#### 1.1.2 Conditional Compilation Policy

**Allowed:**
- Detection of C++23/26 features
- Detection of unreliable C++20 library features (`std::format`, `std::jthread`)
- Platform-specific code via `PlatformDetection.h`

**Prohibited:**
- C++17 fallback code paths
- Syntax-emulation macros (`BALANCER_CONCEPT`, `BALANCER_REQUIRES`)

### 1.2 Design Philosophy

fatp-balancer exists to prove that FAT-P's concurrency, container, and domain stacks compose into a real distributed systems primitive. Every component in `include/balancer/` must earn its existence by solving a real problem in the scheduling domain. No padding, no speculative complexity.

The single most important design constraint: **the balancer must never know it is talking to a simulation.** The `INode` interface is the boundary. Code in `include/balancer/` depends only on `INode`. Code in `sim/` implements `INode` with threads. Code in a production deployment implements `INode` with sockets or gRPC. The `include/balancer/` and `policies/` directories ship to production unchanged.

### 1.3 Versioning and Compatibility

- No version number above 1
- Project has not been released; no deprecated features, no backward compatibility concerns
- **Never add backward compatibility aliases** — if something is renamed, rename it completely
- **Never design for "incremental adoption"** — changes are atomic and complete
- **Never preserve broken patterns** — if a design is wrong, fix it everywhere immediately
- **"Backward compatible" is not a virtue** — do not weigh it as a positive in any design decision

### 1.4 Policy-Based Design

Policy template parameters are **optional**, not mandatory. Use them when:
- Users need custom strategies, allocators, or thread-safety models
- Behavior customization has concrete identified use cases

Do **NOT** use policies for future flexibility without identified use cases. Start simple; add policies when real needs emerge.

### 1.5 Separation of Concerns

| Layer | Location | Concern |
|-------|----------|---------|
| Interface | `include/balancer/` | Contracts, types, core balancer logic |
| Policies | `policies/` | Scheduling strategies |
| Simulation | `sim/` | Thread-backed node implementations |
| Application | `app/` | CLI demo entry point |
| WASM | `wasm/` | Browser entry point |
| Tests | `tests/` | All test suites |

The `INode` interface is the hard boundary between `include/balancer/` and everything below it. Headers above the boundary must not include anything from `sim/`, `app/`, or `wasm/`.

### 1.6 Dependency Policy

**Allowed:**
- FAT-P (mandatory; expected at sibling path `../FatP` or via `FATP_INCLUDE_DIR`)
- Standard library (`std`)
- SDL2 and SDL2_ttf in `wasm/` and WASM demo targets only; must compile out cleanly when absent
- System APIs and compiler intrinsics with proper gating

**Prohibited:**
- Any other third-party library
- Boost, Abseil, fmt, or similar

FAT-P components may be used by any layer. SDL2 must never be included by `include/balancer/` or `policies/`.

### 1.7 Repository Structure

```
fatp-balancer/
├── include/balancer/       # Core balancer headers — the transferable interface
│   ├── Job.h               # Job, Priority, Cost, Deadline, JobClass
│   ├── INode.h             # Transferable node contract
│   ├── ISchedulingPolicy.h # Strategy interface
│   ├── ClusterView.h       # Immutable snapshot for policy decisions
│   ├── LoadMetrics.h       # Per-node and cluster metrics
│   ├── AdmissionControl.h  # Three-layer admission
│   ├── AgingEngine.h       # Priority aging + deadline expiry
│   ├── CostModel.h         # Online learning model
│   ├── Balancer.h          # Core coordinator
│   └── BalancerConfig.h    # JSON-loadable configuration
├── policies/               # Scheduling strategy implementations
│   ├── RoundRobin.h
│   ├── LeastLoaded.h
│   ├── WeightedCapacity.h
│   ├── WorkStealing.h
│   ├── ShortestJobFirst.h
│   ├── EarliestDeadlineFirst.h
│   └── Composite.h
├── sim/                    # Simulation layer (INode backed by ThreadPool)
│   ├── SimulatedNode.h
│   ├── SimulatedCluster.h
│   └── FaultInjector.h
├── app/                    # CLI demo
│   └── main.cpp
├── wasm/                   # WebAssembly entry point
│   └── wasm_main.cpp
├── tests/                  # All test suites
│   ├── test_admission.cpp
│   ├── test_aging.cpp
│   ├── test_cost_model.cpp
│   ├── test_policies.cpp
│   ├── test_node_fsm.cpp
│   ├── test_balancer_core.cpp
│   └── test_fault_scenarios.cpp
├── bench/                  # Benchmark suite
│   └── benchmark_balancer.cpp
├── docs/                   # Architecture diagrams, design notes
│   └── graphviz.svg
├── guidelines/             # This governance document set
├── .github/workflows/      # CI workflows (metadata-exempt; YAML)
├── CMakeLists.txt
├── build.sh
├── build.ps1
└── build.bat
```

#### 1.7.1 Path Conventions

**BALANCER_META.path** must be the repo-relative path using forward slashes.

**Include directives:**
- Include balancer headers as: `#include "balancer/Job.h"`
- Include FAT-P headers as: `#include "Expected.h"` (FAT-P's include dir on include path)
- Ordering: group by architectural layer with alphabetical sort within each group (see §5.11)

#### 1.7.2 BALANCER_META Scope

Every **repository-authored code file** must contain a `BALANCER_META` block in the format specified by the Meta Header Guidelines.

Exclusions (no `BALANCER_META` required):
- YAML files (`.yml`, `.yaml`) including `.github/workflows/*`
- Generated directories (`build/`, `.vcpkg_installed/`)
- Non-code artifacts (`docs/` SVG outputs, `bench/results/`)

---

## 2. Layer System

### 2.1 Official Layers

fatp-balancer uses a six-layer architecture. Each header must declare exactly one layer via `BALANCER_META.layer`.

```
Interface → Core → Policies → Learning → Sim → Testing
```

| Layer | Description | May Depend On |
|-------|-------------|---------------|
| **Interface** | Contracts, pure types, INode, ISchedulingPolicy | `std` + FAT-P Foundation |
| **Core** | Balancer coordinator, admission, aging | Interface + FAT-P Containers/Concurrency |
| **Policies** | Scheduling strategy implementations | Interface + Core + FAT-P |
| **Learning** | CostModel, DegradationCurve, affinity matrix | Interface + Core + FAT-P |
| **Sim** | SimulatedNode, FaultInjector (never ships to production) | All above |
| **Testing** | Tests, benchmarks | All above |

**Layer dependency rule:** Components may only `#include` headers from layers **at or below** their own.

### 2.2 Layer Classification Requirements

Every header file must declare its architectural layer in `BALANCER_META.layer`.

```cpp
#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: INode
  file_role: public_header
  path: include/balancer/INode.h
  namespace: balancer
  layer: Interface
  summary: Transferable node contract — implement this to connect to a real cluster.
*/
```

**Rules:**
1. Components may only `#include` headers from layers at or below their own
2. Mismatch between `BALANCER_META.layer` and actual includes is a **Critical** violation
3. `sim/` headers must never be included by `include/balancer/` or `policies/`

### 2.3 The Transferability Boundary

The `INode` and `ISchedulingPolicy` interfaces in the Interface layer are the **transferability boundary**. They are the only surface that a production deployment needs to implement.

**Hard rule:** No `sim/` path may appear in any `#include` within `include/balancer/` or `policies/`. CI enforces this via an include-scan step.

---

## 3. Component Design Standards

### 3.1 Interface Design

Public interfaces use `Expected<T, E>` for all fallible operations. No exceptions in domain logic.

```cpp
// Good
Expected<JobHandle, SubmitError> submit(Job job);
Expected<void, CancelError>     cancel(JobHandle handle);

// Bad — exceptions couple the balancer to caller exception-handling strategy
JobHandle submit(Job job);  // throws on failure
```

### 3.2 The INode Contract

Any class implementing `INode` must satisfy:
- `submit()` is thread-safe
- `cancel()` is thread-safe
- `status()` is thread-safe and non-blocking
- `metrics()` returns a value snapshot (no reference stability required)
- `onComplete` callback is invoked exactly once per job, from any thread

Implementations that violate these contracts break the balancer. Document deviations explicitly.

### 3.3 The ISchedulingPolicy Contract

Scheduling policies must be:
- **Stateless between `selectNode()` calls** — state lives in the CostModel, not the policy
- **Exception-free** — return `Expected`, never throw
- **Non-blocking** — `selectNode()` must return in bounded time

### 3.4 The CostModel Contract

The CostModel is shared across all scheduling policies. It must be:
- **Thread-safe** — multiple policies may call `predict()` concurrently
- **Always valid** — `predict()` never returns a negative duration
- **Cold-start safe** — returns neutral predictions (1.0 multiplier) before sufficient observations

---

## 4. Code Review Protocol

### 4.1 Review vs. Rewrite

**Review output is a findings list with targeted patches — never a complete rewritten file.**

A review produces:
1. A numbered list of findings with evidence (verbatim code quotes, not paraphrases)
2. Targeted patches for each finding
3. An escalation note if the root cause requires redesign

A review does **not** produce a complete replacement file unless the user explicitly requests one.

### 4.2 Formatting Standards

fatp-balancer uses the same `.clang-format` configuration as FAT-P:

| Setting | Value |
|---------|-------|
| `BreakBeforeBraces` | Allman |
| `IndentWidth` | 4 |
| `UseTab` | Never |
| `ColumnLimit` | 120 (target 100) |
| `InsertBraces` | true |
| `PointerAlignment` | Left |
| `NamespaceIndentation` | None |

---

## 5. Code Standards

### 5.1 Operational Modes for AI Assistants

| Mode | Trigger | Output |
|------|---------|--------|
| Review | "review", "audit", "check" | Findings list + targeted patches |
| Implementation | "implement", "write", "create" | Complete files |
| Modification | "fix", "update", "change" | Modified sections or complete file if >30% changes |

**No unsolicited code:** Do not generate code unless the current mode is Implementation or Modification.

### 5.2 Naming Conventions

| Element | Style | Example |
|---------|-------|---------|
| Class/Struct | PascalCase | `CostModel`, `AgingEngine` |
| Function/Method | camelCase | `selectNode()`, `submitJob()` |
| Class instance member | `m` prefix + PascalCase | `mNodeRegistry`, `mCostModel` |
| Struct/aggregate member | camelCase (no prefix) | `id`, `priority`, `estimatedCost` |
| Static member variable | `s` prefix + PascalCase | `sInstanceCount` |
| Local variable | camelCase | `candidateNode`, `predictedCost` |
| Template parameter | PascalCase | `Policy`, `NodeType` |
| Type alias (STL-compatible) | snake_case | `value_type` |
| Type alias (project-specific) | PascalCase | `NodeId`, `JobHandle` |
| Preprocessor constant/macro | SCREAMING_SNAKE | `BALANCER_MAX_PRIORITY_LEVELS` |
| Compile-time constant (`constexpr`) | `k` prefix + PascalCase | `kDefaultAlpha`, `kMaxRetries` |
| Namespace | lowercase | `balancer`, `balancer::detail` |

**STL-compatible method names** (for range-based for, `std::size()`, etc.) retain snake_case: `begin()`, `end()`, `size()`, `empty()`, `push_back()`.

**The m-prefix rule:** Apply to class instance members (types with constructors, destructors, or private state). Aggregate structs (all-public, no user-declared special members) use plain camelCase.

### 5.3 Header Guards

Use `#pragma once` exclusively.

### 5.4 Header-Only Enforcement for Interface and Policies Layers

`include/balancer/` and `policies/` are header-only. All code lives in `.h` files.

- Functions defined at namespace scope require `inline`
- Variables at namespace scope require `inline` or `constexpr`
- Anonymous namespaces in headers are **strongly discouraged**
- If used, require a justification comment: `// Anonymous namespace: [justification]`
- No mutable state in anonymous namespaces — ever

`sim/` and `app/` are compiled (`.cpp` files). They do not need `inline`.

### 5.5 Documentation Comments

**Two types of comments:**

| Type | Purpose | Audience |
|------|---------|----------|
| **Doxygen** (`/** */`) | Contract specification | IDE tooltips, API reference |
| **Regular** (`//`) | Design rationale, implementation notes | Code readers, maintainers |

Doxygen: brief, contract-focused, no implementation details.
Regular comments: extensive where needed — explain *why*, document algorithmic choices, warn about gotchas.

### 5.6 Doxygen Standards

**Required for:**
- All public classes, structs, and interfaces
- All public methods (brief + params + return + exceptions)
- All public type aliases and constants

**File header layout:**
1. `#pragma once` on line 1
2. `BALANCER_META` immediately after
3. Doxygen `@file` header after `BALANCER_META`

```cpp
#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: INode
  file_role: public_header
  path: include/balancer/INode.h
  namespace: balancer
  layer: Interface
  summary: Transferable node contract.
*/

/**
 * @file INode.h
 * @brief Transferable node contract — implement to connect to a real cluster.
 *
 * @see INode_User_Manual.md for usage documentation.
 * @see INode_Companion_Guide.md for design rationale.
 */
```

### 5.7 Prohibited Content

- No `using namespace` at global scope in headers
- No fictional macros — document only what exists
- No special symbols or unusual Unicode in code
- No `sim/` includes in `include/balancer/` or `policies/`

### 5.8 Using Directives Policy

| Scope | Allowed |
|-------|---------|
| Global scope in headers | **NO** |
| Function/block scope | YES |
| Namespace alias | YES |

### 5.9 `[[nodiscard]]` Usage

Apply to:
- Resource acquisition functions (ignoring the handle leaks the resource)
- Error code returns (ignoring hides failures)
- Predicates (`isEmpty()`, `isWarm()`)
- Factory functions

Do not apply to:
- Methods with useful side effects as the primary purpose
- Chainable methods where return value is optional

### 5.10 Include Ordering Convention

Group includes by layer, alphabetical within each group, separated by blank lines:

```cpp
// Standard library
#include <atomic>
#include <chrono>
#include <functional>

// FAT-P Foundation
#include "ConcurrencyPolicies.h"
#include "Expected.h"
#include "StrongId.h"

// FAT-P Containers
#include "FastHashMap.h"
#include "SlotMap.h"

// FAT-P Concurrency
#include "LockFreeQueue.h"
#include "ThreadPool.h"

// FAT-P Domain
#include "DiagnosticLogger.h"
#include "JsonLite.h"
#include "StateMachine.h"

// Balancer Interface
#include "balancer/INode.h"
#include "balancer/Job.h"

// Balancer Core
#include "balancer/Balancer.h"
```

---

## 6. Unit Testing Standards

### 6.1 Testing Philosophy

- Thorough testing with 100% coverage goal
- Consider all corner cases and edge conditions
- Tests must validate both happy paths and failure modes
- Fault injection paths must be tested, not just the steady state

### 6.2 Test File Structure

Tests use the FAT-P `FatPTest.h` framework with adapted namespaces:

```cpp
#include <iostream>

#include "balancer/ComponentName.h"
#include "FatPTest.h"

namespace balancer::testing::componentns
{

FATP_TEST_CASE(feature_one)
{
    // ...
    return true;
}

} // namespace balancer::testing::componentns

namespace balancer::testing
{

bool test_ComponentName()
{
    FATP_PRINT_HEADER(COMPONENT NAME)

    TestRunner runner;

    FATP_RUN_TEST_NS(runner, componentns, feature_one);

    return 0 == runner.print_summary();
}

} // namespace balancer::testing

#ifdef ENABLE_TEST_APPLICATION
int main()
{
    return balancer::testing::test_ComponentName() ? 0 : 1;
}
#endif
```

### 6.3 Key Testing Requirements

| Element | Requirement |
|---------|-------------|
| **Namespace** | `balancer::testing::componentns` |
| **Test definition** | `FATP_TEST_CASE(name)` |
| **Test execution** | `FATP_RUN_TEST_NS(runner, componentns, name)` |
| **Assertions** | `FatPTest.h` macros |
| **Main function** | Always `#ifdef ENABLE_TEST_APPLICATION` guarded |

---

## 7. Component Documentation Standards

### 7.1 Required Component Documentation Sections

Every non-trivial component gets these sections:

1. **Intent** — What problem does this component solve?
2. **Invariant** — What is always true about this component's state?
3. **Complexity Model** — How expensive are operations?
4. **Concurrency Model** — What is thread-safe? What is not?
5. **Error Model** — Which operations return `Expected`? What error codes?
6. **When to Use / When Not to Use** — Decision criteria

### 7.2 Error Reporting Convention

All fallible operations return `Expected<T, E>`. No exceptions in domain logic.

Error types are project-specific enums in `include/balancer/`. Every error enum value must be documented with its semantics and the conditions that produce it.

---

## 8. Documentation Quality Standards

### 8.1 Documentation Philosophy

The goal is a reader who has never seen the project being able to understand the design decisions, not just the API surface. Documentation explains **what**, **why**, and **when** — not just how.

### 8.2 Vocabulary Ban

The following vague terms are **banned** in component Overviews, User Manuals, and Companion Guides:

| Banned | Required replacement |
|--------|---------------------|
| "fast" | "O(1)", "O(log n)", specific complexity |
| "efficient" | Mechanism-specific: "zero-copy", "lock-free", "single allocation" |
| "safe" | Specify: "overflow-safe", "thread-safe", "exception-safe" |
| "simple" | Remove or explain specifically |
| "powerful" | Remove |
| "easy" | Remove |
| "seamless" | Remove |
| "robust" | Specify the failure mode handled |
| "flexible" | Specify what can be customized |
| "scalable" | Provide complexity or contention model |

Teaching documents (Case Studies, Handbooks, Foundations) are exempt from this ban.

### 8.3 The Transferability Story

All documentation for `include/balancer/` components must make the transferability principle explicit: this code ships to production unchanged; the simulation is a test fixture. Never conflate the two in documentation prose.

### 8.4 Performance Claims

No specific benchmark numbers in headers or documentation prose:
- No multiplier claims ("3x faster than X")
- No absolute timing numbers ("~24 ns per job")
- Do describe architectural characteristics: "O(1) prediction," "lock-free ingestion path," "EMA update is a single multiply-add"

Benchmark result files in `bench/results/` are exempt — they exist to hold data.

### 8.5 Library Maturity Claims

Do not use "production-tested," "battle-tested," or "production-ready." The project has not been deployed to production. Acknowledge what production-grade schedulers have that this does not.

### 8.6 Diagram Guidelines

Use Mermaid diagrams for:
- Node state machine (mandatory — shows the lifecycle)
- Priority aging flow
- Admission control layers
- Learning model update pipeline
- Fault injection and recovery sequence

```
stateDiagram-v2 — for node FSM, aging state
flowchart       — for admission control, learning pipeline
sequenceDiagram — for job submit/complete cycle
```

---

## 9. Benchmark Environment Reference

### 9.1 Windows Test Machine

| Component | Specification |
|-----------|---------------|
| Processor | Intel Core Ultra 9 285K |
| RAM | 64.0 GB |
| OS | Windows 11 Pro |

**Release build (MSVC 2022):**
```
/std:c++20 /O2 /DNDEBUG /MD /EHsc /W3
/D "NOMINMAX" /D "WIN32_LEAN_AND_MEAN"
```

### 9.2 Linux Test Machine

**Release build (GCC 13+):**
```bash
g++ -std=c++20 -O3 -DNDEBUG -march=native -flto
```

> **Critical:** Benchmarks must always run with Release builds. Never benchmark Debug builds.

---

## 10. Quick Reference Checklists

### Before Submitting Code

- [ ] Compiles successfully
- [ ] No truncated files
- [ ] No AI process comments (`NEW`, `FIXED`, `ADDED`, etc.)
- [ ] Lines wrapped at 100 columns (120 max, macros exempt)
- [ ] No `sim/` includes in `include/balancer/` or `policies/`
- [ ] `BALANCER_META.layer` present and verified against actual includes
- [ ] For headers: `#pragma once` first, then `BALANCER_META`, then Doxygen
- [ ] Includes grouped by layer with alphabetical sort within groups (§5.10)
- [ ] No backward compatibility aliases

### Before Submitting Tests

- [ ] Uses `FATP_TEST_CASE` and `FATP_RUN_TEST_NS`
- [ ] Test functions in `balancer::testing::componentns` namespace
- [ ] Uses `FatPTest.h` assertions
- [ ] Fault scenarios tested, not just happy paths
- [ ] `#ifdef ENABLE_TEST_APPLICATION` guarded `main()`

### Before Submitting Documentation

- [ ] All required sections present
- [ ] Each section explains **what**, **why**, and **when**
- [ ] Vocabulary ban enforced (Overviews, User Manuals, Companion Guides)
- [ ] Transferability principle explicit in `include/balancer/` component docs
- [ ] No specific benchmark numbers in prose
- [ ] No library maturity claims

---

## 11. AI Operational Behavior

### 11.1 Scope

This contract governs AI behavior when:
- Generating new fatp-balancer components or artifacts
- Reviewing or modifying existing code, tests, or documentation
- Answering questions about balancer design or implementation
- Producing any output that claims compliance with these standards

### 11.2 The Override Rule

> **If any AI output conflicts with these Development Guidelines, the AI output is invalid.**

Human maintainers may override AI concerns with explicit justification.

### 11.3 Non-Goals for AI Assistants

AI assistants **must not**:

1. Infer unspecified guarantees or invariants
2. Generalize behavior from `std::` equivalents unless explicitly documented
3. Invent benchmark results, performance claims, or test outcomes
4. Assume concurrency safety unless the component explicitly documents it
5. Optimize for brevity at the expense of required structure
6. Use banned vocabulary without mechanism-specific replacement
7. Pretend to have compiled or executed code without actually doing so
8. Suggest "gradual adoption," "incremental migration," or "backward compatible" approaches
9. Preserve existing patterns solely because they exist
10. Return complete rewritten files when asked to **review** — review output is findings + targeted patches
11. Deliver a band-aid when the root cause is known — if the root cause is identified, fix the root cause
12. Include `sim/` includes in `include/balancer/` or `policies/` headers — this breaks transferability

#### 11.3.1 The Band-Aid Rule

> **If you know the root cause, fix the root cause.**

If the AI's analysis identifies a structural defect, the delivered fix must address the structural defect. The AI must not:
- Ship a probabilistic mitigation when a provably correct fix is known
- Weaken a test to accommodate a known-defective implementation
- Frame the correct fix as optional
- Use diff size or disruption as justification for the lesser fix

**Litmus test:** After delivering a fix, ask: *"If the user does not push back, does the product have a known structural defect that I could have eliminated?"* If yes, the fix is incomplete.

**Permitted exceptions:**
- The structural fix requires information the AI does not have
- The structural fix would change public API semantics and needs human approval — flag as blocker, not optional
- The user explicitly requests a minimal/tactical fix

#### 11.3.2 The Transferability Rule

The balancer must never know it is talking to a simulation. AI assistants must not:
- Add `sim/` includes to `include/balancer/` or `policies/`
- Add simulation-specific assumptions to `INode` or `ISchedulingPolicy`
- Create "shortcut" paths that only work with `SimulatedNode`

Any change that leaks simulation details into the core layer is a **Critical** violation.

### 11.4 Required AI Behaviors

#### Capability Disclosure

State honestly whether compilation is available. Do not claim compilation success without actual compilation.

#### Input Validation

Upon receiving guidelines, code, or documentation:
1. Verify completeness — scan for truncation signs
2. If truncation is suspected, state it immediately and do not proceed
3. List missing dependencies required for meaningful review or generation
4. Do not guess at missing content

#### Layer Verification Protocol

Before flagging any dependency as a violation:
1. Check if the included header has `BALANCER_META.layer`
2. If present, verify the inclusion is permitted by the layer hierarchy (§2)
3. If missing, ask the maintainer before claiming violation
4. Never assume a component's layer from its name alone

#### Deliverable Packaging Protocol

When output includes downloadable files:

Include a section titled `Modified Files (N)` listing only the files that were actually changed, with repo-relative paths.

Provide download links **only** for modified files. Do not attach unchanged files.

#### Inventory Count Maintenance

When files are added, removed, or restructured, update all hardcoded counts in README.md and documentation (header count, test count, FAT-P component count).

### 11.5 Evidence Requirements for Reviews

Every bug report must follow this structure:

```
**Bug:** [Brief description]
**Evidence:** [Verbatim code quote showing the issue]
**Counterexample:**
  - Input: [concrete values]
  - Actual: [what the code produces]
  - Expected: [what it should produce]
**Impact:** [Severity and consequences]
**Fix:** [Proposed solution]
```

Do not fabricate components. Do not claim false consensus. Do not template-match without codebase evidence.

---

## 12. Load-Bearing Elements

### 12.1 Do Not Weaken

| Element | Why It's Load-Bearing |
|---------|----------------------|
| **Authority hierarchy** | Development Guidelines > Style Guides > everything else |
| **Vocabulary ban** | Forces specificity in component documentation |
| **Mandatory honesty sections** | Prevents marketing-speak in docs |
| **Test namespace pattern** | `balancer::testing::componentns` — prevents ODR violations |
| **Evidence requirements** | Counterexamples, verbatim quotes — prevents hallucinated bug reports |
| **Review ≠ Rewrite** | Reviews produce findings + patches, never complete rewrites |
| **Band-Aid Rule** | Fix the root cause — prevents structural defects being framed as optional |
| **Transferability Rule** | No `sim/` in core — the balancer must be usable without the simulation |

### 12.2 Safe to Modify

| Element | Notes |
|---------|-------|
| Specific examples | Update freely |
| Benchmark environment specs | Hardware/compiler details change |
| Vocabulary ban entries | Can add; be cautious removing |
| Checklist items | Can add; be cautious removing |

### 12.3 The Test

Before changing any rule, ask: *"Does this make AI output more constrained or less constrained?"*

- More constrained → Probably fine
- Less constrained → Requires explicit justification

---

## Changelog

### v1.0 (March 2026)
- Initial release, adapted from Fat-P Library Development Guidelines v3.8
- Replaced `fat_p` namespace with `balancer`
- Replaced `FATP_META` with `BALANCER_META`
- Replaced six-layer FAT-P architecture with fatp-balancer six-layer architecture (Interface → Core → Policies → Learning → Sim → Testing)
- Replaced header-only-everywhere rule with header-only for Interface/Policies, compiled for Sim/App
- Added §1.2 The Transferability Boundary as first-class design principle
- Added §11.3.2 The Transferability Rule to AI Non-Goals
- Added FAT-P as explicit allowed dependency
- Removed HPC/scientific computing domain references; replaced with distributed systems domain
- Adapted repository structure to fatp-balancer layout
- Retained all governance mechanisms: Band-Aid Rule, Review ≠ Rewrite, Evidence Requirements, Vocabulary Ban, Layer Verification Protocol

---

*fatp-balancer Development Guidelines v1.0 — March 2026*
