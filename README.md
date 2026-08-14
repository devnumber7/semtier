# SemTier

SemTier is a standalone C prototype for testing allocation-time semantic
memory-tiering hints. The prototype takes a semantic property of an allocation
context, such as "this region contains serialized pointer-chasing data", 
realizes it as a concrete virtual address range at runtime, and optionally
applies NUMA placement to that range.

The current prototype is intentionally small. It does not yet perform LLVM
analysis. It proves the runtime substrate needed by those
later steps:

```text
semantic allocation hint -> mmap arena -> virtual address range -> NUMA policy / future Tierce metadata
```

## Result Summary

The initial CloudLab experiment uses a dual-socket NUMA machine as a local-vs-
remote memory stand-in. Computation is pinned to NUMA node 0. The benchmark then
places pointer-chasing data either on node 0 or node 1.

![SemTier NUMA matrix]([https://github.com/devnumber7/semtier/blob/main/matrix_results.svg])

The expected result is that remote pointer chasing is slower because each next
address depends on the previous load. In the collected run:

```text
malloc_remote/local  = 1.644x
semtier_remote/local = 1.641x
```

The matching ratios show that SemTier's semantic arena placement reproduces the
same hardware NUMA penalty as `numactl`-controlled malloc placement. This
validates that SemTier can turn a semantic allocation hint into a concrete
memory range whose physical placement behaves as expected.

## SemTier Breakdown

SemTier has three pieces:

1. A C runtime allocator that routes marked allocations into page-aligned
   `mmap()` arenas.
2. A metadata/event path that exports arena ranges as `(pid, addr, len, site_id,
   ds_id, flags)` records.
3. A Linux NUMA policy path that uses `mbind()` to place semantic arenas before
   pages are touched.

The runtime API is plain C:

```c
semtier_region_begin(SITE_LIST_NODE,
    SEMTIER_HINT_SERIAL_DEP |
    SEMTIER_HINT_POINTER_CHASING |
    SEMTIER_HINT_LATENCY_CRITICAL);

node = semtier_alloc(sizeof(*node), SITE_LIST_NODE, SEMTIER_HINT_OWNED);

semtier_region_end();
```

Marked allocations are packed into semantic arenas. Unmarked allocations can
fall back to normal `malloc`.

## Reasoning 

The compiler cannot directly tag heap pages because heap addresses do not exist
until runtime. A compiler or programmer can only identify an allocation context:

```text
this allocation site/context is part of a serialized pointer-chasing structure
```

SemTier binds that static or semantic fact to concrete runtime memory:

```text
pid = X
addr = 0x...
len = N bytes
flags = SERIAL_DEP | POINTER_CHASING | LATENCY_CRITICAL
```

That is the form a tiering system can consume later. The said tiering system 
remains responsible for migration and scheduling while using this semantic range
metadata as an initial criticality prior.

## Benchmark

The first benchmark is a randomized linked-list traversal:

```c
for (struct node *cur = head; cur; cur = cur->next) {
    sum += cur->value;
}
```

This is a serialized memory access pattern. The next address comes from the
current load, so a remote miss is difficult for the CPU to overlap with other
independent memory requests. That makes it a good minimal benchmark for
semantic vulnerability hints.

The automated collection script runs four cases:

```text
semtier_local   CPU node0, SemTier serial arena on node0
semtier_remote  CPU node0, SemTier serial arena on node1
malloc_local    CPU node0, malloc memory on node0
malloc_remote   CPU node0, malloc memory on node1
```

The malloc cases establish the hardware baseline. The SemTier cases test
whether semantic arena placement can reproduce the same local-vs-remote effect.

## Build

```sh
make
```

Required on CloudLab/Ubuntu:

```sh
sudo apt update
sudo apt install -y build-essential numactl
```

## Standalone Runs

Run SemTier pointer chasing:

```sh
SEMTIER_FAST_NODE=0 SEMTIER_SLOW_NODE=1 \
  numactl --cpunodebind=0 \
  ./bench/pointer-chase --mode semtier --bench chase --nodes 5000000 --iters 20
```

Run malloc local and remote baselines:

```sh
numactl --cpunodebind=0 --membind=0 \
  ./bench/pointer-chase --mode malloc --bench chase --nodes 5000000 --iters 20

numactl --cpunodebind=0 --membind=1 \
  ./bench/pointer-chase --mode malloc --bench chase --nodes 5000000 --iters 20
```

Inspect placement during the benchmark sleep window:

```sh
numastat -p $(pgrep -n pointer-chase)
```

If placement does not match the selected node, enable strict diagnostics:

```sh
SEMTIER_DEBUG=1 SEMTIER_STRICT_POLICY=1 \
SEMTIER_FAST_NODE=1 SEMTIER_SLOW_NODE=0 \
  numactl --cpunodebind=0 \
  ./bench/pointer-chase --mode semtier --bench chase \
  --nodes 5000000 --iters 20 --sleep-before-shutdown 20
```

Expected diagnostic:

```text
semtier: mbind ... target_node=1 ... rc=0 errno=0
```

## Automated Collection

Collect repeated measurements:

```sh
./scripts/collect_matrix.py --nodes 5000000 --iters 20 --reps 3 --sleep 5
```

Outputs:

```text
results/collection/matrix_results.csv
results/collection/matrix_results.json
results/collection/*_rep*.jsonl
```

Generate the SVG plot from the CSV:

```sh
./scripts/plot_matrix.py results/collection/matrix_results.csv
```

This writes:

```text
results/collection/matrix_results.svg
```

The README image above points at that SVG path, so rerunning the plotter updates
the embedded result.

## Event Format

The runtime exports semantic arena events with this C ABI:

```c
struct semtier_event {
    uint32_t type;
    uint32_t flags;
    uint32_t site_id;
    uint32_t pid;
    uint64_t ds_id;
    uint64_t addr;
    uint64_t len;
    uint64_t timestamp_ns;
};
```

For future Tierce integration, the first intended hook is scoring-only:

```text
PAC = observed_PAC + decaying_semantic_prior(range_flags)
```

This keeps Tierce authoritative for page placement and resource allocation while
allowing compiler/runtime semantics to reduce discovery latency.

## Current Status

Proven:

- SemTier creates compact semantic arenas for pointer-chasing heap data.
- SemTier can place those arenas on either NUMA node with `mbind()`.
- Remote placement produces the same slowdown ratio as a malloc/numactl remote
  baseline.
- The collector and plotter produce reproducible CSV/JSON/SVG output.

Not yet implemented:

- Automatic LLVM inference of loop-carried dependent allocation contexts.
- Data-structure ownership inference.
- Integration benchmark with an actual tiering system. 
- Larger real-data-structure benchmarks such as hash-table chaining, trees, or
  graph traversal.
