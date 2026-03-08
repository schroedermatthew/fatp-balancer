---
doc_id: GOV-BALANCER-HYGIENE-001
doc_type: governance
title: "fatp-balancer Systemic Hygiene Policy"
topics: ["header composability", "ODR", "namespace collision", "include order", "macro hygiene", "self-contained headers", "transferability boundary"]
cxx_standard: "C++20"
last_verified: "2026-03-01"
audience: ["C++ developers", "library maintainers", "AI assistants"]
status: "active"
---

# fatp-balancer Systemic Hygiene Policy

**Status:** Active
**Version:** 1.0
**Applies to:** All headers in `include/balancer/`, `policies/`, and `sim/`
**Baseline:** C++20, FAT-P as sole external dependency
**Authority:** Subordinate to the *fatp-balancer Development Guidelines*. In case of conflict, the Development Guidelines take precedence.

This policy keeps fatp-balancer **composable, correct, and transferable** as the codebase grows. It defines hard rules that prevent the class of problems that typically kill header-only libraries at scale, plus one rule that is unique to this project: the transferability boundary between `include/balancer/` and `sim/`.

---

## 1. Goals

### 1.1 Must-Have Outcomes

A fatp-balancer consumer must be able to:

1. Include **any subset** of `include/balancer/` and `policies/` headers in the same translation unit.
2. Include them in **any order**.
3. Build with common warning settings (`-Wall -Wextra -Wpedantic`) without redefinition errors, ambiguous overloads, macro redefinition warnings, or ODR violations.
4. Take `include/balancer/` and `policies/` to a production deployment **without taking `sim/`**.

### 1.2 Scope

This policy covers:
- Public headers (`include/balancer/*.h`)
- Policy headers (`policies/*.h`)
- Simulation headers (`sim/*.h`) — same composability rules, plus layer isolation rules
- Tests and benchmarks — where "temporary" hacks most often leak into core patterns

### 1.3 Non-Goals

- Enforcing naming aesthetics — see Development Guidelines §5.2
- Enforcing a single layout for documentation files — see Teaching Documents Style Guide

---

## 2. Definitions

### 2.1 Public Header

A header is **public** if it is intended to be included directly by consumers. In fatp-balancer:
- All headers in `include/balancer/` are public
- All headers in `policies/` are public
- Headers in `sim/` are **not public** — they are simulation infrastructure

Public headers must be: self-contained, include-order independent, warning-clean, stable in naming.

### 2.2 Root Namespace

"Root namespace" means `namespace balancer { ... }` without module scoping.

Root namespace symbols are effectively global to the entire library. They must be treated like API surface.

### 2.3 Namespace Flattening

Exporting nested names into the root namespace:

```cpp
namespace balancer {
  using detail::NodeStateImpl;  // BAD: flattening
}
```

Flattening is the primary cause of cross-module collisions.

### 2.4 Transferability Regression

Any change that:
- makes a `sim/` header included by `include/balancer/` or `policies/`, or
- makes the balancer's behavior depend on `SimulatedNode` internals, or
- makes `INode` non-implementable without `sim/` types

is a **transferability regression** and is treated as a P0 defect.

### 2.5 Composability Regression

Any change that makes two headers no longer includable together in any order is a **composability regression** and is treated as a P0 defect.

---

## 3. Hard Rules (MUST)

### Rule A — No Root Namespace Flattening in Headers

**Public headers MUST NOT inject `using ...;` declarations into `namespace balancer` at namespace scope.**

#### A.1 Forbidden

```cpp
// LoadMetrics.h
namespace balancer {
namespace detail { struct BandMetrics {}; }
using detail::BandMetrics;  // BAD: pollutes balancer root
}
```

#### A.2 Allowed

```cpp
namespace balancer::detail { struct BandMetrics {}; }  // GOOD
```

#### A.3 Allowed Convenience Pattern

Provide an opt-in local macro that users expand in their `.cpp`:

```cpp
// In LoadMetrics.h
#define USING_BALANCER_METRICS()           \
  using balancer::LoadMetrics;             \
  using balancer::ClusterMetrics;          \
  using balancer::BandMetrics

// In user code:
USING_BALANCER_METRICS();  // GOOD: local scope import
```

---

### Rule B — Root Namespace Symbols Are Reserved for Core Contracts

Only the primary public contracts may live in `balancer` root:
- `balancer::INode`
- `balancer::ISchedulingPolicy`
- `balancer::Job`, `balancer::JobHandle`, `balancer::Priority`
- `balancer::Balancer`
- `balancer::Expected` (re-export from FAT-P)

