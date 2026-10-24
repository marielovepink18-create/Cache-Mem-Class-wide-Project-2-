/* loaded.c - loaded-latency / throughput-latency sweep (MLC-style).
 *
 * One latency thread does a random pointer chase (1 outstanding miss) through its own large buffer while
 * `threads` load-generator threads stream through private buffers. The load generators are throttled by
 * `delay`: a busy loop of ~1 core cycle per iteration executed after every 64-byte line they touch.
 * delay=0 is maximum intensity; large delay approaches an idle system.
 *
 * Reported per window: latency of the chase thread (ns/load) and aggregate bandwidth of the load threads.
 * Little's Law check (done in analysis): outstanding lines N = bandwidth * latency / 64 B.
 *
 *   latsize=256M size=256M threads=3 delay=0 rw=100 pattern=seq|rand huge=0
 *   latcpu=0 cpus=2,4,6   reps=5 ms=300 tag= -H
 */
#include "common.h"
#include "kern.h"

typedef struct {
    int id, cpu, huge, rdtens, rnd, reps, fail, delay, is_lat;
    size_t size;
    sync_t *s;
    double *out; /* lat thread: ns/step ; load thread: GB/s */
    uint64_t sink;
} targ;

#define BLKB 1024 /* load generators work on 1 KiB blocks (16 lines): per-line loop overhead otherwise limits delivered bandwidth */
static void *volatile g_sink; /* volatile: keeps the compiler from deleting the dependent-load loops */

static void lat_worker(targ *a) {
    setup_thread(a->cpu);
    size_t nn = a->size / LINE;
    mem_t m = mem_alloc(nn * LINE, a->huge);
    void *p = NULL;
    if (!m.p) a->fail = 1;
    else {
        memset(m.p, 0, nn * LINE);
        void *st[1];
        if (build_chains((char *)m.p, nn, LINE, 1, 1, 777, st) != 0) a->fail = 1;
        else { p = st[0]; p = chase1(p, 1 << 20); }
    }
    sync_ready(a->s);
    for (int g = 1; g <= a->reps + 1; g++) {
        sync_wait_gen(a->s, g);
        if (a->fail) {
            while (sync_running(a->s, g)) sleep_ms(1);
            a->out[g - 1] = -1;
            sync_done(a->s);
            continue;
        }
        size_t steps = 0;
        double t0 = now_sec();
        while (sync_running(a->s, g)) { p = chase1(p, 256); steps += 256; }
        double t1 = now_sec();
        a->out[g - 1] = (t1 - t0) * 1e9 / (double)steps;
        sync_done(a->s);
    }
    g_sink = p;
    mem_free(&m);
}

static void load_worker(targ *a) {
    setup_thread(a->cpu);
    mem_t m = mem_alloc(a->size, a->huge);
    size_t nb = a->size / BLKB, nb2 = 2;
    int shift = 63;
    if (!m.p) a->fail = 1;
    else {
        uint64_t *w = (uint64_t *)m.p;
        for (size_t i = 0; i < a->size / 8; i++) w[i] = i * 2654435761u + 1;
        while (nb2 * 2 <= nb) nb2 *= 2;
        int lg = 0;
        while (((size_t)1 << lg) < nb2) lg++;
        shift = 64 - lg;
    }
    sync_ready(a->s);
    uint64_t acc = 0, x = 0x9E3779B97F4A7C15ULL * (uint64_t)(a->id + 1), cnt = 0;
    size_t j = 0;
    char *base = (char *)m.p;
    for (int g = 1; g <= a->reps + 1; g++) {
        sync_wait_gen(a->s, g);
        if (a->fail) {
            while (sync_running(a->s, g)) sleep_ms(1);
            a->out[g - 1] = -1;
            sync_done(a->s);
            continue;
        }
        uint64_t lines = 0;
        double t0 = now_sec();
        while (sync_running(a->s, g)) {
            for (int u = 0; u < 128; u++) {
                size_t idx;
                if (a->rnd) { x = x * 6364136223846793005ULL + 1442695040888963407ULL; idx = (size_t)(x >> shift); }
                else { idx = j; if (++j == nb) j = 0; }
                uint64_t *blk = (uint64_t *)(base + idx * BLKB);
                int wr;
                if (a->rdtens >= 10) wr = 0;
                else if (a->rdtens <= 0) wr = 1;
                else wr = (int)((cnt++ % 10) >= (uint64_t)a->rdtens);
                if (wr) wr_block(blk, BLKB / 8, x + idx); else acc += rd_block(blk, BLKB / 8);
                for (long d = (long)a->delay * (BLKB / LINE); d > 0; d--) __asm__ volatile("");
            }
            lines += 128 * (BLKB / LINE);
        }
        double t1 = now_sec();
        a->out[g - 1] = (double)lines * LINE / (t1 - t0) / 1e9;
        sync_done(a->s);
    }
    a->sink = acc;
    mem_free(&m);
}

