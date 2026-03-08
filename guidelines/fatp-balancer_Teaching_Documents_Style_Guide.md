# fatp-balancer Teaching Documents Style Guide

**Status:** Active
**Applies to:** All documentation files in `docs/` and component-level documentation
**Authority:** Subordinate to *fatp-balancer Development Guidelines*
**Version:** 1.0 (March 2026)

---

## Purpose

This guide ensures that fatp-balancer documentation teaches rather than merely describes. Every document must answer three questions: **what** this component does, **why** it is designed that way, and **when** to use it. Documents that only describe the API surface are incomplete.

---

## Document Types

| Type | Purpose | Trigger |
|------|---------|---------|
| **Overview** | Decision guide: should I use this component? | "What is X and should I use it?" |
| **User Manual** | Usage guide: how do I use this component? | "How do I call X?" |
| **Companion Guide** | Design rationale: why is X designed this way? | "Why does X work this way?" |
| **Case Study** | Failure analysis: why did this fail, how was it fixed? | "Why did this fail?" |
| **Foundations** | Background concepts: what do I need to know? | "What background is required?" |
| **Handbook** | Team discipline: how should we operate? | "What practices should we adopt?" |
| **Pattern Guide** | Applied patterns: how do I apply this design pattern? | "How do I structure X?" |
| **Design Note** | Decision record: what decision did we make and why? | "What did we decide?" |
| **Benchmark Results** | Performance data: how does this perform? | "What are the numbers?" |

---

## Document Selection Guide

| Question | Document |
|----------|----------|
| "Should I use RoundRobin or LeastLoaded?" | Overview for both |
| "How do I configure the AgingEngine?" | User Manual |
| "Why does the CostModel use EMA instead of a rolling average?" | Companion Guide |
| "Why did the learning model overfit to one node during testing?" | Case Study |
| "What is an EMA and why does it matter here?" | Foundations |
| "How should our team handle node failure scenarios in tests?" | Handbook |
| "How do I structure a composite scheduling policy?" | Pattern Guide |
| "Why did we choose JobClass tagging over payload inspection?" | Design Note |
| "What is the submit-route throughput at 1M jobs?" | Benchmark Results |

---

## Document Structure Requirements

### Overview

Every Overview must contain these sections in this order:

1. **What is this component?** — One paragraph, no jargon. What problem does it solve?
2. **The transferability context** — If this is an `include/balancer/` component, state explicitly that it ships to production. If it is a `sim/` component, state explicitly that it is a test fixture.
3. **When to use this** — Concrete decision criteria. What load patterns, priorities, or requirements make this the right choice?
4. **When NOT to use this** — Equal weight with "when to use." Every component has cases where it is the wrong tool.
5. **Performance characteristics** — Architectural description (complexity, allocation model). No specific benchmark numbers.
6. **Comparison with alternatives** — Honest comparison with other components that solve similar problems. Introduce each alternative with context before comparing.
7. **Limitations** — What does this component not do? What can go wrong?

**Vocabulary ban applies to Overviews.** See Development Guidelines §8.2.

### User Manual

Every User Manual must contain:

1. **Prerequisites** — What must be set up before using this component? (FAT-P path, CMake options)
2. **Quick Start** — Minimum working example, annotated
3. **Core Operations** — One section per major operation with complete, compilable examples
4. **Configuration** — All configurable parameters with types, defaults, and effects
5. **Error Handling** — Every `Expected` error code with the conditions that produce it and how to handle each
6. **Thread Safety** — Explicit statement: which operations are thread-safe, which require external synchronization
7. **Integration with the Balancer** — How does this component connect to the rest of the system?

### Companion Guide

A Companion Guide explains the **design decisions** behind the component — not the API. It answers "why" questions that do not belong in a User Manual.

Required sections:
1. **Design Goals** — What properties was this component designed to have?
2. **Key Design Decisions** — Each major decision with alternatives considered and why this was chosen
3. **Where It Excels** — Under what conditions does this design shine?
4. **Where It Loses** — Under what conditions is this design at a disadvantage? (Mandatory — no document is complete without this)
5. **Possible Futures** — What improvements are known? What would need to change for them to be worthwhile?

