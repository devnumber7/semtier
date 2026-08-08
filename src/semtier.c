#include "semtier.h"
#include "semtier_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

#define SEMTIER_MAX_ARENAS 1024u
#define SEMTIER_REGION_STACK 64u
#define SEMTIER_DEFAULT_ARENA_MB 64u
#define SEMTIER_ALLOC_ALIGN _Alignof(max_align_t)

struct semtier_arena {
    void *base;
    size_t len;
    size_t offset;
    size_t next_report_offset;
    uint32_t site_id;
    uint32_t flags;
    semtier_ds_t ds_id;
};

struct semtier_region_ctx {
    semtier_ds_t ds_id;
    uint32_t site_id;
    uint32_t flags;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct semtier_arena g_arenas[SEMTIER_MAX_ARENAS];
static size_t g_arena_count;
static atomic_uint_fast64_t g_next_ds = 1;
static int g_initialized;
static struct semtier_ring *g_ring;
static char g_ring_name[SEMTIER_RING_NAME_MAX];
static FILE *g_log;
static size_t g_arena_bytes;
static size_t g_grow_granule;

static _Thread_local struct semtier_region_ctx g_region_stack[SEMTIER_REGION_STACK];
static _Thread_local size_t g_region_depth;

static size_t env_size_mb(const char *name, size_t fallback_mb) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback_mb;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || parsed == 0) {
        return fallback_mb;
    }
    return (size_t)parsed;
}

static int hint_uses_arena(uint32_t flags) {
    return (flags & (SEMTIER_HINT_SERIAL_DEP | SEMTIER_HINT_STREAMING |
                     SEMTIER_HINT_BACKGROUND | SEMTIER_HINT_ROOT |
                     SEMTIER_HINT_OWNED | SEMTIER_HINT_POINTER_CHASING |
                     SEMTIER_HINT_LATENCY_CRITICAL)) != 0;
}

static void publish_event(uint32_t type, void *addr, size_t len, uint32_t site_id,
                          semtier_ds_t ds_id, uint32_t flags) {
    struct semtier_event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.flags = flags;
    event.site_id = site_id;
    event.pid = (uint32_t)getpid();
    event.ds_id = ds_id;
    event.addr = (uint64_t)(uintptr_t)addr;
    event.len = (uint64_t)len;
    event.timestamp_ns = semtier_now_ns();

    semtier_ring_write(g_ring, &event);

    if (g_log) {
        fprintf(g_log,
                "{\"type\":%u,\"pid\":%u,\"site_id\":%u,\"ds_id\":%llu,"
                "\"addr\":\"0x%llx\",\"len\":%llu,\"flags\":%u,"
                "\"timestamp_ns\":%llu}\n",
                event.type, event.pid, event.site_id,
                (unsigned long long)event.ds_id,
                (unsigned long long)event.addr,
                (unsigned long long)event.len, event.flags,
                (unsigned long long)event.timestamp_ns);
    }
}

void semtier_init(void) {
    pthread_mutex_lock(&g_lock);
    if (g_initialized) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    g_arena_bytes = env_size_mb("SEMTIER_ARENA_MB", SEMTIER_DEFAULT_ARENA_MB);
    g_arena_bytes *= 1024u * 1024u;
    g_arena_bytes = semtier_align_up(g_arena_bytes, semtier_page_size());
    g_grow_granule = env_size_mb("SEMTIER_GROW_GRANULE_MB", 2u);
    g_grow_granule *= 1024u * 1024u;
    g_grow_granule = semtier_align_up(g_grow_granule, semtier_page_size());

    const char *log_path = getenv("SEMTIER_EVENT_LOG");
    if (log_path && log_path[0] != '\0') {
        g_log = fopen(log_path, "a");
    }

    if (semtier_ring_init_for_process(g_ring_name, sizeof(g_ring_name), &g_ring) != 0) {
        g_ring = NULL;
        g_ring_name[0] = '\0';
    }

    g_initialized = 1;
    pthread_mutex_unlock(&g_lock);
}

void semtier_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_initialized) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    for (size_t i = 0; i < g_arena_count; ++i) {
        if (g_arenas[i].base) {
            publish_event(SEMTIER_EVENT_REGION_DEAD, g_arenas[i].base,
                          g_arenas[i].len, g_arenas[i].site_id,
                          g_arenas[i].ds_id, g_arenas[i].flags);
            munmap(g_arenas[i].base, g_arenas[i].len);
        }
    }
    g_arena_count = 0;

    if (g_log) {
        fflush(g_log);
        fclose(g_log);
        g_log = NULL;
    }

    semtier_ring_close(g_ring_name, g_ring);
    g_ring = NULL;
    g_ring_name[0] = '\0';
    g_initialized = 0;
    pthread_mutex_unlock(&g_lock);
}

