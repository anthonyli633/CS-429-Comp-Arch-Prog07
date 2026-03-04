#include "tdmm.h"

#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdio.h>

#define TDMM_HEAP_BYTES (64u * 1024u * 1024u)
#define max(a, b) ((a) > (b) ? (a) : (b))
#define MAX_ORDER 26  // 1 << 26 = 64 MiB


typedef struct block_hdr {
    size_t size;
    uint8_t free;
    uint8_t _pad[3];  // ensures 4-byte alignment
    struct block_hdr *prev;
    struct block_hdr *next;
} block_hdr_t;

typedef struct {
    // OS memory
    size_t bytes_from_os;
    // Memory usage
    size_t cur_inuse_bytes;
    size_t peak_inuse_bytes;
    // Utilization
    double util_sum;
    size_t num_util;
} tdmm_metrics_t;

static int mixed = 0;
static void *g_heap_base = NULL;
static size_t g_heap_size = 0;
static block_hdr_t *g_head = NULL;
static alloc_strat_e g_strat = FIRST_FIT;
static tdmm_metrics_t g_metrics = {0};

static block_hdr_t *free_lists[MAX_ORDER + 1] = {0};
static int num_buddy_blocks = 0;

static inline void push(int order, block_hdr_t *blk) {
    blk->free = 1;
    blk->prev = NULL;
    blk->next = free_lists[order];
    free_lists[order] = blk;
}
static inline block_hdr_t *pop(int order) {
    block_hdr_t *h = free_lists[order];
    if (!h) return NULL;
    free_lists[order] = h->next;
    h->next = NULL;
    return h;
}
static int remove_(int order, block_hdr_t *target) {
    block_hdr_t **pp = &free_lists[order];
    while (*pp) {
        if (*pp == target) {
            *pp = (*pp)->next;
            target->next = NULL;
            return 1;
        }
        pp = &((*pp)->next);
    }
    return 0;
}

const tdmm_metrics_t *t_metrics_ptr(void) {
    return &g_metrics;
}

static size_t ALIGN4(size_t x) {
    return (x + 3) / 4 * 4;
}
static size_t page_round_up(size_t n) {
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    size_t p = (size_t)ps;
    return (n + p - 1) / p * p;
}
static size_t hdr_size(void) {
    return ALIGN4(sizeof(block_hdr_t));
}

static void *payload_from_hdr(block_hdr_t *h) {
    return (void *)((uint8_t *)h + hdr_size());
}

static block_hdr_t *hdr_from_payload(void *p) {
    return (block_hdr_t *)((uint8_t *)p - hdr_size());
}

static int ptr_in_heap(const void *p) {
    if (!g_heap_base || g_heap_size == 0) return 0;
    uintptr_t x = (uintptr_t)p;
    uintptr_t b = (uintptr_t)g_heap_base;
    return (x >= b) && (x < b + g_heap_size);
}

static void merge(block_hdr_t *b) {
    if (!b) return;
    while (b->prev && b->prev->free) b = b->prev;
    while (b->next && b->next->free) {
        block_hdr_t *n = b->next;
        b->size += hdr_size() + n->size;
        b->next = n->next;
        if (b->next) b->next->prev = b;
    }
}

static block_hdr_t *find_block(size_t need) {
    block_hdr_t *choice = NULL;
    if (g_strat == MIXED) { mixed = (mixed + 1) % 3; }
    if (g_strat == FIRST_FIT || g_strat == MIXED && mixed == 0) {
        for (block_hdr_t *cur = g_head; cur; cur = cur->next) {
            if (cur->free && cur->size >= need) return cur;
        }
        return NULL;
    }
    if (g_strat == BEST_FIT || g_strat == MIXED && mixed == 1) {
        for (block_hdr_t *cur = g_head; cur; cur = cur->next) {
            if (!cur->free || cur->size < need) continue;
            if (!choice || cur->size < choice->size) choice = cur;
        }
        return choice;
    }
    if (g_strat == WORST_FIT || g_strat == MIXED && mixed == 2) {
        for (block_hdr_t *cur = g_head; cur; cur = cur->next) {
            if (!cur->free || cur->size < need) continue;
            if (!choice || cur->size > choice->size) choice = cur;
        }
        return choice;
    }
    return NULL;
}

static void split_block(block_hdr_t *b, size_t need) {
    if (!b || b->size < need) return;

    size_t hsz = hdr_size();
    size_t remaining = b->size - need;
    if (remaining < hsz + 4) return;

    uint8_t *new_addr = (uint8_t *)payload_from_hdr(b) + need;
    block_hdr_t *n = (block_hdr_t *)new_addr;

    n->size = remaining - hsz;
    n->free = 1;
    n->prev = b;
    n->next = b->next;

    if (b->next) b->next->prev = n;
    b->next = n;
    b->size = need;
}

typedef enum {
    METRIC_INIT = 0,
    METRIC_MALLOC,
    METRIC_FREE,
} metric_event_t;

size_t t_overhead_bytes(void) {
    if (g_strat == BUDDY) {
        return num_buddy_blocks * hdr_size();
    }
    size_t blocks = 0;
    for (block_hdr_t *cur = g_head; cur; cur = cur->next) {
        blocks++;
    }
    return blocks * hdr_size();
}

