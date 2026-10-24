/* kernel.c - simple loop kernels used to correlate runtime with cache-miss / TLB-miss behaviour.
 *
 *   kernel=mul    z[i] = x[i]*y[i]
 *   kernel=saxpy  y[i] = a*x[i] + y[i]
 * Arrays are double, `size` bytes EACH (3 arrays are allocated; 24 B of traffic per element either way).
 * The arrays are processed in blocks of `blk` bytes. order=seq visits blocks in address order,
 * order=rand visits them in a random permutation:
 *   blk=size, order=seq   -> one long unit-stride stream (best case: prefetch-friendly, minimal TLB pressure)
 *   blk=4096, order=rand  -> sequential inside each 4 KiB page, but a new page (new TLB entry) every 512 elements
 *   blk=64,   order=rand  -> every cache line is a fresh random line: cache AND TLB miss on (almost) every block
 * Before timing, both kernels are validated against a plain scalar reference on a small odd-sized problem.
 *
 *   size=64M kernel=mul order=seq blk=0 huge=0 cpu=2 ghz=0 reps=5 ms=100 tag= -H
 *   (blk=0 means blk=size)
 *   passes=N  run exactly N passes per rep instead of timing for `ms` (use with reps=1 when profiling with hardware
 *             counters, so counter totals can be divided by the printed `elements` count)
 */
#include "common.h"
#include "kern.h"

#define NOINLINE __attribute__((noinline))

static NOINLINE void k_mul(double *__restrict z, const double *__restrict x, const double *__restrict y, size_t n) {
    for (size_t i = 0; i < n; i++) z[i] = x[i] * y[i];
}
static NOINLINE void k_saxpy(double *__restrict y, const double *__restrict x, double a, size_t n) {
    for (size_t i = 0; i < n; i++) y[i] = a * x[i] + y[i];
}

static int validate(void) {
    size_t n = 4096 + 7;
    double *x = malloc(n * 8), *y = malloc(n * 8), *z = malloc(n * 8), *y2 = malloc(n * 8);
    for (size_t i = 0; i < n; i++) { x[i] = (double)(i % 7 + 1); y[i] = y2[i] = (double)(i % 5 + 1); }
    k_mul(z, x, y, n);
    int ok = 1;
    for (size_t i = 0; i < n; i++) if (z[i] != x[i] * y[i]) ok = 0;
    k_saxpy(y, x, 0.5, n);
    for (size_t i = 0; i < n; i++) if (y[i] != 0.5 * x[i] + y2[i]) ok = 0;
    free(x); free(y); free(z); free(y2);
    return ok;
}

int main(int argc, char **argv) {
    size_t size = arg_size(argc, argv, "size", "64M");
    size_t blk = arg_size(argc, argv, "blk", "0");
    const char *kn = arg_str(argc, argv, "kernel", "mul");
    const char *ord = arg_str(argc, argv, "order", "seq");
    int huge = (int)arg_long(argc, argv, "huge", 0);
    int cpu = (int)arg_long(argc, argv, "cpu", -1);
    int reps = (int)arg_long(argc, argv, "reps", 5);
    int ms = (int)arg_long(argc, argv, "ms", 100);
    double ghz = arg_dbl(argc, argv, "ghz", 0.0);
    long fixed_passes = arg_long(argc, argv, "passes", 0);
    const char *tag = arg_str(argc, argv, "tag", "");
    int saxpy = strcmp(kn, "saxpy") == 0, rnd = strcmp(ord, "rand") == 0;

    if (!validate()) { fprintf(stderr, "VALIDATION FAILED: vectorised kernel != scalar reference\n"); return 2; }
    if (blk == 0 || blk > size) blk = size;
    blk = (blk + 63) & ~(size_t)63;
    size = (size / blk) * blk;
    if (size < 64) { fprintf(stderr, "size too small\n"); return 1; }
    size_t nb = size / blk, ne = size / 8, nbe = blk / 8;

    setup_thread(cpu);
    mem_t mx = mem_alloc(size, huge), my = mem_alloc(size, huge), mz = mem_alloc(size, huge);
    if (!mx.p || !my.p || !mz.p) { fprintf(stderr, "allocation failed\n"); return 1; }
    double *x = (double *)mx.p, *y = (double *)my.p, *z = (double *)mz.p;
    for (size_t i = 0; i < ne; i++) { x[i] = (double)(i % 7 + 1); y[i] = (double)(i % 5 + 1); z[i] = 0.0; }
    int hok = mem_huge_verified(&mx);

    uint32_t *ordv = (uint32_t *)malloc(nb * sizeof(uint32_t));
    for (size_t i = 0; i < nb; i++) ordv[i] = (uint32_t)i;
    if (rnd) { rng_t r = { 424242 }; shuffle_u32(ordv, nb, &r); }

    /* one untimed pass */
    for (size_t b = 0; b < nb; b++) {
        size_t o = (size_t)ordv[b] * nbe;
        if (saxpy) k_saxpy(y + o, x + o, 1e-9, nbe); else k_mul(z + o, x + o, y + o, nbe);
    }

    /* passes per timing chunk: enough work (>= 1M elements) that timer overhead is negligible */
    size_t chunk = (((size_t)1 << 20) + ne - 1) / ne;
    if (arg_has(argc, argv, "-H"))
        printf("tool,tag,size_bytes,kernel,order,blk,huge_req,huge_ok,cpu,rep,ns_per_elem,gbps,cycles_per_elem,elements\n");
    for (int r = 0; r < reps; r++) {
        size_t passes = 0;
        double t0 = now_sec(), t1;
        do {
            size_t nchunk = fixed_passes ? (size_t)fixed_passes : chunk;
            for (size_t c = 0; c < nchunk; c++) {
                for (size_t b = 0; b < nb; b++) {
                    size_t o = (size_t)ordv[b] * nbe;
                    if (saxpy) k_saxpy(y + o, x + o, 1e-9, nbe); else k_mul(z + o, x + o, y + o, nbe);
                }
            }
            passes += nchunk;
            t1 = now_sec();
        } while (!fixed_passes && (t1 - t0) * 1e3 < ms);
        double ns = (t1 - t0) * 1e9 / ((double)passes * (double)ne);
        printf("kernel,%s,%zu,%s,%s,%zu,%d,%d,%d,%d,%.5f,%.4f,%.4f,%.0f\n", tag, size, kn, rnd ? "rand" : "seq", blk, huge, hok,
               cpu, r, ns, 24.0 / ns, ghz > 0 ? ns * ghz : 0.0, (double)passes * (double)ne);
        fflush(stdout);
    }
    volatile double sink = z[ne / 2] + y[ne / 3];
    (void)sink;
    return 0;
}