static struct semtier_arena *find_arena(size_t size, uint32_t site_id,
                                        semtier_ds_t ds_id, uint32_t flags) {
    uint32_t arena_flags = flags;
    for (size_t i = 0; i < g_arena_count; ++i) {
        struct semtier_arena *arena = &g_arenas[i];
        if (arena->site_id == site_id && arena->ds_id == ds_id &&
            arena->flags == arena_flags &&
            arena->offset + size <= arena->len) {
            return arena;
        }
    }
    return NULL;
}

static struct semtier_arena *create_arena(size_t min_size, uint32_t site_id,
                                          semtier_ds_t ds_id, uint32_t flags) {
    if (g_arena_count >= SEMTIER_MAX_ARENAS) {
        errno = ENOMEM;
        return NULL;
    }

    size_t page = semtier_page_size();
    size_t len = g_arena_bytes;
    if (len < min_size) {
        len = semtier_align_up(min_size, page);
    }

    void *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                      -1, 0);
    if (base == MAP_FAILED) {
        return NULL;
    }

    (void)semtier_apply_initial_policy(base, len, flags);

    struct semtier_arena *arena = &g_arenas[g_arena_count++];
    arena->base = base;
    arena->len = len;
    arena->offset = 0;
    arena->next_report_offset = g_grow_granule;
    arena->site_id = site_id;
    arena->flags = flags;
    arena->ds_id = ds_id;

    publish_event(SEMTIER_EVENT_REGION_CREATE, base, len, site_id, ds_id, flags);
    return arena;
}

void *semtier_alloc(size_t size, uint32_t site_id, uint32_t flags) {
    if (!g_initialized) {
        semtier_init();
    }

    if (size == 0) {
        size = 1;
    }

    if (!hint_uses_arena(flags) && g_region_depth == 0) {
        return malloc(size);
    }

    semtier_ds_t ds_id = 0;
    uint32_t effective_flags = flags;
    if (g_region_depth > 0) {
        struct semtier_region_ctx *ctx = &g_region_stack[g_region_depth - 1u];
        ds_id = ctx->ds_id;
        effective_flags |= ctx->flags;
        if ((effective_flags & SEMTIER_HINT_OWNED) && site_id == 0) {
            site_id = ctx->site_id;
        }
    } else {
        ds_id = ((uint64_t)site_id << 32u) | (uint64_t)flags;
    }

    size_t aligned = semtier_align_up(size, SEMTIER_ALLOC_ALIGN);
    pthread_mutex_lock(&g_lock);
    struct semtier_arena *arena = find_arena(aligned, site_id, ds_id, effective_flags);
    if (!arena) {
        arena = create_arena(aligned, site_id, ds_id, effective_flags);
    }
    if (!arena) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    void *ptr = (char *)arena->base + arena->offset;
    arena->offset += aligned;
    if (arena->offset >= arena->next_report_offset) {
        publish_event(SEMTIER_EVENT_REGION_GROW, arena->base, arena->offset,
                      arena->site_id, arena->ds_id, arena->flags);
        while (arena->next_report_offset <= arena->offset) {
            arena->next_report_offset += g_grow_granule;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ptr;
}

void semtier_free(void *ptr) {
    if (!ptr) {
        return;
    }

    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < g_arena_count; ++i) {
        uintptr_t base = (uintptr_t)g_arenas[i].base;
        uintptr_t end = base + g_arenas[i].len;
        uintptr_t value = (uintptr_t)ptr;
        if (value >= base && value < end) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
    }
    pthread_mutex_unlock(&g_lock);
    free(ptr);
}

semtier_ds_t semtier_region_begin(uint32_t site_id, uint32_t flags) {
    if (!g_initialized) {
        semtier_init();
    }
    if (g_region_depth >= SEMTIER_REGION_STACK) {
        return 0;
    }

    semtier_ds_t ds_id = atomic_fetch_add_explicit(&g_next_ds, 1,
                                                   memory_order_relaxed);
    g_region_stack[g_region_depth].ds_id = ds_id;
    g_region_stack[g_region_depth].site_id = site_id;
    g_region_stack[g_region_depth].flags = flags | SEMTIER_HINT_ROOT;
    g_region_depth++;
    return ds_id;
}

void semtier_region_end(void) {
    if (g_region_depth > 0) {
        g_region_depth--;
    }
}

semtier_ds_t semtier_current_region(void) {
    if (g_region_depth == 0) {
        return 0;
    }
    return g_region_stack[g_region_depth - 1u].ds_id;
}

void semtier_publish_region(void *addr, size_t len, uint32_t site_id,
                            semtier_ds_t ds_id, uint32_t flags) {
    if (!g_initialized) {
        semtier_init();
    }
    publish_event(SEMTIER_EVENT_REGION_CREATE, addr, len, site_id, ds_id, flags);
}

void semtier_flush(void) {
    pthread_mutex_lock(&g_lock);
    if (g_log) {
        fflush(g_log);
    }
    pthread_mutex_unlock(&g_lock);
}

const char *semtier_ring_name(void) {
    if (!g_initialized) {
        semtier_init();
    }
    return g_ring_name;
}