static void update_metrics(metric_event_t ev, size_t req_bytes, size_t actual_bytes) {
    g_metrics.bytes_from_os = g_heap_size;
    if (ev == METRIC_MALLOC) {
        g_metrics.cur_inuse_bytes += actual_bytes;
        g_metrics.peak_inuse_bytes = max(g_metrics.peak_inuse_bytes, g_metrics.cur_inuse_bytes);
    } else if (ev == METRIC_FREE) {
        g_metrics.cur_inuse_bytes = max(0, (ssize_t)g_metrics.cur_inuse_bytes - (ssize_t)actual_bytes);
    }

    if (g_metrics.bytes_from_os > 0) {
        double u = (double)g_metrics.cur_inuse_bytes / (double)g_metrics.bytes_from_os;
        g_metrics.util_sum += u;
        g_metrics.num_util += 1;
    }
}

static block_hdr_t *buddy(block_hdr_t *b, int order) {
    uintptr_t base = (uintptr_t)g_heap_base;
    uintptr_t addr = (uintptr_t)b;
    size_t size = 1ULL << order;
    return (block_hdr_t *)(base + ((addr - base) ^ size));
}
static int size_to_order_(size_t bytes) {
    int o = 0;
    size_t s = 1;
    while (s < bytes) { s <<= 1; o++; }
    return o;
}
static int size_to_order(size_t bytes) {
    return max(size_to_order_(bytes), size_to_order_(hdr_size() + 4));
}

void t_init(alloc_strat_e strat) {
    g_strat = strat;
	if (g_strat == MIXED) { mixed = 2; }

    size_t req = (size_t) 1 << MAX_ORDER;
    void *mem = mmap(NULL, req, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        g_heap_base = NULL;
        g_heap_size = 0;
        g_head = NULL;
        return;
    }

    g_heap_base = mem;
    g_heap_size = req;

    if (g_strat == BUDDY) {
        for (int i = 0; i <= MAX_ORDER; i++) free_lists[i] = NULL;
        block_hdr_t *b = (block_hdr_t *)g_heap_base;
        b->size = (size_t)1 << MAX_ORDER;
        b->free = 1;
        b->prev = NULL;
        b->next = NULL;
        num_buddy_blocks = 1;
        push(MAX_ORDER, b);
    } else {
        g_head = (block_hdr_t *)g_heap_base;
        g_head->size = g_heap_size - hdr_size();
        g_head->free = 1;
        g_head->prev = NULL;
        g_head->next = NULL;
    }

    g_metrics = (tdmm_metrics_t){0};
    update_metrics(METRIC_INIT, 0, 0);
}

void *buddy_malloc(size_t size) {
    int order = size_to_order(size + hdr_size());
    int o = order;
    while (o <= MAX_ORDER && !free_lists[o]) o++;
    if (o > MAX_ORDER) return NULL;
    block_hdr_t *b = pop(o);

    while (o > order) {
        o--;
        block_hdr_t *right = (block_hdr_t *)((uint8_t *)b + (1ULL << o));
        b->size = right->size = (size_t)1ULL << o;
        right->free = 1;
        push(o, right);
        num_buddy_blocks++;
    }

    b->free = 0;
    size_t actual = b->size - hdr_size();
    update_metrics(METRIC_MALLOC, size, actual);
    return payload_from_hdr(b);
}
void *t_malloc(size_t size) {
    if (size == 0) { update_metrics(METRIC_MALLOC, 0, 0); return NULL; }
    if (!g_heap_base) t_init(g_strat);
    if (!g_heap_base) { update_metrics(METRIC_MALLOC, size, 0); return NULL; }
    if (g_strat == BUDDY) return buddy_malloc(size);

    size_t need = ALIGN4(size);
    block_hdr_t *b = find_block(need);
    if (!b) { update_metrics(METRIC_MALLOC, size, 0); return NULL; }

    split_block(b, need);
    b->free = 0;

    void *p = payload_from_hdr(b);
    if ((uintptr_t)p % 4 != 0) { update_metrics(METRIC_MALLOC, size, 0); return NULL; }

    update_metrics(METRIC_MALLOC, size, need);
    return p;
}

void buddy_free(void *ptr) {
    block_hdr_t *b = hdr_from_payload(ptr);
    int order = size_to_order(b->size);
    size_t actual = b->size - hdr_size();

    b->free = 1;
    while (order < MAX_ORDER) {
        block_hdr_t *bud = buddy(b, order);
        if (!remove_(order, bud)) break;
        if (bud < b) b = bud;
        order++; num_buddy_blocks--;
        b->size = 1ULL << order;
    }
    push(order, b);
    update_metrics(METRIC_FREE, 0, actual);
}
void t_free(void *ptr) {
    if (!ptr) { update_metrics(METRIC_FREE, 0, 0); return; }
    if (!ptr_in_heap(ptr)) { update_metrics(METRIC_FREE, 0, 0); return; }

    block_hdr_t *b = hdr_from_payload(ptr);
    if (!ptr_in_heap(b)) { update_metrics(METRIC_FREE, 0, 0); return; }
    if (b->free) { update_metrics(METRIC_FREE, 0, 0); return; }

    if (g_strat == BUDDY) { buddy_free(ptr); return; }

    size_t freed = b->size;
    b->free = 1;
    merge(b);
    update_metrics(METRIC_FREE, 0, freed);
}