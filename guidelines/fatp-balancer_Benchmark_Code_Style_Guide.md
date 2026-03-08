# fatp-balancer Benchmark Code Style Guide

**Status:** Active
**Applies to:** All benchmark translation units (`bench/benchmark_*.cpp`)
**Authority:** Subordinate to *fatp-balancer Development Guidelines*
**Version:** 1.0 (March 2026)

---

## Purpose

This guide defines how to write benchmarks for fatp-balancer. Benchmarks establish the performance story of the balancer under realistic load conditions:

- Job throughput at various priority mixes
- Scheduling latency per policy
- Learning model overhead (predict/update cycle)
- Admission control cost under load
- Fault recovery time

Benchmarks must be reproducible, honest about methodology, and use the same `FatPBenchmarkRunner.h` infrastructure as FAT-P.

Reference implementations: `FatPBenchmarkRunner.h`, `FatPBenchmarkHeader.h`

---

## File Structure

### Naming

```
bench/benchmark_balancer.cpp           # Main balancer throughput benchmark
bench/benchmark_policies.cpp           # Per-policy comparison
bench/benchmark_cost_model.cpp         # Learning model overhead
bench/benchmark_admission.cpp          # Admission control cost
```

### Required Sections

```cpp
// 1. File header with build instructions
// 2. BALANCER_META block
// 3. Includes
// 4. Benchmark configuration (env vars + defaults)
// 5. Platform configuration
// 6. CPU frequency monitoring
// 7. Benchmark scope (priority/affinity)
// 8. Startup header printing
// 9. Timer (calibration)
// 10. Statistics
// 11. Data generation
// 12. Correctness guardrails (checks OUTSIDE timed regions)
// 13. Benchmark cases
// 14. Output formatting (machine-readable export)
// 15. Main
```

---

## Benchmark Configuration

All fatp-balancer benchmarks use the same environment variables as FAT-P benchmarks. Do not invent per-benchmark config names.

### Canonical Environment Variables

| Variable | Meaning | Default |
|----------|---------|---------|
| `BALANCER_BENCH_WARMUP_RUNS` | Warmup batches (not reported) | `3` |
| `BALANCER_BENCH_BATCHES` | Measured batches | Windows: `15`, others: `50` |
| `BALANCER_BENCH_SEED` | RNG seed for job generation | `12345` |
| `BALANCER_BENCH_TARGET_WORK` | Jobs per batch | `100000` |
| `BALANCER_BENCH_MIN_BATCH_MS` | Minimum wall time per batch (calibration) | `50` |
| `BALANCER_BENCH_VERBOSE_STATS` | Print extra statistics | `0` |
| `BALANCER_BENCH_OUTPUT_CSV` | Optional CSV output path | empty |
| `BALANCER_BENCH_OUTPUT_JSON` | Optional JSON output path | empty |
| `BALANCER_BENCH_NO_SCOPE` | Disable priority/affinity changes | unset |
| `BALANCER_BENCH_NO_STABILIZE` | Disable CPU stabilization wait | unset |
| `BALANCER_BENCH_NO_COOLDOWN` | Disable cool-down sleeps | unset |
| `BALANCER_BENCH_NODE_COUNT` | Number of simulated nodes | `8` |
| `BALANCER_BENCH_PRIORITY_MIX` | Priority distribution (CSV: C,H,N,L,B) | `1,5,60,25,9` |

**Rule:** Every benchmark must print the resolved configuration once at startup.

---

## Platform Configuration

```cpp
#if defined(_WIN32) || defined(_WIN64)
static constexpr size_t DEFAULT_WARMUP_RUNS   = 3;
static constexpr size_t DEFAULT_MEASURED_RUNS = 15;
#else
static constexpr size_t DEFAULT_WARMUP_RUNS   = 3;
static constexpr size_t DEFAULT_MEASURED_RUNS = 50;
#endif
```

---

## CPU Frequency Monitoring

Use `FatPBenchmarkRunner.h`'s `CpuFreqInfo` if available, or implement locally following the reference source rule:

**Only print `[THROTTLED]` when the reference is a true base frequency.**

