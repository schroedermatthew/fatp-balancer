# fatp-balancer CI Workflow Style Guide

**Status:** Active
**Applies to:** All `.github/workflows/*.yml` files
**Authority:** Subordinate to *fatp-balancer Development Guidelines*
**Version:** 1.0 (March 2026)

---

## 1. Purpose

This guide standardizes GitHub Actions CI workflows for fatp-balancer. Consistent workflows ensure:

- Uniform quality gates across build configurations
- Predictable CI behavior
- Enforcement of C++20 minimum
- Sanitizer coverage for the concurrency-heavy code paths
- Verification that core headers never include `sim/` headers (transferability gate)

---

## 2. Directory Structure

```
.github/workflows/          # CI workflow files (*.yml)
include/balancer/           # Core headers
policies/                   # Policy headers
sim/                        # Simulation headers and sources
tests/                      # Test files (test_*.cpp)
bench/                      # Benchmark files (benchmark_*.cpp)
```

**Critical path conventions for workflows:**

| Target | Path |
|--------|------|
| Core headers | `include/balancer/*.h` |
| Policy headers | `policies/*.h` |
| Test sources | `tests/test_*.cpp` |
| Bench sources | `bench/benchmark_*.cpp` |
| Workflow file itself | `.github/workflows/ci.yml` |

---

## 3. Workflow Trigger Policy

### 3.1 Main CI Workflow (`ci.yml`)

Triggers on push, pull_request, and manual dispatch.

```yaml
on:
  workflow_dispatch:
  push:
    paths:
      - 'include/balancer/**'
      - 'policies/**'
      - 'sim/**'
      - 'tests/**'
      - 'bench/**'
      - 'CMakeLists.txt'
      - '.github/workflows/ci.yml'
  pull_request:
    paths:
      - 'include/balancer/**'
      - 'policies/**'
      - 'sim/**'
      - 'tests/**'
      - 'bench/**'
      - 'CMakeLists.txt'
      - '.github/workflows/ci.yml'
```

The `push` and `pull_request` paths must be identical.

### 3.2 Benchmark Workflow (`benchmarks.yml`)

Benchmark workflow is a **separate YAML file** triggered only by manual dispatch. Benchmarks are never embedded in the main CI workflow.

```yaml
on:
  workflow_dispatch:
    inputs:
      batches:
        description: 'Measured batches per benchmark'
        required: false
        default: '20'
        type: string
      target_work:
        description: 'Target ops per batch'
        required: false
        default: '100000'
        type: string
```

---

## 4. C++ Standard Policy

**C++20 is the minimum. C++23 is tested for forward compatibility.**

| Standard | Status | Compiler Matrix |
|----------|--------|-----------------|
| C++20 | Primary | GCC-12, GCC-13, Clang-16, MSVC |
| C++23 | Forward compat | GCC-14, Clang-17, MSVC (`/std:c++latest`) |

C++17 is not compatible with fatp-balancer or FAT-P. It is not tested in CI.

---

## 5. Required Jobs

Every CI workflow MUST include these jobs:

| Job | Purpose | Required |
|-----|---------|----------|
| `linux-gcc` | GCC 12/13/14 (C++20/C++23) build + tests | Yes |
| `linux-clang` | Clang 16/17 (C++20/C++23) build + tests | Yes |
| `windows-msvc` | MSVC (C++20/C++23) build + tests | Yes |
| `sanitizer-asan` | AddressSanitizer | Yes |
| `sanitizer-ubsan` | UndefinedBehaviorSanitizer | Yes |
| `sanitizer-tsan` | ThreadSanitizer | Yes — this is a concurrency-heavy project |
| `header-check` | Verify headers compile standalone | Yes |
| `transferability-gate` | Verify no `sim/` includes in core or policies | Yes |
| `strict-warnings` | Extended warning flags | Yes |
| `ci-success` | Gate job aggregating all results | Yes |

Benchmark jobs belong in `benchmarks.yml` only.

---

## 6. Compiler Version Matrix

### 6.1 GCC Versions

| Version | C++ Standard | Runner | Role |
|---------|--------------|--------|------|
| GCC 12 | C++20 | ubuntu-24.04 | Oldest supported |
| GCC 13 | C++20 | ubuntu-24.04 | Primary |
| GCC 14 | C++23 | ubuntu-24.04 | Forward compat |

### 6.2 Clang Versions

| Version | C++ Standard | Runner | Role |
|---------|--------------|--------|------|
| Clang 16 | C++20 | ubuntu-22.04 | Primary |
| Clang 17 | C++23 | ubuntu-22.04 | Forward compat |

### 6.3 MSVC Standards

| Standard | Flag | Role |
|----------|------|------|
| C++20 | `/std:c++20` | Primary |
| C++23 | `/std:c++latest` | Forward compat |