static void worker(void *vp) {
    targ *a = (targ *)vp;
    if (a->is_lat) lat_worker(a); else load_worker(a);
}

int main(int argc, char **argv) {
    size_t latsize = arg_size(argc, argv, "latsize", "256M");
    size_t size = arg_size(argc, argv, "size", "256M");
    int T = (int)arg_long(argc, argv, "threads", 1);
    int delay = (int)arg_long(argc, argv, "delay", 0);
    int rw = (int)arg_long(argc, argv, "rw", 100);
    int huge = (int)arg_long(argc, argv, "huge", 0);
    int latcpu = (int)arg_long(argc, argv, "latcpu", -1);
    int reps = (int)arg_long(argc, argv, "reps", 5);
    int ms = (int)arg_long(argc, argv, "ms", 300);
    const char *pat = arg_str(argc, argv, "pattern", "seq");
    const char *tag = arg_str(argc, argv, "tag", "");
    int cpus[256], ncpu = parse_cpu_list(arg_str(argc, argv, "cpus", ""), cpus, 256);
    int rnd = strcmp(pat, "rand") == 0;
    if (T < 0) T = 0;
    if (T > 255) T = 255;

    sync_t S;
    memset(&S, 0, sizeof S);
    int N = T + 1;
    targ *A = (targ *)calloc((size_t)N, sizeof(targ));
    thr_t *H = (thr_t *)calloc((size_t)N, sizeof(thr_t));
    for (int i = 0; i < N; i++) {
        A[i].id = i; A[i].is_lat = (i == 0); A[i].huge = huge; A[i].rdtens = rw / 10; A[i].rnd = rnd;
        A[i].reps = reps; A[i].delay = delay; A[i].s = &S;
        A[i].size = i == 0 ? latsize : size;
        A[i].cpu = i == 0 ? latcpu : ((i - 1 < ncpu) ? cpus[i - 1] : -1);
        A[i].out = (double *)calloc((size_t)reps + 1, sizeof(double));
        thr_start(&H[i], worker, &A[i]);
    }
    sync_wait_ready(&S, N);
    for (int g = 1; g <= reps + 1; g++) sync_window(&S, g, N, ms);
    for (int i = 0; i < N; i++) thr_join(&H[i]);
    for (int i = 0; i < N; i++) if (A[i].fail) { fprintf(stderr, "allocation failure\n"); return 1; }

    if (arg_has(argc, argv, "-H"))
        printf("tool,tag,latsize_bytes,size_bytes,threads,delay,rw,pattern,huge_req,rep,lat_ns,gbps_load\n");
    for (int r = 1; r <= reps; r++) {
        double bw = 0;
        for (int i = 1; i < N; i++) bw += A[i].out[r];
        printf("loaded,%s,%zu,%zu,%d,%d,%d,%s,%d,%d,%.3f,%.4f\n", tag, latsize, size, T, delay, rw, rnd ? "rand" : "seq", huge,
               r - 1, A[0].out[r], bw);
    }
    return 0;
}