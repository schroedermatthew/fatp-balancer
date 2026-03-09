# nn_advisor_harness

Python training and inference harness for
[NNAdvisor](../../include/balancer/NNAdvisor.h).

## Overview

`NNAdvisor` expects the C++ side to read a small JSON file (`alert.json`)
written by an external process.  This harness is that process: it trains a
scikit-learn classifier on labeled cluster telemetry and runs an inference
loop that watches a `TelemetrySnapshot` JSON file, predicts the correct alert,
and writes `alert.json` for `NNAdvisor::evaluate()` to pick up.

```
C++ sim  ──writes──►  snapshot.json
                           │
                    nn_advisor_harness loop
                           │  (load model, predict)
                           ▼
                       alert.json  ◄──reads──  NNAdvisor::evaluate()
                                                     │
                                                     ▼
                                           FeatureSupervisor::applyChanges()
                                                     │
                                                     ▼
                                            policy switch / admission change
```

## Requirements

```
pip install scikit-learn numpy joblib
```

Python 3.9+.  No other dependencies.

## Quick start

```bash
# 1. Bootstrap a labelled dataset (no C++ sim required)
python nn_advisor_harness.py generate --samples 10000 --out dataset.jsonl

# 2. Train
python nn_advisor_harness.py train --dataset dataset.jsonl --model model.joblib

# 3. Run the inference loop alongside the C++ sim
python nn_advisor_harness.py loop \
    --model    model.joblib \
    --snapshot /path/to/snapshot.json \
    --alert-out /path/to/alert.json \
    --interval  0.5
```

Pass the same `alert.json` path to `NNAdvisor` on the C++ side:

```cpp
balancer::NNAdvisor advisor{supervisor, "/path/to/alert.json"};
// In the tick loop:
advisor.evaluate();
```

## Subcommands

### generate

Synthesises a labelled dataset by sampling random cluster states and labelling
them with the same threshold logic as `TelemetryAdvisor::determineAlert()`.
No running C++ sim is required.

```
python nn_advisor_harness.py generate \
    --samples 10000 \
    --out     dataset.jsonl \
    --seed    42
```

**Note on alert.overload:** With default thresholds, `alert.overload` fires when
nodes are in **Overloaded** state (busy but not failed).  A cluster where all nodes
are Overloaded has `activeFrac = 0` and `unavailFrac = 0`, so overload fires
without triggering node_fault.  The denominator is `knownNodes = active + unavailable
+ overloaded`, matching `buildClusterMetrics()` in `Balancer.h`.  The generator
samples active/overloaded/unavailable as three disjoint buckets and produces all
four alert classes.

### collect

Labels an existing directory of `TelemetrySnapshot` JSON files.  Use this
when the C++ sim writes snapshots to disk during a test run.

```
python nn_advisor_harness.py collect \
    --snapshots-dir ./run_snapshots \
    --out           dataset.jsonl
```

Each `*.json` file must contain a single `TelemetrySnapshot` object as
produced by `sim::formatJson()`.

### train

Trains a two-hidden-layer MLP (64 × 64, ReLU) on a JSONL dataset.  Prints a
classification report on a held-out 15% test split.

```
python nn_advisor_harness.py train \
    --dataset dataset.jsonl \
    --model   model.joblib
```

The saved `.joblib` file contains a `sklearn.pipeline.Pipeline` with a
`StandardScaler` and an `MLPClassifier`.  The feature vector and scaler are
bundled together so inference is a single `model.predict(X)` call.

### infer

One-shot: read a snapshot, predict, write `alert.json`.

```
python nn_advisor_harness.py infer \
    --model      model.joblib \
    --snapshot   snapshot.json \
    --alert-out  alert.json
```

### loop

Watch a snapshot file for changes and write `alert.json` on each update.
This is the normal runtime path.

```
python nn_advisor_harness.py loop \
    --model      model.joblib \
    --snapshot   snapshot.json \
    --alert-out  alert.json \
    --interval   0.5
```

The loop polls `snapshot.json`'s mtime every `--interval` seconds.  On a
change it runs inference and writes `alert.json` only if the predicted alert
has changed.  Missing snapshot file: waits silently until the sim creates it.
Ctrl-C exits cleanly.

## Feature vector

12 features extracted from `TelemetrySnapshot`:

| # | Name                   | Description                                   |
|---|------------------------|-----------------------------------------------|
| 0 | `unavail_frac`         | `unavailableNodes / total`                    |
| 1 | `active_frac`          | `activeNodes / total`                         |
| 2 | `overloaded_frac`      | `overloadedFraction` (pre-computed in snapshot)|
| 3 | `failed_frac`          | `failedFraction` (pre-computed in snapshot)   |
| 4 | `log1p_p50_us`         | `log1p(clusterP50LatencyUs)`                  |
| 5 | `log1p_p99_us`         | `log1p(clusterP99LatencyUs)`                  |
| 6 | `in_flight_norm`       | `inFlightJobs / max(totalSubmitted, 1)`        |
| 7 | `mean_node_utilization`| Mean `utilization` across `nodes[]`           |
| 8 | `max_node_queue_depth` | `log1p(max queueDepth across nodes[])`        |
| 9 | `mean_node_p50_us`     | `log1p(mean p50LatencyUs across nodes[])`     |
|10 | `rejected_frac`        | `(rateLimited + saturated) / totalSubmitted`  |
|11 | `node_count_log`       | `log1p(len(nodes))`                           |

## Alert labels

| Label              | Condition (mirrors TelemetryAdvisor defaults)         |
|--------------------|-------------------------------------------------------|
| `""`               | Healthy — no threshold crossed                        |
| `"alert.node_fault"`| `unavailableNodes / total >= 0.25`                   |
| `"alert.overload"` | `activeNodes / knownNodes < 0.75` (knownNodes includes Overloaded nodes)|
| `"alert.latency"`  | `clusterP50LatencyUs >= 10000 µs`                    |

Priority: `node_fault > overload > latency > ""`.

## alert.json schema

```json
{ "active_alert": "alert.overload" }
```

This is the exact schema `NNAdvisor::evaluate()` expects.  Unknown values are
rejected by the C++ validator before reaching `applyChanges()`.