---

## 7. FAT-P Dependency Setup

All build jobs must make FAT-P headers available. The canonical approach:

```yaml
- name: Checkout FAT-P
  uses: actions/checkout@v4
  with:
    repository: schroedermatthew/FatP
    path: FatP

- name: Build tests
  run: |
    g++-${{ matrix.version }} -std=c++${{ matrix.std }} \
      -Wall -Wextra -Wpedantic -Werror \
      -O2 -DNDEBUG \
      -DENABLE_TEST_APPLICATION \
      -I./include \
      -I./FatP/include/fat_p \
      ${{ env.TEST_SRC }} -o test_bin
```

For CMake-based builds:

```yaml
- name: Configure
  run: |
    cmake -B build \
      -DFATP_INCLUDE_DIR=./FatP/include \
      -DBALANCER_BUILD_TESTS=ON \
      -DCMAKE_BUILD_TYPE=Release
```

---

## 8. MSVC-Specific Requirements

### 8.1 Required Libraries

MSVC builds must link `advapi32.lib` for `FatPTest.h` and `FatPBenchmarkRunner.h`:

```yaml
cl ... /Fe:test_bin.exe /link advapi32.lib
```

### 8.2 Warning Suppressions

| Warning | Flag | Reason |
|---------|------|--------|
| C4324 | `/wd4324` | Structure padded due to alignment (intentional for cache-line alignment from FAT-P) |

### 8.3 MSVC Build Command Template

```yaml
- name: Build tests
  run: |
    $stdFlag = if (${{ matrix.std }} -eq 23) { "/std:c++latest" } else { "/std:c++${{ matrix.std }}" }
    cl $stdFlag /W4 /WX /wd4324 /EHsc /permissive- /O2 /DNDEBUG /DENABLE_TEST_APPLICATION `
      /I.\include /I.\FatP\include\fat_p `
      tests\test_balancer.cpp /Fe:test_bin.exe /link advapi32.lib
```

### 8.4 MSVC Path Format

Windows uses backslashes in YAML:

```yaml
/I.\include /I.\FatP\include\fat_p tests\test_balancer.cpp
```

---

## 9. Linux Build Requirements

### 9.1 GCC Build Template

```yaml
- name: Build tests
  run: |
    g++-${{ matrix.version }} -std=c++${{ matrix.std }} \
      -Wall -Wextra -Wpedantic -Werror \
      -O2 -DNDEBUG \
      -DENABLE_TEST_APPLICATION \
      -I./include \
      -I./FatP/include/fat_p \
      ${{ env.TEST_SRC }} -o test_bin
```

### 9.2 Clang Build Template

```yaml
- name: Build tests
  run: |
    clang++-${{ matrix.version }} -std=c++${{ matrix.std }} \
      -Wall -Wextra -Wpedantic -Werror \
      -Wno-gnu-zero-variadic-macro-arguments \
      -O2 -DNDEBUG \
      -DENABLE_TEST_APPLICATION \
      -I./include \
      -I./FatP/include/fat_p \
      ${{ env.TEST_SRC }} -o test_bin
```

---

## 10. Sanitizer Jobs

All sanitizers run at C++20. This project has substantial threading; TSan is mandatory.

### 10.1 AddressSanitizer

```yaml
sanitizer-asan:
  name: AddressSanitizer
  runs-on: ubuntu-24.04
  steps:
    - uses: actions/checkout@v4
    - name: Checkout FAT-P
      uses: actions/checkout@v4
      with:
        repository: schroedermatthew/FatP
        path: FatP
    - name: Build with ASan
      run: |
        g++-13 -std=c++20 -Wall -Wextra -g -O1 \
          -fsanitize=address -fno-omit-frame-pointer \
          -DENABLE_TEST_APPLICATION \
          -I./include -I./FatP/include/fat_p \
          ${{ env.TEST_SRC }} -o test_bin
    - name: Run with ASan
      env:
        ASAN_OPTIONS: detect_leaks=1:abort_on_error=1
      run: ./test_bin
```

### 10.2 UndefinedBehaviorSanitizer

```yaml
sanitizer-ubsan:
  name: UndefinedBehaviorSanitizer
  runs-on: ubuntu-24.04
  steps:
    - uses: actions/checkout@v4
    - name: Checkout FAT-P
      uses: actions/checkout@v4
      with:
        repository: schroedermatthew/FatP
        path: FatP
    - name: Build with UBSan
      run: |
        g++-13 -std=c++20 -Wall -Wextra -g -O1 \
          -fsanitize=undefined -fno-omit-frame-pointer \
          -DENABLE_TEST_APPLICATION \
          -I./include -I./FatP/include/fat_p \
          ${{ env.TEST_SRC }} -o test_bin
    - name: Run with UBSan
      env:
        UBSAN_OPTIONS: print_stacktrace=1:halt_on_error=1
      run: ./test_bin
```