| Source | `ref_is_max` | Throttle detection |
|--------|-------------|-------------------|
| `base_frequency` | false | Reliable |
| `cpuinfo_max_freq` | true | Disabled |
| Windows registry `~MHz` | false | Reliable |

**Every benchmark function must call `print_cpu_context()` at its start** to record frequency state for that specific run.

---

## What to Benchmark

### Core Benchmark: `benchmark_balancer.cpp`

The primary benchmark measures end-to-end job throughput: submit → route → complete, with all five priority levels present and the cost model active.

**Measurements:**
- Jobs/second (overall throughput)
- P50 routing latency (time from submit to node assignment)
- P99 routing latency
- Throughput by priority band (Critical, High, Normal, Low, Bulk)

**Scenarios:**
1. **Steady state** — uniform load, all nodes healthy, model warm
2. **Burst** — 10x normal load for 1 second, then return to steady state
3. **Mixed priorities** — realistic distribution (1% Critical, 5% High, 60% Normal, 25% Low, 9% Bulk)
4. **Saturation** — load exceeds cluster capacity, measure admission rejection rate

### Policy Benchmark: `benchmark_policies.cpp`

Compares all scheduling policies under identical load:

| Policy | Measured |
|--------|---------|
| RoundRobin | Throughput, routing latency |
| LeastLoaded | Throughput, routing latency |
| WeightedCapacity | Throughput, routing latency |
| WorkStealing | Throughput, latency under uneven load |
| ShortestJobFirst | Throughput, mean job completion time |
| EarliestDeadlineFirst | Throughput, deadline miss rate |
| Composite | Throughput, all above per priority band |

**Important:** Policies must be compared with identical job streams and node configurations. Round-robin benchmark ordering across policies.

### Cost Model Benchmark: `benchmark_cost_model.cpp`

Measures the overhead introduced by the learning model:

- `predict()` call latency (cold vs. warm)
- `update()` call latency (EMA update per completion)
- Full cycle: submit → predict → route → complete → update
- Overhead comparison: balancer with learning vs. balancer with neutral (1.0) predictions

This benchmark answers the question: how much does the learning model cost?

### Admission Control Benchmark: `benchmark_admission.cpp`

Measures admission control at high request rates:

- Token bucket throughput at various request rates
- Leaky bucket throughput
- Holding queue behavior under cluster saturation
- Rejection rate by priority band under load

---

## Benchmark Structure

### Required Pattern

```cpp
void benchSubmitRoute(const BenchConfig& cfg)
{
    print_header("Submit → Route (all policies, mixed priorities)");
    print_cpu_context();

    // Setup OUTSIDE timed region
    SimulatedCluster cluster(cfg.nodeCount);
    Balancer balancer(cluster.nodes(), CompositePolicy{});
    auto jobs = generateJobs(cfg.targetWork, cfg.seed, cfg.priorityMix);

    // Correctness guardrail OUTSIDE timed region
    // Verify job generation produced expected distribution
    verifyPriorityDistribution(jobs, cfg.priorityMix);

    // Warmup
    for (size_t i = 0; i < cfg.warmupRuns; ++i)
    {
        for (auto& job : jobs)
            (void)balancer.submit(job);
        cluster.drainAll();
    }

    // Measured runs
    std::vector<double> samples;
    samples.reserve(cfg.measuredRuns);

    for (size_t batch = 0; batch < cfg.measuredRuns; ++batch)
    {
        auto t0 = high_resolution_clock::now();

        for (auto& job : jobs)
            DoNotOptimize(balancer.submit(job));
        cluster.drainAll();

        auto t1 = high_resolution_clock::now();
        samples.push_back(duration_cast<nanoseconds>(t1 - t0).count() / double(jobs.size()));
    }

    auto stats = computeStats(samples);
    print_result("Submit+Route", stats, "ns/job");
}
```

### Rules

- **Setup is outside the timed region** — node initialization, job generation, model warm-up do not count toward the benchmark
- **Correctness guardrails are outside the timed region** — verify the benchmark is measuring what it claims
- **`DoNotOptimize()` on results** — prevent the compiler from eliminating the work
- **Drain the cluster between batches** — so each batch starts from a clean state unless specifically testing accumulated load
- **Print CPU context at the start of each benchmark function**

