#include "semtier.h"
#include "semtier_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *event_type_name(uint32_t type) {
    switch (type) {
    case SEMTIER_EVENT_REGION_CREATE:
        return "REGION_CREATE";
    case SEMTIER_EVENT_REGION_GROW:
        return "REGION_GROW";
    case SEMTIER_EVENT_REGION_DEAD:
        return "REGION_DEAD";
    case SEMTIER_EVENT_ALLOC_SAMPLE:
        return "ALLOC_SAMPLE";
    default:
        return "UNKNOWN";
    }
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --pid PID [--follow] [--apply-policy] "
            "[--fast-node N] [--slow-node N]\n"
            "       %s --name /semtier_PID [--follow] [--apply-policy]\n",
            argv0, argv0);
}

static int read_name_for_pid(int pid, char *buf, size_t len) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/semtier.%d.name", pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    if (!fgets(buf, (int)len, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int target_node_for_event(const struct semtier_event *event,
                                 int fast_node, int slow_node) {
    if (event->flags & (SEMTIER_HINT_SERIAL_DEP | SEMTIER_HINT_POINTER_CHASING |
                        SEMTIER_HINT_LATENCY_CRITICAL)) {
        return fast_node;
    }
    if (event->flags & (SEMTIER_HINT_STREAMING | SEMTIER_HINT_BACKGROUND)) {
        return slow_node;
    }
    return -1;
}

int main(int argc, char **argv) {
    int pid = -1;
    int follow = 0;
    int apply_policy = 0;
    int fast_node = -1;
    int slow_node = -1;
    char name[SEMTIER_RING_NAME_MAX] = {0};

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            snprintf(name, sizeof(name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--follow") == 0) {
            follow = 1;
        } else if (strcmp(argv[i], "--apply-policy") == 0) {
            apply_policy = 1;
        } else if (strcmp(argv[i], "--fast-node") == 0 && i + 1 < argc) {
            fast_node = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--slow-node") == 0 && i + 1 < argc) {
            slow_node = atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (name[0] == '\0') {
        if (pid <= 0 || read_name_for_pid(pid, name, sizeof(name)) != 0) {
            usage(argv[0]);
            return 2;
        }
    }

    struct semtier_ring *ring = NULL;
    if (semtier_ring_open(name, &ring) != 0) {
        fprintf(stderr, "failed to open ring %s: %s\n", name, strerror(errno));
        return 1;
    }

    uint64_t read_idx = 0;
    do {
        uint64_t write_idx = atomic_load_explicit(&ring->write_idx,
                                                  memory_order_acquire);
        if (write_idx > read_idx + SEMTIER_RING_CAPACITY) {
            read_idx = write_idx - SEMTIER_RING_CAPACITY;
        }

        while (read_idx < write_idx) {
            struct semtier_event event =
                ring->events[read_idx % SEMTIER_RING_CAPACITY];
            printf("{\"type\":\"%s\",\"pid\":%u,\"site_id\":%u,"
                   "\"ds_id\":%llu,\"addr\":\"0x%llx\",\"len\":%llu,"
                   "\"flags\":%u,\"timestamp_ns\":%llu",
                   event_type_name(event.type), event.pid, event.site_id,
                   (unsigned long long)event.ds_id,
                   (unsigned long long)event.addr,
                   (unsigned long long)event.len, event.flags,
                   (unsigned long long)event.timestamp_ns);

            if (apply_policy && event.type == SEMTIER_EVENT_REGION_CREATE) {
                int node = target_node_for_event(&event, fast_node, slow_node);
                if (node >= 0) {
                    int rc = semtier_move_range_to_node((int)event.pid,
                                                        (void *)(uintptr_t)event.addr,
                                                        (size_t)event.len, node);
                    printf(",\"move_node\":%d,\"move_rc\":%d", node, rc);
                    if (rc != 0) {
                        printf(",\"move_errno\":%d", errno);
                    }
                }
            }
            printf("}\n");
            fflush(stdout);
            read_idx++;
        }

        if (follow) {
            usleep(10000);
        }
    } while (follow);

    semtier_ring_unmap(ring);
    return 0;
}
