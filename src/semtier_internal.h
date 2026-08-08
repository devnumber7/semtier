#ifndef SEMTIER_INTERNAL_H
#define SEMTIER_INTERNAL_H

#include "semtier.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define SEMTIER_RING_MAGIC 0x53454d5449455231ull
#define SEMTIER_RING_CAPACITY 4096u
#define SEMTIER_RING_NAME_MAX 64u

struct semtier_ring {
    uint64_t magic;
    uint32_t capacity;
    uint32_t event_size;
    atomic_uint_fast64_t write_idx;
    atomic_uint_fast64_t dropped;
    struct semtier_event events[SEMTIER_RING_CAPACITY];
};

uint64_t semtier_now_ns(void);
size_t semtier_page_size(void);
size_t semtier_align_up(size_t value, size_t align);

int semtier_ring_init_for_process(char *name_buf, size_t name_buf_len,
                                  struct semtier_ring **ring_out);
void semtier_ring_close(const char *name, struct semtier_ring *ring);
int semtier_ring_open(const char *name, struct semtier_ring **ring_out);
void semtier_ring_unmap(struct semtier_ring *ring);
void semtier_ring_write(struct semtier_ring *ring,
                        const struct semtier_event *event);

int semtier_apply_initial_policy(void *addr, size_t len, uint32_t flags);
int semtier_move_range_to_node(int pid, void *addr, size_t len, int node);

#endif
