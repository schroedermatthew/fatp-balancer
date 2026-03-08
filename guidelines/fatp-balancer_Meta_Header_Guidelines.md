---
doc_id: readme-balancer-meta-header-guidelines
doc_type: governance
status: active
audience: contributors
applies_to:
  - include/balancer/*.h
  - policies/*.h
  - sim/*.h
  - tests/*.cpp
  - bench/*.cpp
  - CMakeLists.txt
version: 1
last_updated: 2026-03-01
---

# fatp-balancer Meta Header Guidelines

## Purpose

`BALANCER_META` is a **machine-readable metadata block** embedded in C/C++ comments to make the fatp-balancer project indexable by automated tools and AI assistants. It supports:

- Component indexing (header → tests → benchmarks → docs)
- Faster navigation during refactors and reviews
- Hygiene visibility (macro surface, include cleanliness)
- CI linting for drift (moved files, missing links, layer violations, transferability violations)

`BALANCER_META` must be **comment-only** and must not change compilation behavior.

---

## Applicability

`BALANCER_META` is required for every **repository-authored code file**:

- `include/balancer/*.h`
- `policies/*.h`
- `sim/*.h`
- `tests/*.cpp`
- `bench/*.cpp`
- `CMakeLists.txt`
- `build.sh`, `build.ps1`, `build.bat`

Exclusions (no `BALANCER_META` required):
- YAML files (`.yml`, `.yaml`) including `.github/workflows/*`
- Generated directories (`build/`, `.vcpkg_installed/`)
- Non-code artifacts (`docs/` SVG outputs, `bench/results/`)

---

## Placement Rules

### Header Files (`.h`)

**Required layout:**

1. `#pragma once` on the **first line**
2. `BALANCER_META` block **immediately after `#pragma once`**
3. Doxygen `@file` header **after `BALANCER_META`**
4. `BALANCER_META` must appear before any `#include`

**Single source of truth:** Layer classification lives in `BALANCER_META.layer`. Do not duplicate it in Doxygen.

### Source Files (`.cpp`)

1. If there is an existing file header comment, keep it first
2. Place `BALANCER_META` immediately after the file header comment
3. Otherwise place it at the top before includes

### CMake Files

1. Keep `cmake_minimum_required(...)` first if present
2. Place `BALANCER_META` immediately after

### Script Files (`.sh`, `.ps1`, `.bat`)

1. Keep shebang line (`#!...`) first if present
2. Place `BALANCER_META` immediately after the shebang

### Blank Line Separation

Always leave **one empty line** after the closing of a `BALANCER_META` block before the next content.

---

## Format

`BALANCER_META` is a comment block beginning with the sentinel line `BALANCER_META:` followed by YAML.

### C/C++ Block Comment Form

```cpp
/*
BALANCER_META:
  meta_version: 1
  ...
*/
```

### Line-Comment Form (CMake, shell, PowerShell, batch)

```text
# BALANCER_META:
#   meta_version: 1
#   ...
```

### Comment Terminator Safety (Critical)

Inside a `/* ... */` block comment, `*/` terminates the comment even inside YAML strings.

**`BALANCER_META` content must never contain `*/` anywhere.**

This forbids glob patterns such as `**/*` or any value with `*` immediately followed by `/`. Use `docs_search` plain text fields instead.

### Formatting Constraints

- **Indentation:** 2 spaces
- **Encoding:** ASCII or UTF-8
- **Line length:** ≤ 100 columns
- **Key order:** follow the canonical key order below

---

## Schema v1

### Required Keys (All Files)

| Key | Type | Meaning |
|-----|------|---------|
| `meta_version` | int | Schema version. Must be `1`. |
| `component` | string or list | Canonical component name(s) |
| `file_role` | enum | Role in the repository (see enums) |
| `path` | string | Repo-relative path using forward slashes |
| `layer` | string | Architectural layer (see layer enum) |
| `summary` | string | One sentence describing the file's purpose |

### Strongly Recommended Keys

| Key | Type | Meaning |
|-----|------|---------|
| `namespace` | string or list | Primary namespaces (`balancer`, `balancer::detail`, …) |
| `api_stability` | enum | Stability classification |
| `related` | map | Links to docs/tests/benchmarks |
| `hygiene` | map | Machine-derived signals |

### `file_role` Enum

- `public_header`
- `internal_header`
- `source`
- `test`
- `benchmark`
- `doc_support`
- `build_script`
- `tooling`

### `layer` Enum

| Value | Used For |
|-------|---------|
| `Interface` | `include/balancer/` headers — INode, ISchedulingPolicy, Job, pure types |
| `Core` | Balancer coordinator, AdmissionControl, AgingEngine |
| `Policies` | `policies/*.h` — scheduling strategy implementations |
| `Learning` | CostModel, DegradationCurve, affinity matrix |
| `Sim` | `sim/*.h` — SimulatedNode, FaultInjector (never ships to production) |
| `Testing` | `tests/`, `bench/` files |

### `api_stability` Enum

- `in_work`
- `experimental`
- `candidate`
- `stable`

---

## Canonical Key Order

1. `meta_version`
2. `component`
3. `file_role`
4. `path`
5. `namespace`
6. `layer`
7. `summary`
8. `api_stability`
9. `related`
10. `hygiene`
11. `generated`

---

## `related` Section

Connects evidence and explanations to the file. All paths are repo-relative.

```yaml
related:
  docs:
    - docs/INode_User_Manual.md
  tests:
    - tests/test_INode_HeaderSelfContained.cpp
  benchmarks:
    - bench/benchmark_balancer.cpp
```

For in-work components, use plain-text search fields:

```yaml
related:
  docs_search: "CostModel"
  tests_search: "test_cost_model"
```

Rules:
- Prefer explicit paths when files exist
- Entries must point to files that exist in-tree
- Search fields must be plain text and must not contain `*/`

---

## `hygiene` Section

Machine-derived. Do not hand-edit counts.

```yaml
hygiene:
  pragma_once: true
  include_guard: false
  defines_total: 2
  defines_unprefixed: 0
  undefs_total: 0
  includes_windows_h: false
