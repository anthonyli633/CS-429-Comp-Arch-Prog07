#define _POSIX_C_SOURCE 200809L

#include "tdmm.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#ifndef MiB
#define MiB (1024u * 1024u)
#endif

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}
static const char *policy_name(alloc_strat_e s) {
    switch (s) {
        case FIRST_FIT: return "FIRST_FIT";
        case BEST_FIT:  return "BEST_FIT";
        case WORST_FIT: return "WORST_FIT";
		case BUDDY:    return "BUDDY";
		case MIXED:    return "MIXED";
        default:        return "UNKNOWN";
    }
}

typedef struct {
    size_t bytes_from_os;
    size_t cur_inuse_bytes;
    size_t peak_inuse_bytes;
    double util_sum;
    size_t num_util;
} tdmm_metrics_t;

extern const tdmm_metrics_t *t_metrics_ptr(void);
extern size_t t_overhead_bytes(void);

static FILE *open_csv_or_die(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(1);
    }
    setvbuf(f, NULL, _IOFBF, 1 << 20);
    return f;
}

static void run_util_trace_to_csv(alloc_strat_e strat) {
    const size_t N = 4000;
    const size_t M = 2000;
    const size_t R = 5;
    const size_t MIN_SZ = 16 * 16;
    const size_t MAX_SZ = 4096 * 16;

    char path[128];
    snprintf(path, sizeof(path), "util_trace_%s.csv", policy_name(strat));
    FILE *out = open_csv_or_die(path);

    fprintf(out, "policy,event,op,req_bytes,utilization,cur_inuse_bytes,overhead_bytes\n");

    void **ptrs = (void **)calloc(N + M, sizeof(void *));
    if (!ptrs) {
        fprintf(stderr, "calloc failed\n");
        fclose(out);
        return;
    }

    uint32_t rng = 0xC0FFEEu;

    t_reset();
    t_init(strat);
    uint64_t event = 0;
    size_t overhead_peak = 0;

    for (size_t round = 0; round < R; round++) {
        // Phase 1: allocate N
        for (size_t i = 0; i < N; i++) {
            size_t sz = MIN_SZ + (xorshift32(&rng) % (MAX_SZ - MIN_SZ + 1));
            ptrs[i] = t_malloc(sz);

            const tdmm_metrics_t *m = t_metrics_ptr();
            double u = (m->bytes_from_os ? (double)m->cur_inuse_bytes / (double)m->bytes_from_os : 0.0);
            size_t oh = t_overhead_bytes();
            if (oh > overhead_peak) overhead_peak = oh;
            
            if (i % 100 == 0) {
                fprintf(out, "%s,%llu,malloc,%zu,%.10f,%zu,%zu\n",
                        policy_name(strat), (unsigned long long)event++, sz, u, m->cur_inuse_bytes, oh);
            }
        }

        // Phase 2: free every other
        for (size_t i = 0; i < N; i += 2) {
            if (!ptrs[i]) continue;
            t_free(ptrs[i]);
            ptrs[i] = NULL;

            const tdmm_metrics_t *m = t_metrics_ptr();
            double u = (m->bytes_from_os ? (double)m->cur_inuse_bytes / (double)m->bytes_from_os : 0.0);
            size_t oh = t_overhead_bytes();
            if (oh > overhead_peak) overhead_peak = oh;
            
            if (i % 100 == 0) {
                fprintf(out, "%s,%llu,free,0,%.10f,%zu,%zu\n",
                        policy_name(strat), (unsigned long long)event++, u, m->cur_inuse_bytes, oh);
            }
        }

        // Phase 3: allocate M more
        for (size_t j = 0; j < M; j++) {
            size_t sz = MIN_SZ + (xorshift32(&rng) % (MAX_SZ - MIN_SZ + 1));
            ptrs[N + j] = t_malloc(sz);

            const tdmm_metrics_t *m = t_metrics_ptr();
            double u = (m->bytes_from_os ? (double)m->cur_inuse_bytes / (double)m->bytes_from_os : 0.0);
            size_t oh = t_overhead_bytes();
            if (oh > overhead_peak) overhead_peak = oh;
            
            if (j % 100 == 0) {
                fprintf(out, "%s,%llu,malloc,%zu,%.10f,%zu,%zu\n",
                        policy_name(strat), (unsigned long long)event++, sz, u, m->cur_inuse_bytes, oh);
            }
        }

        // Phase 4: free all remaining
        for (size_t i = 0; i < N + M; i++) {
            if (!ptrs[i]) continue;
            t_free(ptrs[i]);
            ptrs[i] = NULL;

            const tdmm_metrics_t *m = t_metrics_ptr();
            double u = (m->bytes_from_os ? (double)m->cur_inuse_bytes / (double)m->bytes_from_os : 0.0);
            size_t oh = t_overhead_bytes();
            if (oh > overhead_peak) overhead_peak = oh;
            
            if (i % 100 == 0) {
                fprintf(out, "%s,%llu,free,0,%.10f,%zu,%zu\n",
                        policy_name(strat), (unsigned long long)event++, u, m->cur_inuse_bytes, oh);
            }
        }
    }

    const tdmm_metrics_t *m = t_metrics_ptr();
    double avg_u = (m->num_util ? (m->util_sum / (double)m->num_util) : 0.0);
    double peak_u = (m->bytes_from_os ? (double)m->peak_inuse_bytes / (double)m->bytes_from_os : 0.0);
    size_t oh_end = t_overhead_bytes();

    fprintf(out, "SUMMARY,0,avg_util,0,%.10f,0,0\n", avg_u);
    fprintf(out, "SUMMARY,0,peak_util,0,%.10f,0,0\n", peak_u);
    fprintf(out, "SUMMARY,0,os_bytes,0,0.0,0,%zu\n", m->bytes_from_os);
    fprintf(out, "SUMMARY,0,samples,0,0.0,%zu,0\n", m->num_util);
    fprintf(out, "SUMMARY,0,overhead_end,0,0.0,0,%zu\n", oh_end);
    fprintf(out, "SUMMARY,0,overhead_peak,0,0.0,0,%zu\n", overhead_peak);

    free(ptrs);
    fclose(out);
}