**Everything else lives in a sub-namespace:**
- `balancer::detail` (internal helpers)
- `balancer::sim` (simulation layer)
- `balancer::bench` (benchmark infrastructure)

---

### Rule C — Every Module Owns Its Names

Non-core components must live in a sub-namespace:

**Good:**

```cpp
namespace balancer::detail {
  struct DegradationBucket { float slowdown = 1.0f; };
}
```

**Bad:**

```cpp
namespace balancer {
  struct DegradationBucket { ... };  // Pollutes root with implementation detail
}
```

---

### Rule D — Headers Must Be Include-Order Independent

A public header must not compile only when included after some other header.

**Common landmine — two headers define the same name in `balancer` root:**

```cpp
// AdmissionControl.h
namespace balancer { enum class RejectionReason { ... }; }

// AgingEngine.h
namespace balancer { enum class RejectionReason { ... }; }  // BAD: redefinition
```

**Required fix:** One canonical definition in one header. Other headers include that header.

---

### Rule E — No Duplicate Entity Definitions Across Headers

A symbol may only be defined in one header file if two headers can be included together.

Even `inline` functions cannot be defined twice in the same translation unit.

**Required fix:** Shared helpers go in a single shared header, included by all modules that need them.

---

### Rule F — The Transferability Rule (P0)

**`include/balancer/` and `policies/` headers MUST NOT include any `sim/` header — directly or transitively.**

```cpp
// FORBIDDEN in include/balancer/Balancer.h
#include "sim/SimulatedNode.h"  // P0 violation

// FORBIDDEN in policies/LeastLoaded.h
#include "sim/SimulatedCluster.h"  // P0 violation
```

`INode` is the contract. `SimulatedNode` is one implementation. Headers that know about `SimulatedNode` cannot be taken to production without the simulation layer.

**CI enforces this via the transferability gate job.** Any PR that introduces a `sim/` include into `include/balancer/` or `policies/` will fail CI.

---

### Rule G — Macro Hygiene (Single Source, BALANCER_ Prefix)

All configuration macros must be:
- Prefixed `BALANCER_`
- Defined in **one** place
- Guarded with `#ifndef` to prevent redefinition
- Never redefined in multiple headers

**Forbidden:**

```cpp
// Job.h
#define MAX_PRIORITY_LEVELS 5

// LoadMetrics.h
#define MAX_PRIORITY_LEVELS 5  // BAD: macro redefinition
```

**Required:**

```cpp
// BalancerConfig.h — single source
#ifndef BALANCER_MAX_PRIORITY_LEVELS
  #define BALANCER_MAX_PRIORITY_LEVELS 5
#endif
```

---

### Rule H — Public Headers Must Be Self-Contained

A public header must compile when included alone:

```cpp
#include "balancer/INode.h"
int main() {}
```

This implies:
- It includes all required standard headers
- It includes all required FAT-P headers
- It does not rely on transitive includes

---

### Rule I — No `using namespace` in Headers

No public header may contain:

```cpp
using namespace balancer;
using namespace std;
using namespace fat_p;
```

No exceptions.

---

### Rule J — Explicit Ownership of `detail`

All internal-only helpers must live in:
- `balancer::detail` (global internal helpers), or
- `balancer::<module>::detail` (module-private helpers)

No bare `static` functions at global scope in public headers.

---

### Rule K — No Backward Compatibility Shims for Hygiene Fixes

When a systemic hygiene issue is fixed:
- Do not leave alias macros
- Do not leave deprecated typedefs
- Do not ship old names "for compatibility"

Fix the problem fully and update all call sites.

---

## 4. Preferred Patterns (SHOULD)

### 4.1 Prefer Nested Module Namespaces

Instead of `BalancerNodeState`, use `balancer::detail::NodeState`.

### 4.2 Prefer Fully Qualified Internal Calls

Inside headers:

```cpp
return ::balancer::detail::computeAgedPriority(current, elapsed);
```

Not relying on unqualified lookup.

### 4.3 Prefer `inline constexpr` for Constants

```cpp
inline constexpr size_t kPriorityLevels = 5;
```

Avoid non-inline global variables in headers.

### 4.4 Snapshot Before Callback in Thread-Safe Code

If callbacks are invoked outside locks, snapshot the callback list under lock first.

---

## 5. Required Examples

### Example 1 — Transferability Violation

**Bad:**

```cpp
// include/balancer/Balancer.h
#include "sim/SimulatedNode.h"  // VIOLATION: core includes sim
```

**Good:**

```cpp
// include/balancer/Balancer.h
#include "balancer/INode.h"  // CORRECT: depends only on the contract
```

