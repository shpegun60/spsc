# Retained Baseline Results

The canonical baseline runner writes timestamped JSONL, manifest, and assembly
artifacts into this directory. Retain a result only when its manifest identifies
the source revision, toolchain, flags, CPU topology, affinity outcome, and
Rigtorp gitlink used to produce it.

Do not overwrite a prior result file. A new run gets a new timestamped name so
raw samples remain available for later comparison.
