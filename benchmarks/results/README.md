# Retained Baseline Results

The canonical baseline runner writes timestamped JSONL, manifest, and assembly
artifacts into this directory. Retain a result only when its manifest identifies
the source revision, toolchain, flags, CPU topology, affinity outcome, and
Rigtorp gitlink used to produce it.

Every retained manifest must classify itself explicitly:

- `release`: clean library and harness revisions, the canonical minimum sample
  protocol, and distinct producer/consumer affinity
- `diagnostic`: useful investigation data that must not support a release claim
- `diagnostic_invalid`: retained only to explain historical numbers or a known
  harness problem

Use `run_spsc_baseline.ps1 -EvidenceClass release -CaptureLabel <label>` for
release evidence. The runner rejects dirty library or harness worktrees, short
captures, and unresolved same-CPU placement in that mode.

## Retained inventory

| Artifact prefix | Classification | Purpose |
| --- | --- | --- |
| `20260802-181024-026-85904897cc57` | `diagnostic_invalid` | Initial H0 short capture; dirty worktree, CPUs `0,1`, and the pre-hardening retry loop. It is not a canonical baseline. |

Clean H0, H2, and H8 release captures remain required before their raw numbers
are used as committed release evidence. Do not promote untracked exploratory
captures merely because they used more samples; rerun the named revision with
the current clean harness and explicit `release` classification.

Do not overwrite a prior result file. A new run gets a new timestamped name so
raw samples remain available for later comparison.