### Example 2 — Namespace Collision

**Bad:**

```cpp
// AdmissionControl.h
namespace balancer { struct Error { std::string message; }; }

// AgingEngine.h
namespace balancer { struct Error { int code; }; }  // COLLISION
```

**Good:**

```cpp
namespace balancer::admission { struct Error { std::string message; }; }
namespace balancer::aging     { struct Error { int code; }; }
```

### Example 3 — Duplicate Helper

**Bad:**

```cpp
// CostModel.h:   inline float clampMultiplier(float v) { ... }
// Balancer.h:    inline float clampMultiplier(float v) { ... }  // ODR violation
```

**Good:**

```cpp
// BalancerMath.h: inline float clampMultiplier(float v) { ... }  // single definition
// Both CostModel.h and Balancer.h include BalancerMath.h
```

---

## 6. Enforcement (CI / Build Gates)

### 6.1 Include-All Compile Test (MUST)

Maintain a compile-only TU that includes all public headers:

```cpp
// tests/test_include_all_balancer_headers.cpp
#include "balancer/AdmissionControl.h"
#include "balancer/AgingEngine.h"
#include "balancer/Balancer.h"
#include "balancer/BalancerConfig.h"
#include "balancer/ClusterView.h"
#include "balancer/CostModel.h"
#include "balancer/INode.h"
#include "balancer/ISchedulingPolicy.h"
#include "balancer/Job.h"
#include "balancer/LoadMetrics.h"
#include "policies/Composite.h"
#include "policies/EarliestDeadlineFirst.h"
#include "policies/LeastLoaded.h"
#include "policies/RoundRobin.h"
#include "policies/ShortestJobFirst.h"
#include "policies/WeightedCapacity.h"
#include "policies/WorkStealing.h"
int main() { return 0; }
```

### 6.2 Header Self-Contained Tests (MUST)

For each public header `X.h`, provide `tests/test_X_HeaderSelfContained.cpp`. See Test Suite Style Guide.

### 6.3 Transferability Gate (MUST)

CI scan that verifies no `sim/` paths appear in `#include` directives inside `include/balancer/` or `policies/`. See CI Workflow Style Guide §11.2.

### 6.4 Warning Cleanliness

Public headers must be warning-clean under:
- GCC/Clang: `-Wall -Wextra -Wpedantic`
- MSVC: `/W4`

---

## 7. Review Checklist (PR Gate)

### Namespace and Symbol Safety

- [ ] No new `using ...;` exported into `namespace balancer` at namespace scope
- [ ] No new generic root names that could collide across modules
- [ ] Sub-namespace used for module-owned symbols
- [ ] No `using namespace` in headers

### Include and Macro Hygiene

- [ ] Header compiles when included alone
- [ ] No macro redefinitions (central config used for shared constants)
- [ ] New macros are prefixed `BALANCER_` and guarded with `#ifndef`

### ODR and Duplication

- [ ] No helper defined in multiple headers
- [ ] Shared helper moved to a single shared header if used in >1 module

### Transferability

- [ ] No `sim/` includes in `include/balancer/` or `policies/`
- [ ] `INode` interface contains no `SimulatedNode`-specific types

### CI Gates

- [ ] Include-all TU compiles
- [ ] Header self-contained TU compiles
- [ ] Transferability gate passes

---

## 8. Migration Guidance

When the include-all test breaks due to a collision:

1. Identify the name collision
2. Decide ownership — does it belong in `balancer` root, or a sub-namespace?
3. Remove any flattening exports
4. Rename conflicting symbols to module-owned names
5. Update all call sites, tests, benchmarks, docs
6. Add a regression test

When the transferability gate breaks:

1. Identify which `sim/` header was included into core or policies
2. Determine why the dependency exists
3. If the dependency is needed: extract the common type into `include/balancer/` and have both `sim/` and `include/balancer/` use it
4. If the dependency is not needed: remove the include
5. Verify the gate passes

---

## 9. Summary: The Two Laws

> **Law 1:** If two headers cannot be included together in any order, the library is broken.

> **Law 2:** If `include/balancer/` or `policies/` depends on `sim/`, the balancer cannot be deployed without the simulation layer — which breaks the entire architectural premise.

Both laws have the same consequence: a P0 defect that blocks the PR.

---

## Changelog

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2026-03-01 | Initial release, adapted from Fat-P Systemic Hygiene Policy v1.0; added Rule F (Transferability Rule) and Law 2; replaced `fat_p` namespace with `balancer`; replaced FAT-P layer names with fatp-balancer layer names |

---

*fatp-balancer Systemic Hygiene Policy v1.0 — March 2026*