**"Where It Loses" is mandatory.** A Companion Guide that only presents the positive case is not honest.

### Case Study

Case Studies document real failures — during development, testing, or design. They exist so the same mistake is not made twice.

Required sections:
1. **Context** — What was being built, what was the goal?
2. **The Problem** — What went wrong? (Concrete, specific)
3. **Investigation** — How was the root cause identified?
4. **The Fix** — What was changed?
5. **What Was Learned** — What principle does this case study teach?
6. **Where to Look for Similar Issues** — Are there other places in the codebase where the same pattern might exist?

Case Studies are exempt from the vocabulary ban. Historical numbers are allowed ("we measured a 3x regression").

### Design Note

A Design Note is a decision record. It is written when a significant architectural decision is made.

Required sections:
1. **Decision** — What was decided? (One sentence)
2. **Context** — What forces led to this decision?
3. **Alternatives Considered** — What else was evaluated and why it was not chosen
4. **Consequences** — What does this decision enable? What does it constrain?
5. **Date and Author**

### Benchmark Results

Benchmark Results documents contain timestamped, platform-identified performance data.

Required sections:
1. **Platform** — Full hardware and compiler specification
2. **Build Configuration** — Flags, optimization level
3. **Methodology** — Warmup count, measured batch count, workload description, priority mix
4. **Results** — Tables with median, P99, min/max
5. **Interpretation** — What do the numbers mean? (Not marketing — honest interpretation)
6. **Limitations** — What do these numbers not tell you?

---

## Writing Standards

### Vocabulary Ban

The following terms are banned in Overviews, User Manuals, and Companion Guides. Replace with mechanism-specific language.

| Banned | Required replacement |
|--------|---------------------|
| "fast" | Complexity class or mechanism: "O(1)", "single atomic operation" |
| "efficient" | Mechanism: "zero-copy", "lock-free", "single allocation" |
| "safe" | Specify: "overflow-safe", "thread-safe", "exception-safe" |
| "simple" | Remove or explain specifically what is simpler than what |
| "powerful" | Remove |
| "easy" | Remove |
| "seamless" | Remove |
| "robust" | Specify the failure mode that is handled |
| "flexible" | Specify what can be customized and how |
| "scalable" | Provide the contention model or complexity under load |
| "production-tested" | Remove — the project has not been deployed to production |
| "battle-tested" | Remove |
| "production-ready" | Remove |

Teaching documents (Case Studies, Handbooks, Foundations) are exempt.

### The Transferability Principle in Documentation

Every document about an `include/balancer/` or `policies/` component must state the transferability principle clearly:

> This component is part of the transferable core. It ships to production unchanged. The simulation layer (`sim/`) is a test fixture that implements the same interfaces. Documentation for this component describes production behavior.

Every document about a `sim/` component must state the inverse:

> This component is part of the simulation layer. It is a test fixture, not a production component. Do not take it to production. For production deployment, implement `INode` (or `ISchedulingPolicy`) directly.

### The "Where It Loses" Rule

Every Companion Guide and every comparison section in an Overview must include an honest statement of limitations. This is mandatory, not optional.

**Bad:**

```markdown
## Why LeastLoaded is the Right Choice

LeastLoaded provides optimal work distribution by always routing to the least 
busy node, ensuring maximum utilization across your cluster.
```

**Good:**

```markdown
## When LeastLoaded Excels

LeastLoaded distributes work effectively when jobs have similar costs and 
nodes have similar capacities. The O(N) candidate scan is fast at small 
cluster sizes.

## When LeastLoaded Falls Short

LeastLoaded uses queue depth as a proxy for load. A node processing one 
expensive job looks "idle" compared to a node processing ten cheap jobs.
For workloads with high cost variance, ShortestJobFirst or Composite will 
produce better results. At large cluster sizes (N > 64), the O(N) scan 
becomes the bottleneck; consider a heap-backed variant.
```

### Concrete Examples

All examples must be:
- **Complete** — include all necessary includes and namespace qualifiers
- **Compilable** — verify they compile before committing
- **Annotated** — explain non-obvious lines with inline comments
- **Minimal** — show only what is needed to illustrate the point

