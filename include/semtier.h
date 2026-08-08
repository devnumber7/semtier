#ifndef SEMTIER_H
#define SEMTIER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEMTIER_HINT_SERIAL_DEP        (1u << 0)
#define SEMTIER_HINT_STREAMING         (1u << 1)
#define SEMTIER_HINT_BACKGROUND        (1u << 2)
#define SEMTIER_HINT_ROOT              (1u << 3)
#define SEMTIER_HINT_OWNED             (1u << 4)
#define SEMTIER_HINT_POINTER_CHASING   (1u << 5)
#define SEMTIER_HINT_LATENCY_CRITICAL  (1u << 6)

#define SEMTIER_EVENT_REGION_CREATE 1u
#define SEMTIER_EVENT_REGION_GROW   2u
#define SEMTIER_EVENT_REGION_DEAD   3u
#define SEMTIER_EVENT_ALLOC_SAMPLE  4u

typedef uint64_t semtier_ds_t;

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

void semtier_init(void);
void semtier_shutdown(void);

void *semtier_alloc(size_t size, uint32_t site_id, uint32_t flags);
void semtier_free(void *ptr);

semtier_ds_t semtier_region_begin(uint32_t site_id, uint32_t flags);
void semtier_region_end(void);
semtier_ds_t semtier_current_region(void);

void semtier_publish_region(void *addr, size_t len, uint32_t site_id,
                            semtier_ds_t ds_id, uint32_t flags);
void semtier_flush(void);

const char *semtier_ring_name(void);

#ifdef __cplusplus
}
#endif

#endif
