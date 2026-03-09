#!/usr/bin/env python3
"""
nn_advisor_harness.py — Training and inference harness for NNAdvisor.

Bridges the C++ fatp-balancer NNAdvisor with a scikit-learn classifier.

Subcommands
-----------
generate    Synthesise a labelled dataset without running the C++ sim.
            Uses the same threshold logic as TelemetryAdvisor::determineAlert()
            to produce ground-truth labels from randomly sampled cluster states.

collect     Label an existing directory of TelemetrySnapshot JSON files
            (written by the C++ sim layer) using the same threshold logic.

train       Train a classifier on a labelled JSONL dataset and save the model.

infer       One-shot: read a TelemetrySnapshot JSON file, run inference,
            write alert.json for NNAdvisor to pick up.

loop        Watch a TelemetrySnapshot file for changes, run inference on each
            update, and write alert.json.  This is the end-to-end runtime loop.

Dataset format (JSONL, one record per line)
-------------------------------------------
Each line is a JSON object with two keys:
  "features"  — dict matching TelemetrySnapshot field names (see extract_features)
  "label"     — one of "", "alert.none", "alert.overload",
                "alert.latency", "alert.node_fault"

Alert.json format
-----------------
  { "active_alert": "<label>" }

Thresholds (mirrors TelemetryAdvisor defaults exactly)
------------------------------------------------------
  unavailableFraction : 0.25   — node_fault if unavail/total >= this
  overloadFraction    : 0.75   — overload  if active/total  <  this
  latencyThresholdUs  : 10000  — latency   if meanP50Us     >= this (µs)

Usage examples
--------------
  # Bootstrap a 10 000-sample dataset
  python nn_advisor_harness.py generate --samples 10000 --out dataset.jsonl

  # Label snapshots written by the C++ sim
  python nn_advisor_harness.py collect --snapshots-dir ./snapshots --out dataset.jsonl

  # Train and save model
  python nn_advisor_harness.py train --dataset dataset.jsonl --model model.joblib

  # One-shot inference
  python nn_advisor_harness.py infer \\
      --model model.joblib \\
      --snapshot snapshot.json \\
      --alert-out alert.json

  # Runtime inference loop (polls every 0.5 s)
  python nn_advisor_harness.py loop \\
      --model model.joblib \\
      --snapshot snapshot.json \\
      --alert-out alert.json \\
      --interval 0.5
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import sys
import time
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Alerts — mirror BalancerFeatures.h constants exactly
# ---------------------------------------------------------------------------

ALERT_NONE       = ""
ALERT_NODE_FAULT = "alert.node_fault"
ALERT_OVERLOAD   = "alert.overload"
ALERT_LATENCY    = "alert.latency"

ALL_ALERTS = [ALERT_NONE, ALERT_NODE_FAULT, ALERT_OVERLOAD, ALERT_LATENCY]

# ---------------------------------------------------------------------------
# Thresholds — mirror TelemetryAdvisor defaults exactly
# ---------------------------------------------------------------------------

DEFAULT_UNAVAILABLE_FRACTION = 0.25
DEFAULT_OVERLOAD_FRACTION    = 0.75
DEFAULT_LATENCY_THRESHOLD_US = 10_000  # 10 ms

# ---------------------------------------------------------------------------
# Labeller — Python port of TelemetryAdvisor::determineAlert()
# ---------------------------------------------------------------------------

def determine_alert(
    snap: dict[str, Any],
    unavailable_fraction: float = DEFAULT_UNAVAILABLE_FRACTION,
    overload_fraction: float    = DEFAULT_OVERLOAD_FRACTION,
    latency_threshold_us: int   = DEFAULT_LATENCY_THRESHOLD_US,
) -> str:
    """
    Mirrors TelemetryAdvisor::determineAlert() exactly.

    Priority order: node_fault > overload > latency > "".

    Parameters
    ----------
    snap : dict
        A parsed TelemetrySnapshot JSON object.

    Returns
    -------
    str
        One of the ALERT_* constants.
    """
    active_nodes      = int(snap.get("activeNodes", 0))
    unavailable_nodes = int(snap.get("unavailableNodes", 0))
    total             = active_nodes + unavailable_nodes

    if total == 0:
        return ALERT_NONE

    unavail_frac = unavailable_nodes / total
    active_frac  = active_nodes / total
    mean_p50_us  = int(snap.get("clusterP50LatencyUs", 0))

    if unavail_frac >= unavailable_fraction:
        return ALERT_NODE_FAULT

    if active_frac < overload_fraction:
        return ALERT_OVERLOAD

    if mean_p50_us >= latency_threshold_us:
        return ALERT_LATENCY

    return ALERT_NONE

# ---------------------------------------------------------------------------
# Feature extraction
# ---------------------------------------------------------------------------

# Feature names — kept stable so saved models remain loadable.
FEATURE_NAMES = [
    "unavail_frac",          # unavailableNodes / total
    "active_frac",           # activeNodes / total
    "overloaded_frac",       # overloadedFraction (pre-computed in snapshot)
    "failed_frac",           # failedFraction     (pre-computed in snapshot)
    "log1p_p50_us",          # log1p(clusterP50LatencyUs)
    "log1p_p99_us",          # log1p(clusterP99LatencyUs)
    "in_flight_norm",        # inFlightJobs / max(totalSubmitted, 1)
    "mean_node_utilization", # mean utilization across nodes[]
    "max_node_queue_depth",  # max queueDepth across nodes[] (log1p)
    "mean_node_p50_us",      # mean p50LatencyUs across nodes (log1p)
    "rejected_frac",         # (rateLimited + saturated) / max(totalSubmitted, 1)
    "node_count_log",        # log1p(len(nodes))
]

NUM_FEATURES = len(FEATURE_NAMES)


def extract_features(snap: dict[str, Any]) -> list[float]:
    """
    Extract a fixed-length feature vector from a TelemetrySnapshot dict.

    All fields map directly to TelemetrySnapshot struct members.
    Missing fields default to 0 so partial snapshots don't crash the harness.
    """
    active_nodes      = int(snap.get("activeNodes", 0))
    unavailable_nodes = int(snap.get("unavailableNodes", 0))
    total             = max(active_nodes + unavailable_nodes, 1)
    total_submitted   = max(int(snap.get("totalSubmitted", 0)), 1)

    nodes          = snap.get("nodes", [])
    utils          = [float(n.get("utilization", 0.0)) for n in nodes]
    queue_depths   = [int(n.get("queueDepth", 0))       for n in nodes]
    node_p50s      = [int(n.get("p50LatencyUs", 0))     for n in nodes]

    mean_util       = sum(utils) / len(utils)       if utils else 0.0
    max_queue       = max(queue_depths)              if queue_depths else 0
    mean_node_p50   = sum(node_p50s) / len(node_p50s) if node_p50s else 0.0

    rejected_total = (
        int(snap.get("rejectedRateLimited", 0)) +
        int(snap.get("rejectedClusterSaturated", 0))
    )

    return [
        unavailable_nodes / total,
        active_nodes / total,
        float(snap.get("overloadedFraction", 0.0)),
        float(snap.get("failedFraction", 0.0)),
        math.log1p(int(snap.get("clusterP50LatencyUs", 0))),
        math.log1p(int(snap.get("clusterP99LatencyUs", 0))),
        int(snap.get("inFlightJobs", 0)) / total_submitted,
        mean_util,
        math.log1p(max_queue),
        math.log1p(mean_node_p50),
        rejected_total / total_submitted,
        math.log1p(len(nodes)),
    ]

# ---------------------------------------------------------------------------
# Alert.json writer
# ---------------------------------------------------------------------------

def write_alert_json(path: str | Path, alert: str) -> None:
    """Write the minimal alert JSON that NNAdvisor::evaluate() expects."""
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"active_alert": alert}, f)

# ---------------------------------------------------------------------------
# Subcommand: generate
# ---------------------------------------------------------------------------

def cmd_generate(args: argparse.Namespace) -> None:
    """
    Synthesise a labelled dataset by sampling random cluster states and
    labelling them with determine_alert().

    The sampling strategy covers all four alert regions and the healthy
    interior.  It does NOT require a running C++ sim.
    """
    rng     = random.Random(args.seed)
    samples = args.samples
    out     = Path(args.out)

    print(f"Generating {samples} samples → {out}")

    written   = {a: 0 for a in ALL_ALERTS}
    n_written = 0

    with out.open("w", encoding="utf-8") as f:
        while n_written < samples:
            # Sample cluster topology
            n_nodes        = rng.randint(2, 16)
            active_nodes   = rng.randint(0, n_nodes)
            unavail_nodes  = n_nodes - active_nodes
            overloaded_n   = rng.randint(0, active_nodes)
            failed_n       = rng.randint(0, unavail_nodes)

            overloaded_frac = overloaded_n / n_nodes if n_nodes else 0.0
            failed_frac     = failed_n / n_nodes if n_nodes else 0.0

            # Sample latencies
            p50_us = rng.randint(0, 50_000)
            p99_us = p50_us + rng.randint(0, 50_000)

            in_flight      = rng.randint(0, active_nodes * 20)
            total_submitted = max(in_flight + rng.randint(0, 100), 1)
            rej_rate       = rng.randint(0, total_submitted // 4)
            rej_saturated  = rng.randint(0, total_submitted // 8)

            # Build per-node entries
            nodes = []
            for i in range(n_nodes):
                util  = rng.uniform(0.0, 1.0)
                qdep  = rng.randint(0, 20)
                state = rng.randint(0, 7)
                nodes.append({
                    "nodeId":       i + 1,
                    "state":        state,
                    "utilization":  util,
                    "queueDepth":   qdep,
                    "p50LatencyUs": p50_us + rng.randint(-p50_us // 2, p50_us // 2 + 1),
                    "p99LatencyUs": p99_us,
                    "completedJobs": rng.randint(0, 1000),
                    "failedJobs":    rng.randint(0, 10),
                })

            snap: dict[str, Any] = {
                "nodes":                    nodes,
                "activeNodes":              active_nodes,
                "unavailableNodes":         unavail_nodes,
                "totalSubmitted":           total_submitted,
                "inFlightJobs":             in_flight,
                "rejectedRateLimited":      rej_rate,
                "rejectedPriorityRejected": 0,
                "rejectedClusterSaturated": rej_saturated,
                "overloadedFraction":       overloaded_frac,
                "failedFraction":           failed_frac,
                "clusterP99LatencyUs":      p99_us,
                "clusterP50LatencyUs":      p50_us,
            }

            label = determine_alert(snap)
            record = {"features": snap, "label": label}
            f.write(json.dumps(record) + "\n")
            written[label] += 1
            n_written += 1

    print("Label distribution:")
    for alert, count in written.items():
        name = alert if alert else '""'
        print(f"  {name:<22}  {count:>6}  ({100*count/samples:.1f}%)")

# ---------------------------------------------------------------------------
# Subcommand: collect
# ---------------------------------------------------------------------------

def cmd_collect(args: argparse.Namespace) -> None:
    """
    Label an existing directory of TelemetrySnapshot JSON files and write a
    JSONL dataset.  Files are expected to be named *.json and contain a single
    TelemetrySnapshot object each (as produced by sim::formatJson()).
    """
    snapshots_dir = Path(args.snapshots_dir)
    out           = Path(args.out)
    paths         = sorted(snapshots_dir.glob("*.json"))

    if not paths:
        print(f"No *.json files found in {snapshots_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Labelling {len(paths)} snapshot files → {out}")
    written = {a: 0 for a in ALL_ALERTS}

    with out.open("w", encoding="utf-8") as f:
        for p in paths:
            try:
                snap  = json.loads(p.read_text(encoding="utf-8"))
                label = determine_alert(snap)
                f.write(json.dumps({"features": snap, "label": label}) + "\n")
                written[label] += 1
            except Exception as e:
                print(f"  skipping {p.name}: {e}", file=sys.stderr)

    total = sum(written.values())
    print(f"Wrote {total} records. Label distribution:")
    for alert, count in written.items():
        name = alert if alert else '""'
        print(f"  {name:<22}  {count:>6}  ({100*count/max(total,1):.1f}%)")

# ---------------------------------------------------------------------------
# Subcommand: train
# ---------------------------------------------------------------------------

def cmd_train(args: argparse.Namespace) -> None:
    """
    Load a JSONL dataset and train an MLPClassifier.
    Saves the fitted (scaler + classifier) pipeline to a .joblib file.
    """
    try:
        import numpy as np
        from sklearn.neural_network import MLPClassifier
        from sklearn.preprocessing import StandardScaler
        from sklearn.pipeline import Pipeline
        from sklearn.model_selection import train_test_split
        from sklearn.metrics import classification_report
        import joblib
    except ImportError as e:
        print(f"Missing dependency: {e}", file=sys.stderr)
        print("Install with: pip install scikit-learn numpy joblib", file=sys.stderr)
        sys.exit(1)

    dataset = Path(args.dataset)
    out     = Path(args.model)

    print(f"Loading dataset from {dataset}")
    X_raw, y_raw = [], []
    with dataset.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            X_raw.append(extract_features(rec["features"]))
            y_raw.append(rec["label"])

    if not X_raw:
        print("Dataset is empty.", file=sys.stderr)
        sys.exit(1)

    X = np.array(X_raw, dtype=np.float32)
    y = np.array(y_raw)

    print(f"  {len(X)} samples, {X.shape[1]} features, "
          f"{len(set(y))} classes: {sorted(set(y))}")

    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=0.15, random_state=42, stratify=y
    )

    model = Pipeline([
        ("scaler", StandardScaler()),
        ("clf", MLPClassifier(
            hidden_layer_sizes=(64, 64),
            activation="relu",
            max_iter=500,
            random_state=42,
            verbose=False,
        )),
    ])

    print("Training…")
    model.fit(X_train, y_train)

    print("\nTest set report:")
    y_pred = model.predict(X_test)
    print(classification_report(y_test, y_pred, zero_division=0))

    joblib.dump(model, out)
    print(f"Model saved → {out}")

# ---------------------------------------------------------------------------
# Inference helpers
# ---------------------------------------------------------------------------

def load_model(model_path: str | Path):
    """Load a joblib-serialised sklearn Pipeline."""
    try:
        import joblib
    except ImportError:
        print("Missing dependency: joblib.  Install: pip install joblib", file=sys.stderr)
        sys.exit(1)
    return joblib.load(model_path)


def infer_from_snapshot(model, snap: dict[str, Any]) -> str:
    """
    Run model inference on a TelemetrySnapshot dict.
    Returns one of the ALERT_* constants.
    """
    import numpy as np
    feat   = np.array([extract_features(snap)], dtype=np.float32)
    result = model.predict(feat)[0]
    return str(result)

# ---------------------------------------------------------------------------
# Subcommand: infer
# ---------------------------------------------------------------------------

def cmd_infer(args: argparse.Namespace) -> None:
    """
    One-shot inference: read snapshot JSON, predict alert, write alert.json.
    """
    model = load_model(args.model)
    snap  = json.loads(Path(args.snapshot).read_text(encoding="utf-8"))
    alert = infer_from_snapshot(model, snap)

    write_alert_json(args.alert_out, alert)
    print(f"snapshot → alert: {alert!r}  (written to {args.alert_out})")

# ---------------------------------------------------------------------------
# Subcommand: loop
# ---------------------------------------------------------------------------

def cmd_loop(args: argparse.Namespace) -> None:
    """
    Watch a TelemetrySnapshot file for changes and write alert.json on each
    update.  This is the runtime path: the C++ sim writes the snapshot file,
    this loop reads it, runs inference, and NNAdvisor::evaluate() picks up
    the alert.json on the next tick.

    Loop behaviour:
    - Polls the snapshot file's mtime every --interval seconds.
    - On change: parses the snapshot, runs inference, writes alert.json.
    - On missing file: waits silently (the sim may not have written yet).
    - Ctrl-C exits cleanly.
    """
    model         = load_model(args.model)
    snapshot_path = Path(args.snapshot)
    alert_path    = Path(args.alert_out)
    interval      = float(args.interval)

    print(f"Watching  {snapshot_path}")
    print(f"Writing   {alert_path}")
    print(f"Interval  {interval} s  (Ctrl-C to stop)")

    last_mtime = None
    last_alert = None

    try:
        while True:
            time.sleep(interval)

            try:
                mtime = snapshot_path.stat().st_mtime
            except FileNotFoundError:
                continue  # sim hasn't written yet

            if mtime == last_mtime:
                continue  # no change

            try:
                snap  = json.loads(snapshot_path.read_text(encoding="utf-8"))
            except Exception as e:
                print(f"  parse error: {e}", file=sys.stderr)
                continue

            alert = infer_from_snapshot(model, snap)
            last_mtime = mtime

            if alert != last_alert:
                write_alert_json(alert_path, alert)
                label = alert if alert else '"" (healthy)'
                print(f"  [{time.strftime('%H:%M:%S')}] alert → {label}")
                last_alert = alert

    except KeyboardInterrupt:
        print("\nStopped.")

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="nn_advisor_harness",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    # generate
    g = sub.add_parser("generate", help="Synthesise a labelled dataset")
    g.add_argument("--samples", type=int, default=10_000,
                   help="Number of samples to generate (default: 10000)")
    g.add_argument("--out", default="dataset.jsonl",
                   help="Output JSONL path (default: dataset.jsonl)")
    g.add_argument("--seed", type=int, default=42,
                   help="Random seed (default: 42)")

    # collect
    c = sub.add_parser("collect", help="Label an existing snapshot directory")
    c.add_argument("--snapshots-dir", required=True,
                   help="Directory containing TelemetrySnapshot *.json files")
    c.add_argument("--out", default="dataset.jsonl",
                   help="Output JSONL path (default: dataset.jsonl)")

    # train
    t = sub.add_parser("train", help="Train a classifier on a labelled dataset")
    t.add_argument("--dataset", required=True, help="JSONL dataset path")
    t.add_argument("--model", required=True, help="Output .joblib model path")

    # infer
    i = sub.add_parser("infer", help="One-shot inference → alert.json")
    i.add_argument("--model", required=True, help="Trained .joblib model path")
    i.add_argument("--snapshot", required=True,
                   help="TelemetrySnapshot JSON file to read")
    i.add_argument("--alert-out", required=True,
                   help="Path to write alert.json for NNAdvisor")

    # loop
    lo = sub.add_parser("loop", help="Watch snapshot file, write alert.json on each update")
    lo.add_argument("--model", required=True, help="Trained .joblib model path")
    lo.add_argument("--snapshot", required=True,
                    help="TelemetrySnapshot JSON file to watch")
    lo.add_argument("--alert-out", required=True,
                    help="Path to write alert.json for NNAdvisor")
    lo.add_argument("--interval", type=float, default=0.5,
                    help="Poll interval in seconds (default: 0.5)")

    return p


def main() -> None:
    parser = build_parser()
    args   = parser.parse_args()

    dispatch = {
        "generate": cmd_generate,
        "collect":  cmd_collect,
        "train":    cmd_train,
        "infer":    cmd_infer,
        "loop":     cmd_loop,
    }
    dispatch[args.cmd](args)


if __name__ == "__main__":
    main()
