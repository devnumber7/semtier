#include "semtier.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SITE_LIST_NODE 1001u
#define SITE_STREAM_ARRAY 2001u

struct node {
    uint64_t value;
    struct node *next;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static void shuffle(size_t *values, size_t n) {
    uint64_t state = 0x9e3779b97f4a7c15ull;
    for (size_t i = n - 1; i > 0; --i) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        size_t j = (size_t)((state * 2685821657736338717ull) % (i + 1u));
        size_t tmp = values[i];
        values[i] = values[j];
        values[j] = tmp;
    }
}

static struct node *build_list(size_t n, int use_semtier) {
    size_t *perm = malloc(n * sizeof(size_t));
    struct node **nodes = malloc(n * sizeof(struct node *));
    if (!perm || !nodes) {
        fprintf(stderr, "allocation failure\n");
        exit(1);
    }

    for (size_t i = 0; i < n; ++i) {
        perm[i] = i;
    }
    shuffle(perm, n);

    if (use_semtier) {
        semtier_region_begin(SITE_LIST_NODE,
                             SEMTIER_HINT_SERIAL_DEP |
                                 SEMTIER_HINT_POINTER_CHASING |
                                 SEMTIER_HINT_LATENCY_CRITICAL);
    }

    for (size_t i = 0; i < n; ++i) {
        if (use_semtier) {
            nodes[i] = semtier_alloc(sizeof(struct node), SITE_LIST_NODE,
                                     SEMTIER_HINT_OWNED);
        } else {
            nodes[i] = malloc(sizeof(struct node));
        }
        if (!nodes[i]) {
            fprintf(stderr, "node allocation failure\n");
            exit(1);
        }
        nodes[i]->value = i;
        nodes[i]->next = NULL;
    }

    if (use_semtier) {
        semtier_region_end();
    }

    for (size_t i = 0; i + 1 < n; ++i) {
        nodes[perm[i]]->next = nodes[perm[i + 1u]];
    }
    nodes[perm[n - 1u]]->next = NULL;
    struct node *head = nodes[perm[0]];
    free(perm);
    free(nodes);
    return head;
}

static uint64_t chase(struct node *head, size_t iters) {
    uint64_t sum = 0;
    for (size_t r = 0; r < iters; ++r) {
        for (struct node *cur = head; cur; cur = cur->next) {
            sum += cur->value;
        }
    }
    return sum;
}

static uint64_t *build_stream_array(size_t n, int use_semtier) {
    uint64_t *data = use_semtier
                         ? semtier_alloc(n * sizeof(uint64_t), SITE_STREAM_ARRAY,
                                         SEMTIER_HINT_STREAMING)
                         : malloc(n * sizeof(uint64_t));
    if (!data) {
        fprintf(stderr, "stream allocation failure\n");
        exit(1);
    }

    for (size_t i = 0; i < n; ++i) {
        data[i] = i;
    }
    return data;
}

static uint64_t stream(uint64_t *data, size_t n, size_t iters) {
    uint64_t sum = 0;
    for (size_t r = 0; r < iters; ++r) {
        for (size_t i = 0; i < n; ++i) {
            sum += data[i];
        }
    }
    return sum;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--mode malloc|semtier] [--bench chase|stream] "
            "[--nodes N] [--iters N] [--sleep-before-shutdown SEC]\n",
            argv0);
}

int main(int argc, char **argv) {
    int use_semtier = 0;
    int run_stream = 0;
    size_t nodes = 1000000u;
    size_t iters = 4u;
    unsigned sleep_before_shutdown = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "semtier") == 0) {
                use_semtier = 1;
            } else if (strcmp(mode, "malloc") == 0) {
                use_semtier = 0;
            } else {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            const char *bench = argv[++i];
            if (strcmp(bench, "stream") == 0) {
                run_stream = 1;
            } else if (strcmp(bench, "chase") == 0) {
                run_stream = 0;
            } else {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--nodes") == 0 && i + 1 < argc) {
            nodes = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) {
            iters = strtoull(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--sleep-before-shutdown") == 0 && i + 1 < argc) {
            sleep_before_shutdown = (unsigned)strtoul(argv[++i], NULL, 10);
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (use_semtier) {
        semtier_init();
        printf("semtier_ring=%s\n", semtier_ring_name());
    }

    uint64_t build_start = now_ns();
    uint64_t sum;
    uint64_t build_end;
    uint64_t run_start;
    uint64_t run_end;

    if (run_stream) {
        uint64_t *data = build_stream_array(nodes, use_semtier);
        build_end = now_ns();
        run_start = now_ns();
        sum = stream(data, nodes, iters);
        run_end = now_ns();
        if (!use_semtier) {
            free(data);
        }
    } else {
        struct node *head = build_list(nodes, use_semtier);
        build_end = now_ns();
        run_start = now_ns();
        sum = chase(head, iters);
        run_end = now_ns();
    }

    printf("mode=%s bench=%s nodes=%zu iters=%zu total_ns=%" PRIu64
           " build_ns=%" PRIu64 " run_ns=%" PRIu64 " checksum=%" PRIu64 "\n",
           use_semtier ? "semtier" : "malloc", run_stream ? "stream" : "chase",
           nodes, iters, run_end - build_start, build_end - build_start,
           run_end - run_start, sum);
    fflush(stdout);

    if (sleep_before_shutdown > 0) {
        sleep(sleep_before_shutdown);
    }

    if (use_semtier) {
        semtier_flush();
        semtier_shutdown();
    }
    return 0;
}
