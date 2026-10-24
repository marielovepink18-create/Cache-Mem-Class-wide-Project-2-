/* bw.c - sustained bandwidth with controllable pattern, granularity, stride, read/write mix, threads.
 *
 * Each thread owns a private buffer of `size` bytes (total footprint = threads*size) and repeatedly visits
 * blocks of `gran` contiguous bytes located `stride` bytes apart:
 *   pattern=seq   blocks visited in address order (wrapping)
 *   pattern=rand  block start drawn from a 64-bit LCG (uniform, with replacement; no index array so the
 *                 benchmark does not generate extra memory traffic of its own)
 * Read/write mix is per block: rw = percentage of blocks that are read; the rest are written.
 *   nt=1 uses non-temporal stores for written blocks (no read-for-ownership).
 * Reported bandwidth counts only the bytes the kernel touches (gran per block), not the hardware's
 * extra traffic (e.g. the read-for-ownership of normal stores, or unused bytes of a 64B line at stride>gran).
 * One warm-up window is run and discarded.
 *
 *   size=512M threads=1 cpus=2,4,6 pattern=seq gran=64 stride=64 rw=100 nt=0 huge=0 reps=5 ms=300 tag= -H
 */
#include "common.h"
#include "kern.h"

typedef struct {
    int id, cpu, huge, nt, rdtens, rnd, reps, fail, huge_ok;
    size_t size, gran, stride;
    sync_t *s;
    double *bw;      /* GB/s per window (index 0 = warm-up) */
    uint64_t sink;
} targ;

static void worker(void *vp) {
    targ *a = (targ *)vp;
    setup_thread(a->cpu);
    mem_t m = mem_alloc(a->size, a->huge);
    char *base = (char *)m.p;
    size_t nb = 0, nb2 = 2;
    int shift = 63;
    if (!m.p) {
        a->fail = 1;
    } else {
        uint64_t *w = (uint64_t *)base;
        for (size_t i = 0; i < a->size / 8; i++) w[i] = i * 2654435761u + 1; /* write => physical pages exist, data non-zero */
        nb = a->size / a->stride;
        while (nb2 * 2 <= nb) nb2 *= 2;
        int lg = 0;
        while (((size_t)1 << lg) < nb2) lg++;
        shift = 64 - lg;
        a->huge_ok = mem_huge_verified(&m);
    }
    sync_ready(a->s);

    size_t nwb = a->gran / 8;
    uint64_t acc = 0, x = 0x9E3779B97F4A7C15ULL * (uint64_t)(a->id + 1), cnt = 0;
    size_t j = 0;
    for (int g = 1; g <= a->reps + 1; g++) {
        sync_wait_gen(a->s, g);
        if (a->fail) {
            while (sync_running(a->s, g)) sleep_ms(1);
            a->bw[g - 1] = -1;
            sync_done(a->s);
            continue;
        }
        uint64_t blocks = 0;
        double t0 = now_sec();
        while (sync_running(a->s, g)) {
            for (int u = 0; u < 256; u++) {
                size_t idx;
                if (a->rnd) { x = x * 6364136223846793005ULL + 1442695040888963407ULL; idx = (size_t)(x >> shift); }
                else { idx = j; if (++j == nb) j = 0; }
                uint64_t *blk = (uint64_t *)(base + idx * a->stride);
                int wr;
                if (a->rdtens >= 10) wr = 0;
                else if (a->rdtens <= 0) wr = 1;
                else wr = (int)((cnt++ % 10) >= (uint64_t)a->rdtens);
                if (wr) { if (a->nt) nt_block(blk, nwb, x + idx); else wr_block(blk, nwb, x + idx); }
                else acc += rd_block(blk, nwb);
            }
            blocks += 256;
        }
        store_fence();
        double t1 = now_sec();
        a->bw[g - 1] = (double)blocks * (double)a->gran / (t1 - t0) / 1e9;
        sync_done(a->s);
    }
    a->sink = acc;
    mem_free(&m);
}

int main(int argc, char **argv) {
    size_t size = arg_size(argc, argv, "size", "512M");
    size_t gran = arg_size(argc, argv, "gran", "64");
    size_t stride = arg_size(argc, argv, "stride", "0");
    int T = (int)arg_long(argc, argv, "threads", 1);
    int rw = (int)arg_long(argc, argv, "rw", 100);
    int nt = (int)arg_long(argc, argv, "nt", 0);
    int huge = (int)arg_long(argc, argv, "huge", 0);
    int reps = (int)arg_long(argc, argv, "reps", 5);
    int ms = (int)arg_long(argc, argv, "ms", 300);
    const char *pat = arg_str(argc, argv, "pattern", "seq");
    const char *tag = arg_str(argc, argv, "tag", "");
    int cpus[256], ncpu = parse_cpu_list(arg_str(argc, argv, "cpus", ""), cpus, 256);
    int rnd = strcmp(pat, "rand") == 0;

    gran = (gran + 63) & ~(size_t)63;
    if (stride < gran) stride = gran;
    stride = (stride + 63) & ~(size_t)63;
    if (size < 2 * stride) { fprintf(stderr, "size must be >= 2*stride\n"); return 1; }
    if (T < 1) T = 1;
    if (T > 256) T = 256;
    if (ncpu && ncpu < T) fprintf(stderr, "warning: %d cpus for %d threads; extra threads unpinned\n", ncpu, T);

    sync_t S;
    memset(&S, 0, sizeof S);
    targ *A = (targ *)calloc((size_t)T, sizeof(targ));
    thr_t *H = (thr_t *)calloc((size_t)T, sizeof(thr_t));
    for (int i = 0; i < T; i++) {
        A[i].id = i; A[i].cpu = (i < ncpu) ? cpus[i] : -1; A[i].huge = huge; A[i].nt = nt;
        A[i].rdtens = rw / 10; A[i].rnd = rnd; A[i].reps = reps; A[i].size = size;
        A[i].gran = gran; A[i].stride = stride; A[i].s = &S;
        A[i].bw = (double *)calloc((size_t)reps + 1, sizeof(double));
        thr_start(&H[i], worker, &A[i]);
    }
    sync_wait_ready(&S, T);
    for (int g = 1; g <= reps + 1; g++) sync_window(&S, g, T, ms);
    for (int i = 0; i < T; i++) thr_join(&H[i]);

    int failed = 0;
    for (int i = 0; i < T; i++) failed |= A[i].fail;
    if (failed) { fprintf(stderr, "allocation failure (%d x %zu bytes)\n", T, size); return 1; }
    if (arg_has(argc, argv, "-H"))
        printf("tool,tag,size_bytes,threads,pattern,gran,stride,rw,nt,huge_req,huge_ok,rep,gbps_total,gbps_per_thread\n");
    for (int r = 1; r <= reps; r++) {
        double tot = 0;
        for (int i = 0; i < T; i++) tot += A[i].bw[r];
        printf("bw,%s,%zu,%d,%s,%zu,%zu,%d,%d,%d,%d,%d,%.4f,%.4f\n", tag, size, T, rnd ? "rand" : "seq", gran, stride, rw,
               nt, huge, A[0].huge_ok, r - 1, tot, tot / T);
    }
    return 0;
}