static void run_speed_curve_to_csv(alloc_strat_e strat) {
    char path[128];
    snprintf(path, sizeof(path), "speed_%s.csv", policy_name(strat));
    FILE *out = open_csv_or_die(path);

    fprintf(out, "policy,size_bytes,iters,avg_malloc_ns,avg_free_ns,overhead_bytes\n");
    t_reset();
    t_init(strat);

    for (int k = 0; k <= 23; k++) {
        size_t sz = (size_t)1u << k;
        uint64_t iters = (sz <= 1024) ? 200000 :
                         (sz <= 64 * 1024) ? 50000 :
                         (sz <= 1 * MiB) ? 5000 : 800;

        for (int w = 0; w < 100; w++) {
            void *p = t_malloc(sz);
            if (p) t_free(p);
        }

        uint64_t malloc_sum = 0;
        uint64_t free_sum = 0;

        for (uint64_t i = 0; i < iters; i++) {
            uint64_t a0 = now_ns();
            void *p = t_malloc(sz);
            uint64_t a1 = now_ns();

            uint64_t f0 = now_ns();
            if (p) t_free(p);
            uint64_t f1 = now_ns();

            malloc_sum += (a1 - a0);
            free_sum += (f1 - f0);
        }

        double avg_m = iters ? (double)malloc_sum / (double)iters : 0.0;
        double avg_f = iters ? (double)free_sum / (double)iters : 0.0;
        size_t oh = t_overhead_bytes();

        fprintf(out, "%s,%zu,%llu,%.4f,%.4f,%zu\n",
                policy_name(strat), sz, (unsigned long long)iters, avg_m, avg_f, oh);
    }

    fclose(out);
}

static void run_program_runtime_to_csv(alloc_strat_e strat) {
    const size_t OPS = 300000;
    const size_t LIVE = 20000;
    const size_t MIN_SZ = 8;
    const size_t MAX_SZ = 8192;

    char path[128];
    snprintf(path, sizeof(path), "runtime_%s.csv", policy_name(strat));
    FILE *out = open_csv_or_die(path);

    fprintf(out, "policy,total_runtime_ns,avg_util,peak_util,os_bytes,samples,overhead_end,overhead_peak\n");

    void **live = (void **)calloc(LIVE, sizeof(void *));
    if (!live) {
        fprintf(stderr, "calloc failed\n");
        fclose(out);
        return;
    }

    uint32_t rng = 0xBADC0DEu;

    t_reset();
    t_init(strat);
    size_t overhead_peak = 0;
    uint64_t start = now_ns();

    for (size_t op = 0; op < OPS; op++) {
        uint32_t r = xorshift32(&rng);
        size_t idx = (size_t)(r % LIVE);

        if (live[idx] && (r & 1u)) {
            t_free(live[idx]);
            live[idx] = NULL;
        } else {
            size_t sz = MIN_SZ + (xorshift32(&rng) % (MAX_SZ - MIN_SZ + 1));
            live[idx] = t_malloc(sz);
        }
        if ((op & 255u) == 0u) {
            size_t oh = t_overhead_bytes();
            if (oh > overhead_peak) overhead_peak = oh;
        }
    }

    for (size_t i = 0; i < LIVE; i++) {
        if (live[i]) t_free(live[i]);
    }

    uint64_t end = now_ns();
    uint64_t total = end - start;

    const tdmm_metrics_t *m = t_metrics_ptr();
    double avg_u = (m->num_util ? (m->util_sum / (double)m->num_util) : 0.0);
    double peak_u = (m->bytes_from_os ? (double)m->peak_inuse_bytes / (double)m->bytes_from_os : 0.0);

    size_t overhead_end = t_overhead_bytes();
    if (overhead_end > overhead_peak) overhead_peak = overhead_end;

    fprintf(out, "%s,%llu,%.10f,%.10f,%zu,%zu,%zu,%zu\n",
            policy_name(strat),
            (unsigned long long)total,
            avg_u,
            peak_u,
            m->bytes_from_os,
            m->num_util,
            overhead_end,
            overhead_peak);

    free(live);
    fclose(out);
}

int main(void) {
    alloc_strat_e policies[5] = { FIRST_FIT, BEST_FIT, WORST_FIT, BUDDY, MIXED };

    for (int i = 0; i < 5; i++) run_util_trace_to_csv(policies[i]);
    for (int i = 0; i < 5; i++) run_program_runtime_to_csv(policies[i]);
    for (int i = 0; i < 5; i++) run_speed_curve_to_csv(policies[i]);

    printf("Wrote CSVs: util_trace_*.csv, runtime_*.csv, speed_*.csv\n");
    return 0;
}