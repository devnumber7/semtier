#include "semtier_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>

#ifndef MPOL_BIND
#define MPOL_BIND 2
#endif

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE 2
#endif

static long semtier_mbind(void *addr, unsigned long len, int mode,
                          const unsigned long *nodemask,
                          unsigned long maxnode, unsigned flags) {
#ifdef __NR_mbind
    return syscall(__NR_mbind, addr, len, mode, nodemask, maxnode, flags);
#else
    (void)addr;
    (void)len;
    (void)mode;
    (void)nodemask;
    (void)maxnode;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}
#endif

static int env_int(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) {
        return fallback;
    }
    return (int)parsed;
}

int semtier_apply_initial_policy(void *addr, size_t len, uint32_t flags) {
    int fast_node = env_int("SEMTIER_FAST_NODE", -1);
    int slow_node = env_int("SEMTIER_SLOW_NODE", -1);
    int target = -1;

    if (flags & (SEMTIER_HINT_SERIAL_DEP | SEMTIER_HINT_POINTER_CHASING |
                 SEMTIER_HINT_LATENCY_CRITICAL)) {
        target = fast_node;
    } else if (flags & (SEMTIER_HINT_STREAMING | SEMTIER_HINT_BACKGROUND)) {
        target = slow_node;
    }

    if (target < 0) {
        return 0;
    }

#if defined(__linux__)
    const char *debug = getenv("SEMTIER_DEBUG");
    unsigned long mask = 1ul << (unsigned)target;
    errno = 0;
    long rc = semtier_mbind(addr, (unsigned long)len, MPOL_BIND, &mask,
                            (unsigned long)(sizeof(mask) * 8u), 0);
    int saved = errno;
    if (debug && debug[0] != '\0' && strcmp(debug, "0") != 0) {
        fprintf(stderr,
                "semtier: mbind addr=%p len=%zu target_node=%d flags=%u "
                "rc=%ld errno=%d\n",
                addr, len, target, flags, rc, saved);
    }
    if (rc != 0) {
        errno = saved;
        return -1;
    }
    return 0;
#else
    (void)addr;
    (void)len;
    errno = ENOSYS;
    return -1;
#endif
}

int semtier_move_range_to_node(int pid, void *addr, size_t len, int node) {
    if (node < 0) {
        errno = EINVAL;
        return -1;
    }

#if defined(__linux__) && defined(__NR_move_pages)
    size_t page = semtier_page_size();
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)(page - 1u);
    uintptr_t end = semtier_align_up((uintptr_t)addr + len, page);
    size_t count = (end - start) / page;
    void **pages = calloc(count, sizeof(void *));
    int *nodes = calloc(count, sizeof(int));
    int *status = calloc(count, sizeof(int));
    if (!pages || !nodes || !status) {
        free(pages);
        free(nodes);
        free(status);
        errno = ENOMEM;
        return -1;
    }

    for (size_t i = 0; i < count; ++i) {
        pages[i] = (void *)(start + i * page);
        nodes[i] = node;
    }

    long rc = syscall(__NR_move_pages, pid, (unsigned long)count, pages, nodes,
                      status, 0);
    int saved = errno;
    free(pages);
    free(nodes);
    free(status);
    if (rc != 0) {
        errno = saved;
        return -1;
    }
    return 0;
#else
    (void)pid;
    (void)addr;
    (void)len;
    (void)node;
    errno = ENOSYS;
    return -1;
#endif
}