```

**Macro prefix requirement:** All macros in all fatp-balancer source files must use the `BALANCER_` prefix. Unprefixed macros must be `#undef`'d before end of file.

| Approach | Example | Compliant |
|----------|---------|-----------|
| Prefixed macro | `#define BALANCER_MAX_NODES 256` | Yes |
| Unprefixed + `#undef` | `#define LOCAL_MAX 256` ... `#undef LOCAL_MAX` | Yes |
| Unprefixed, no cleanup | `#define MAX 256` | No |

---

## Canonical Key Order and Layer — The Transferability Signal

The `layer` field is the machine-readable transferability signal. Any CI tool can verify that `Sim` layer headers are never included by `Interface`, `Core`, or `Policies` layer headers by scanning `BALANCER_META.layer` and `#include` directives together.

**Layer dependency allowed matrix:**

| Header layer | May include |
|-------------|-------------|
| Interface | Interface (same layer), std, FAT-P Foundation |
| Core | Interface, Core, std, FAT-P Foundation/Containers/Concurrency |
| Policies | Interface, Core, Policies, std, FAT-P |
| Learning | Interface, Core, Policies, Learning, std, FAT-P |
| Sim | All above, std, FAT-P |
| Testing | All above |

**Critical:** A `layer: Sim` header must never appear in the `related.headers` or actual `#include` directives of an `Interface`, `Core`, or `Policies` header.

---

## Examples

### Interface Layer Public Header

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
  summary: Transferable node contract — implement to connect to a real cluster.
  api_stability: in_work
  related:
    docs_search: "INode"
    tests:
      - tests/test_INode_HeaderSelfContained.cpp
  hygiene:
    pragma_once: true
    defines_total: 0
    defines_unprefixed: 0
    undefs_total: 0
    includes_windows_h: false
*/

/**
 * @file INode.h
 * @brief Transferable node contract.
 */
```

### Sim Layer Header

```cpp
#pragma once

/*
BALANCER_META:
  meta_version: 1
  component: SimulatedNode
  file_role: public_header
  path: sim/SimulatedNode.h
  namespace: balancer::sim
  layer: Sim
  summary: INode implementation backed by FAT-P ThreadPool for simulation.
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
```

### Test File

```cpp
/**
 * @file test_cost_model.cpp
 * @brief Unit tests for CostModel.h
 */

/*
BALANCER_META:
  meta_version: 1
  component: CostModel
  file_role: test
  path: tests/test_cost_model.cpp
  namespace: balancer::testing::costmodel
  layer: Testing
  summary: Unit tests for CostModel — EMA convergence, cold start, serialization.
  api_stability: in_work
  related:
    headers:
      - include/balancer/CostModel.h
*/
```

### Benchmark File

```cpp
/*
BALANCER_META:
  meta_version: 1
  component: Balancer
  file_role: benchmark
  path: bench/benchmark_balancer.cpp
  namespace: balancer::bench
  layer: Testing
  summary: End-to-end throughput and latency benchmarks for fatp-balancer.
  api_stability: in_work
*/
```

---

## CI Enforcement

CI meta lint must check:

1. **Presence** — Required files have a `BALANCER_META` block
2. **Parse** — YAML parses; `meta_version == 1`; required keys present
3. **Consistency** — `path` matches file location; `file_role` matches extension and directory
4. **Layer** — No `Sim` layer header included by `Interface`, `Core`, or `Policies` headers
5. **No drift** — `related.*` targets exist when present

---

## Update Rules

### When You Must Update `BALANCER_META`

- File moved or renamed → update `path`
- Component changed → update `component`
- Related files moved → update `related.*`
- Layer changed → update `layer` and verify CI layer check still passes

### What Must Not Be Edited by Hand

- `hygiene` counts and derived flags
- `generated` fields

---

## Common Mistakes

- Placing `BALANCER_META` after includes
- Embedding `*/` inside `BALANCER_META` values (terminates the comment, breaks compilation)
- Omitting the blank line after the `BALANCER_META` block
- Using `layer: Sim` for a header in `include/balancer/` (that header is transferable; it must not be Sim)
- Assigning `layer: Interface` to a header that includes `sim/` (it is lying about being transferable)

---

## Changelog

### v1 (March 2026)
- Initial release, adapted from FAT-P Meta Header Guidelines v2
- Replaced `FATP_META` sentinel and prefix with `BALANCER_META`
- Replaced FAT-P six-layer architecture (Foundation/Containers/Concurrency/Domain/Integration/Testing) with fatp-balancer layer system (Interface/Core/Policies/Learning/Sim/Testing)
- Added transferability signal documentation: layer enum combined with include scanning enforces the INode boundary in CI
- Removed FAT-P-specific path conventions; replaced with fatp-balancer paths

---

*fatp-balancer Meta Header Guidelines v1 — March 2026*