### 10.3 ThreadSanitizer (Mandatory)

```yaml
sanitizer-tsan:
  name: ThreadSanitizer
  runs-on: ubuntu-24.04
  steps:
    - uses: actions/checkout@v4
    - name: Checkout FAT-P
      uses: actions/checkout@v4
      with:
        repository: schroedermatthew/FatP
        path: FatP
    - name: Build with TSan
      run: |
        g++-13 -std=c++20 -Wall -Wextra -g -O1 \
          -fsanitize=thread -fno-omit-frame-pointer \
          -DENABLE_TEST_APPLICATION \
          -I./include -I./FatP/include/fat_p \
          ${{ env.TEST_SRC }} -o test_bin
    - name: Run with TSan
      env:
        TSAN_OPTIONS: halt_on_error=1
      run: ./test_bin
```

**Rationale for mandatory TSan:** fatp-balancer uses ThreadPool, LockFreeQueue, LockFreeRingBuffer, WorkQueue, and CostModel — all shared across threads. TSan is the primary tool for catching data races that unit tests may not expose.

---

## 11. Header Check and Transferability Gate

### 11.1 Header Self-Contained Check

Verifies every public header compiles standalone:

```yaml
header-check:
  name: Header Self-Contained Check
  runs-on: ubuntu-24.04
  steps:
    - uses: actions/checkout@v4
    - name: Checkout FAT-P
      uses: actions/checkout@v4
      with:
        repository: schroedermatthew/FatP
        path: FatP
    - name: Install GCC
      run: sudo apt-get install -y g++-13
    - name: Check headers compile standalone
      run: |
        for header in include/balancer/*.h policies/*.h; do
          echo "Checking $header..."
          echo "#include \"$header\"" > /tmp/check.cpp
          echo "int main() { return 0; }" >> /tmp/check.cpp
          g++-13 -std=c++20 -I./include -I./FatP/include/fat_p /tmp/check.cpp -o /dev/null
        done
```

### 11.2 Transferability Gate (Critical)

Verifies that no `include/balancer/` or `policies/` header includes anything from `sim/`:

```yaml
transferability-gate:
  name: Transferability Gate
  runs-on: ubuntu-24.04
  steps:
    - uses: actions/checkout@v4
    - name: Verify no sim/ includes in core or policies
      run: |
        violations=$(grep -rn '#include.*sim/' include/balancer/ policies/ 2>/dev/null || true)
        if [ -n "$violations" ]; then
          echo "TRANSFERABILITY VIOLATION: sim/ included in core or policies:"
          echo "$violations"
          exit 1
        fi
        echo "Transferability gate passed."
```

**This job is a hard gate. CI must not pass if it fails.** A `sim/` include in `include/balancer/` or `policies/` means the core balancer cannot be taken to production without the simulation layer — which defeats the entire architectural premise.

---

## 12. Strict Warnings Job

```yaml
strict-warnings:
  name: Strict Warnings (GCC-13, C++20)
  runs-on: ubuntu-24.04
  steps:
    - uses: actions/checkout@v4
    - name: Checkout FAT-P
      uses: actions/checkout@v4
      with:
        repository: schroedermatthew/FatP
        path: FatP
    - name: Install GCC
      run: sudo apt-get install -y g++-13
    - name: Build with strict warnings
      run: |
        g++-13 -std=c++20 \
          -Wall -Wextra -Wpedantic -Werror \
          -Wshadow -Wno-unused-parameter \
          -O2 -DNDEBUG \
          -DENABLE_TEST_APPLICATION \
          -I./include -I./FatP/include/fat_p \
          ${{ env.TEST_SRC }} -o test_bin
```

---

## 13. Gate Job

The `ci-success` gate job aggregates all required jobs. CI is not considered passing unless all required jobs succeed.

```yaml
ci-success:
  name: CI Success
  needs:
    - linux-gcc
    - linux-clang
    - windows-msvc
    - sanitizer-asan
    - sanitizer-ubsan
    - sanitizer-tsan
    - header-check
    - transferability-gate
    - strict-warnings
  runs-on: ubuntu-24.04
  if: always()
  steps:
    - name: Check all jobs passed
      run: |
        if [[ "${{ contains(needs.*.result, 'failure') }}" == "true" ]]; then
          echo "One or more required jobs failed."
          exit 1
        fi
        echo "All required jobs passed."
```

---

## 14. Complete CI Workflow Example