---

## Statistics

Use the same statistics infrastructure as FAT-P benchmarks:

```cpp
struct BenchStats
{
    double median;
    double p25;
    double p75;
    double p99;
    double min;
    double max;
    double stddev;
    size_t n;
};

BenchStats computeStats(std::vector<double>& samples);  // sorts in place
```

Report median as the primary result. Include P99 for latency measurements. Include min/max to detect outliers.

---

## Correctness Guardrails

Every benchmark must include at least one correctness check **outside the timed region** that verifies the benchmark is doing real work:

```cpp
// After warmup, before measured runs:
{
    // Verify the balancer is actually routing to all nodes
    auto metrics = cluster.getClusterMetrics();
    if (metrics.activeNodes == 0)
        throw std::runtime_error("Benchmark setup error: no active nodes");

    // Verify job priority distribution matches config
    verifyPriorityDistribution(jobs, cfg.priorityMix);
}
```

**Rationale:** If the balancer silently rejects all jobs due to a misconfigured admission policy, the benchmark measures rejection overhead, not routing throughput. Guardrails catch this before the measured runs.

---

## Output Format

### Human-Readable Output

```
=== fatp-balancer Benchmark ===
Configuration:
  Nodes:        8
  Jobs/batch:   100000
  Batches:      50
  Priority mix: 1/5/60/25/9 (C/H/N/L/B)
  Seed:         12345
  CPU:          3700 MHz (base: 3700)

[Submit → Route, Composite policy, mixed priorities]
CPU: 3698 MHz (base: 3700)
  median:   142 ns/job
  p25:      138 ns/job
  p75:      147 ns/job
  p99:      183 ns/job
  min/max:  131 / 210 ns/job
  n:        50 batches

[Cost model overhead — warm vs. neutral]
CPU: 3699 MHz (base: 3700)
  With learning:   142 ns/job
  Without learning:  91 ns/job
  Overhead:          51 ns/job (56%)
```

### Machine-Readable Export

When `BALANCER_BENCH_OUTPUT_JSON` is set:

```json
{
  "timestamp": "2026-03-01T12:00:00Z",
  "config": {
    "node_count": 8,
    "jobs_per_batch": 100000,
    "batches": 50,
    "priority_mix": [1, 5, 60, 25, 9],
    "seed": 12345
  },
  "results": [
    {
      "name": "submit_route_composite",
      "unit": "ns/job",
      "median": 142.3,
      "p25": 138.1,
      "p75": 147.2,
      "p99": 183.4,
      "min": 131.0,
      "max": 210.5,
      "n": 50
    }
  ]
}
```

---

## What Not to Benchmark

- Do not benchmark the WASM demo renderer — that is SDL2 overhead, not balancer overhead
- Do not benchmark `SimulatedNode` internal thread dispatch in isolation and label it "balancer throughput" — include the full submit → route → complete cycle
- Do not benchmark with a single node — the balancer's routing decisions only matter with N > 1

---

## Performance Claims Policy

No specific benchmark numbers in `include/balancer/` headers, `policies/` headers, or documentation prose.

Do describe architectural properties:
- "O(1) prediction via hash map lookup"
- "EMA update is a single multiply-add per completion"
- "Lock-free ingestion path"

Benchmark results live in `bench/results/` where they are timestamped and platform-identified.

---

## Checklist Before Submitting a Benchmark

- [ ] `BALANCER_META` block present with `file_role: benchmark`
- [ ] Startup configuration print present
- [ ] CPU frequency monitoring present
- [ ] `print_cpu_context()` called at start of each benchmark function
- [ ] Setup and correctness guardrails are outside the timed region
- [ ] `DoNotOptimize()` applied to results
- [ ] Cluster drained between batches (unless testing accumulated load)
- [ ] Statistics include median and P99
- [ ] Human-readable output follows standard format
- [ ] Machine-readable JSON export supported via environment variable
- [ ] No specific multiplier claims in benchmark source comments

---

*fatp-balancer Benchmark Code Style Guide v1.0 — March 2026*
*Adapted from Fat-P Benchmark Code Style Guide v1.4*