### Tables

Use tables for:
- Comparison of components (introduce each component with context before the table)
- Configuration parameters
- Error codes and their conditions
- Complexity models

Do not use tables as a substitute for explanation. A table of performance numbers with no interpretation is incomplete.

### Diagrams

Use Mermaid diagrams where visual structure communicates better than prose:

| Diagram Type | Best For |
|-------------|---------|
| `stateDiagram-v2` | Node FSM, job lifecycle |
| `flowchart` | Admission control layers, aging pipeline |
| `sequenceDiagram` | Submit → route → complete cycle |
| `classDiagram` | Interface relationships |
| `graph` | Priority queue hierarchy, policy comparison |

**Required diagrams:**
- Node FSM diagram (in the NodeStateMachine or Balancer documentation)
- Priority aging flow (in the AgingEngine documentation)
- Admission control layers (in the AdmissionControl documentation)
- Learning model update pipeline (in the CostModel documentation)

**Mermaid syntax restrictions:**
- Avoid `()` in node labels (parsed as shape)
- Avoid `>=`, `<=`, `<>` (parsed as delimiters)
- Avoid `{}` (parsed as diamond shape)
- Keep labels concise

### Introducing External Projects Before Comparing

When comparing fatp-balancer to other schedulers or load balancers, introduce the external project with context before comparing. The reader may not know what is being compared against.

**Bad:**

```markdown
| Feature | fatp-balancer | HAProxy | Nginx |
|---------|--------------|---------|-------|
| Priority queues | Yes | No | No |
```

**Good:**

```markdown
### Context

**HAProxy** is a widely-deployed TCP/HTTP load balancer. Its scheduling 
focuses on connection-level routing, not job-level priority. It is the 
industry standard for HTTP workloads.

**Nginx** (in upstream mode) provides round-robin and weighted routing 
for HTTP traffic. Like HAProxy, it operates at the connection level.

fatp-balancer operates at the job level with a richer priority model.
The comparison below reflects this difference in abstraction.

[Then the table]
```

---

## Performance Claims in Documentation

**No specific benchmark numbers in headers or documentation prose.**

- No multiplier claims: "3x faster than RoundRobin"
- No absolute timing: "~142 ns per job"
- No percentage claims: "56% overhead from the learning model"

These numbers belong in `bench/results/` where they are timestamped and platform-identified.

**Do** describe architectural properties:
- "O(1) node lookup via FastHashMap"
- "EMA update is a single multiply-add per completion"
- "Lock-free ingestion path scales with producer count"
- "DegradationCurve update touches one bucket per completion"

**Exception:** Companion Guides and Case Studies may include historical numbers when describing the development process ("when we measured this optimization, we saw..."). This is a historical fact, not a performance claim.

---

## Library Maturity Claims

Do not use "production-tested," "battle-tested," or "production-ready." The project has not been deployed to production. Acknowledge honestly:

**Bad:**
> fatp-balancer is production-ready and battle-tested.

**Good:**
> fatp-balancer is a research and demonstration project. The architecture is designed to be transferable to production environments, but it has not been deployed in production. Use appropriate caution.

---

## Checklist Before Submitting Documentation

### Structure
- [ ] All required sections present for the document type
- [ ] "Where It Loses" present in Companion Guides and comparison sections

### Content Quality
- [ ] Each section explains what, why, and when — not just how
- [ ] Vocabulary ban enforced (Overviews, User Manuals, Companion Guides)
- [ ] Transferability principle stated explicitly where applicable
- [ ] No library maturity claims
- [ ] No specific benchmark numbers in prose
- [ ] External projects introduced before comparison

### Examples
- [ ] All examples are complete and compilable
- [ ] Namespace qualifiers are explicit (or local `using` in function scope)
- [ ] No `using namespace` at global scope

### Tables and Diagrams
- [ ] Tables preceded by explanatory prose
- [ ] Mermaid diagrams present where required
- [ ] Mermaid syntax restrictions followed

---

*fatp-balancer Teaching Documents Style Guide v1.0 — March 2026*
*Adapted from Fat-P Teaching Documents Style Guide*
