# SemTier Prototype

SemTier is a standalone C prototype for allocation-time semantic
vulnerability hints. It is intentionally independent of Tierce: the runtime
turns semantic allocation contexts into concrete virtual address ranges, then
exports those ranges through a POSIX shared-memory ring and optional JSONL log.

## Build

```sh
make
```

## Runtime API

The first useful path is manual instrumentation:

```c
semtier_region_begin(SITE_LIST_NODE,
    SEMTIER_HINT_SERIAL_DEP |
    SEMTIER_HINT_POINTER_CHASING |
    SEMTIER_HINT_LATENCY_CRITICAL);

node = semtier_alloc(sizeof(*node), SITE_LIST_NODE, SEMTIER_HINT_OWNED);

semtier_region_end();
```

The runtime groups marked allocations into page-aligned `mmap()` arenas and
publishes `REGION_CREATE`, `REGION_GROW`, and `REGION_DEAD` events.

## Standalone Testing

Run a pointer-chasing benchmark:

```sh
SEMTIER_EVENT_LOG=/tmp/semtier-events.jsonl \
  ./bench/pointer-chase --mode semtier --bench chase --nodes 1000000 --iters 4
```

Run the malloc baseline:

```sh
./bench/pointer-chase --mode malloc --bench chase --nodes 1000000 --iters 4
```

Or use the Nestfarm smoke script:

```sh
./scripts/nestfarm_smoke.sh
```

Print ring events while the process is alive:

```sh
./tools/semtier-drain --pid <pid> --follow
```

The benchmark also writes the active ring name to stdout.

## NUMA Initial Placement On Linux

On Linux, the runtime can apply initial placement with `mbind()` before pages are
touched:

```sh
SEMTIER_FAST_NODE=0 SEMTIER_SLOW_NODE=1 \
  ./bench/pointer-chase --mode semtier --bench chase
```

The drain tool can also attempt `move_pages()` for another running process:

```sh
./tools/semtier-drain --pid <pid> --follow --apply-policy \
  --fast-node 0 --slow-node 1
```

This usually needs permission to move another process's pages. The runtime-side
`mbind()` path is the simpler standalone path for Nestfarm experiments.

For a first two-node run:

```sh
SEMTIER_FAST_NODE=0 SEMTIER_SLOW_NODE=1 \
  ./bench/pointer-chase --mode semtier --bench chase --nodes 5000000 --iters 4
```

If placement does not match the selected node, enable diagnostics:

```sh
SEMTIER_DEBUG=1 SEMTIER_STRICT_POLICY=1 \
SEMTIER_FAST_NODE=1 SEMTIER_SLOW_NODE=0 \
  ./bench/pointer-chase --mode semtier --bench chase --nodes 5000000 --iters 20
```

<!--
## Tierce Integration Point

Tierce should consume the same event shape:

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

The intended Tierce hook is scoring-only at first:

```text
PAC = observed_PAC + decaying_semantic_prior(range_flags)
```

This keeps Tierce authoritative for placement and resource allocation while
letting compiler/runtime semantics reduce discovery latency.
 --!>