```yaml
name: fatp-balancer CI

on:
  workflow_dispatch:
  push:
    paths:
      - 'include/balancer/**'
      - 'policies/**'
      - 'sim/**'
      - 'tests/**'
      - 'CMakeLists.txt'
      - '.github/workflows/ci.yml'
  pull_request:
    paths:
      - 'include/balancer/**'
      - 'policies/**'
      - 'sim/**'
      - 'tests/**'
      - 'CMakeLists.txt'
      - '.github/workflows/ci.yml'

env:
  TEST_SRC: tests/test_balancer.cpp

jobs:
  linux-gcc:
    name: Linux GCC ${{ matrix.version }} C++${{ matrix.std }}
    runs-on: ubuntu-24.04
    strategy:
      matrix:
        include:
          - { version: 12, std: 20 }
          - { version: 13, std: 20 }
          - { version: 13, std: 20, config: Debug }
          - { version: 14, std: 23 }
    steps:
      - uses: actions/checkout@v4
      - name: Checkout FAT-P
        uses: actions/checkout@v4
        with:
          repository: schroedermatthew/FatP
          path: FatP
      - name: Install GCC
        run: sudo apt-get install -y g++-${{ matrix.version }}
      - name: Build
        run: |
          g++-${{ matrix.version }} -std=c++${{ matrix.std }} \
            -Wall -Wextra -Wpedantic -Werror -O2 -DNDEBUG \
            -DENABLE_TEST_APPLICATION \
            -I./include -I./FatP/include/fat_p \
            ${{ env.TEST_SRC }} -o test_bin
      - name: Test
        run: ./test_bin

  linux-clang:
    name: Linux Clang ${{ matrix.version }} C++${{ matrix.std }}
    runs-on: ubuntu-22.04
    strategy:
      matrix:
        include:
          - { version: 16, std: 20 }
          - { version: 17, std: 23 }
    steps:
      - uses: actions/checkout@v4
      - name: Checkout FAT-P
        uses: actions/checkout@v4
        with:
          repository: schroedermatthew/FatP
          path: FatP
      - name: Install Clang
        run: sudo apt-get install -y clang-${{ matrix.version }}
      - name: Build
        run: |
          clang++-${{ matrix.version }} -std=c++${{ matrix.std }} \
            -Wall -Wextra -Wpedantic -Werror \
            -Wno-gnu-zero-variadic-macro-arguments \
            -O2 -DNDEBUG \
            -DENABLE_TEST_APPLICATION \
            -I./include -I./FatP/include/fat_p \
            ${{ env.TEST_SRC }} -o test_bin
      - name: Test
        run: ./test_bin

  windows-msvc:
    name: Windows MSVC C++${{ matrix.std }}
    runs-on: windows-latest
    strategy:
      matrix:
        std: [20, 23]
    steps:
      - uses: actions/checkout@v4
      - name: Checkout FAT-P
        uses: actions/checkout@v4
        with:
          repository: schroedermatthew/FatP
          path: FatP
      - uses: microsoft/setup-msbuild@v1
      - name: Build
        run: |
          $stdFlag = if (${{ matrix.std }} -eq 23) { "/std:c++latest" } else { "/std:c++${{ matrix.std }}" }
          cl $stdFlag /W4 /WX /wd4324 /EHsc /permissive- /O2 /DNDEBUG /DENABLE_TEST_APPLICATION `
            /I.\include /I.\FatP\include\fat_p `
            tests\test_balancer.cpp /Fe:test_bin.exe /link advapi32.lib
        shell: pwsh
      - name: Test
        run: .\test_bin.exe

  # sanitizer jobs omitted here for brevity — see Sections 10.1-10.3
  # header-check, transferability-gate, strict-warnings — see Sections 11-12

  ci-success:
    name: CI Success
    needs: [linux-gcc, linux-clang, windows-msvc, sanitizer-asan, sanitizer-ubsan, sanitizer-tsan, header-check, transferability-gate, strict-warnings]
    runs-on: ubuntu-24.04
    if: always()
    steps:
      - name: Check all jobs passed
        run: |
          if [[ "${{ contains(needs.*.result, 'failure') }}" == "true" ]]; then
            exit 1
          fi
```

---

## 15. Checklist Before Submitting a Workflow

- [ ] Trigger paths identical for `push` and `pull_request`
- [ ] FAT-P checkout step present in all build jobs
- [ ] `transferability-gate` job present and in `ci-success.needs`
- [ ] `sanitizer-tsan` present and in `ci-success.needs`
- [ ] All jobs included in `ci-success.needs`
- [ ] MSVC builds link `advapi32.lib`
- [ ] Clang builds include `-Wno-gnu-zero-variadic-macro-arguments`
- [ ] No benchmark jobs in `ci.yml`

---

*fatp-balancer CI Workflow Style Guide v1.0 — March 2026*
*Adapted from Fat-P CI Workflow Style Guide v3.0*
