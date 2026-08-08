#include "semtier_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int is_shm_name(const char *name) {
    return name && strncmp(name, "/semtier_", 9) == 0;
}

static int create_file_backed_ring(char *name_buf, size_t name_buf_len) {
    snprintf(name_buf, name_buf_len, "/tmp/semtier.%ld.ring", (long)getpid());
    return open(name_buf, O_CREAT | O_TRUNC | O_RDWR, 0600);
}

uint64_t semtier_now_ns(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
#else
    return 0;
#endif
}

size_t semtier_page_size(void) {
    long page = sysconf(_SC_PAGESIZE);
    return page > 0 ? (size_t)page : 4096u;
}

size_t semtier_align_up(size_t value, size_t align) {
    if (align == 0) {
        return value;
    }
    return (value + align - 1u) & ~(align - 1u);
}

int semtier_ring_init_for_process(char *name_buf, size_t name_buf_len,
                                  struct semtier_ring **ring_out) {
    if (!name_buf || name_buf_len == 0 || !ring_out) {
        errno = EINVAL;
        return -1;
    }

    snprintf(name_buf, name_buf_len, "/semtier_%ld", (long)getpid());

    int fd = shm_open(name_buf, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 && errno == EEXIST) {
        shm_unlink(name_buf);
        fd = shm_open(name_buf, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if (fd < 0) {
        fd = create_file_backed_ring(name_buf, name_buf_len);
        if (fd < 0) {
            return -1;
        }
    }

    size_t len = sizeof(struct semtier_ring);
    if (ftruncate(fd, (off_t)len) != 0) {
        int saved = errno;
        close(fd);
        if (is_shm_name(name_buf)) {
            shm_unlink(name_buf);
        } else {
            unlink(name_buf);
        }
        errno = saved;
        return -1;
    }

    void *mapped = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (mapped == MAP_FAILED) {
        if (is_shm_name(name_buf)) {
            shm_unlink(name_buf);
        } else {
            unlink(name_buf);
        }
        errno = saved;
        return -1;
    }

    struct semtier_ring *ring = (struct semtier_ring *)mapped;
    memset(ring, 0, sizeof(*ring));
    ring->magic = SEMTIER_RING_MAGIC;
    ring->capacity = SEMTIER_RING_CAPACITY;
    ring->event_size = sizeof(struct semtier_event);
    atomic_store_explicit(&ring->write_idx, 0, memory_order_release);
    atomic_store_explicit(&ring->dropped, 0, memory_order_release);

    char desc_path[PATH_MAX];
    snprintf(desc_path, sizeof(desc_path), "/tmp/semtier.%ld.name", (long)getpid());
    FILE *desc = fopen(desc_path, "w");
    if (desc) {
        fprintf(desc, "%s\n", name_buf);
        fclose(desc);
    }

    *ring_out = ring;
    return 0;
}

void semtier_ring_close(const char *name, struct semtier_ring *ring) {
    if (ring) {
        munmap(ring, sizeof(struct semtier_ring));
    }
    if (name && name[0] != '\0') {
        if (is_shm_name(name)) {
            shm_unlink(name);
        } else {
            unlink(name);
        }
    }
}

int semtier_ring_open(const char *name, struct semtier_ring **ring_out) {
    if (!name || !ring_out) {
        errno = EINVAL;
        return -1;
    }

    int fd = is_shm_name(name) ? shm_open(name, O_RDWR, 0600)
                               : open(name, O_RDWR);
    if (fd < 0) {
        return -1;
    }

    void *mapped = mmap(NULL, sizeof(struct semtier_ring), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (mapped == MAP_FAILED) {
        errno = saved;
        return -1;
    }

    struct semtier_ring *ring = (struct semtier_ring *)mapped;
    if (ring->magic != SEMTIER_RING_MAGIC ||
        ring->capacity != SEMTIER_RING_CAPACITY ||
        ring->event_size != sizeof(struct semtier_event)) {
        munmap(ring, sizeof(struct semtier_ring));
        errno = EINVAL;
        return -1;
    }

    *ring_out = ring;
    return 0;
}

void semtier_ring_unmap(struct semtier_ring *ring) {
    if (ring) {
        munmap(ring, sizeof(struct semtier_ring));
    }
}

void semtier_ring_write(struct semtier_ring *ring,
                        const struct semtier_event *event) {
    if (!ring || !event) {
        return;
    }

    uint64_t idx = atomic_fetch_add_explicit(&ring->write_idx, 1,
                                             memory_order_acq_rel);
    ring->events[idx % SEMTIER_RING_CAPACITY] = *event;
    if (idx >= SEMTIER_RING_CAPACITY) {
        atomic_fetch_add_explicit(&ring->dropped, 1, memory_order_relaxed);
    }
}
