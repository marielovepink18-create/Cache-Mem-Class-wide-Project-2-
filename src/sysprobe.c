/* sysprobe.c - prints key=value facts used by the scripts: cache sizes, core topology, SMT, large-page size,
 * and an effective core-frequency estimate from a dependent-IMUL chain (3-cycle latency on x86).
 *   sysprobe cpu=N      (pin the frequency calibration to logical CPU N)
 */
#include "common.h"

/* Core-clock estimate. A chain of dependent 64-bit IMULs has 3-cycle latency on every recent Intel and AMD core.
 * (A chain of dependent "add $1" is NOT usable: Intel Golden Cove / newer folds those at rename and reports 15+ GHz.) */
#if defined(__x86_64__)
#define OP8 "imul %0,%0\n\timul %0,%0\n\timul %0,%0\n\timul %0,%0\n\timul %0,%0\n\timul %0,%0\n\timul %0,%0\n\timul %0,%0\n\t"
#define LAT 3.0
#define HAVE_CALIB 1
#endif

static double calib_ghz(void) {
#ifdef HAVE_CALIB
    double best = 0;
    for (int trial = 0; trial < 5; trial++) {
        uint64_t x = 3;
        long iters = 6000000;
        double t0 = now_sec();
        for (long i = 0; i < iters; i++) __asm__ volatile(OP8 : "+r"(x));
        double t1 = now_sec();
        double ghz = (double)iters * 8.0 * LAT / (t1 - t0) / 1e9;
        if (ghz > best) best = ghz;
    }
    return best;
#else
    return 0.0;
#endif
}

int main(int argc, char **argv) {
    int cpu = (int)arg_long(argc, argv, "cpu", -1);
    long l1 = 0, l2 = 0, l3 = 0, line = 64;
    int cores = 0, logical = 0, smt = 0, hetero = 0;
    char phys[1024] = "";
#ifdef _WIN32
    DWORD len = 0;
    GetLogicalProcessorInformation(NULL, &len);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *b = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(len);
    if (b && GetLogicalProcessorInformation(b, &len)) {
        size_t n = len / sizeof(*b);
        for (size_t i = 0; i < n; i++) {
            if (b[i].Relationship == RelationProcessorCore) {
                cores++;
                ULONG_PTR mk = b[i].ProcessorMask;
                int first = -1;
                for (int k = 0; k < 64; k++) if (mk & ((ULONG_PTR)1 << k)) { logical++; if (first < 0) first = k; }
                if (b[i].ProcessorCore.Flags == 1) smt = 1;
                char t[16];
                snprintf(t, sizeof t, "%s%d", phys[0] ? "," : "", first);
                if (strlen(phys) + strlen(t) < sizeof phys - 1) strcat(phys, t);
            } else if (b[i].Relationship == RelationCache) {
                CACHE_DESCRIPTOR c = b[i].Cache;
                if (c.Type == CacheInstruction) continue;
                long *dst = c.Level == 1 ? &l1 : c.Level == 2 ? &l2 : c.Level == 3 ? &l3 : NULL;
                if (!dst) continue;
                if (*dst == 0) *dst = (long)c.Size; else if (*dst != (long)c.Size) hetero = 1;
                if (c.LineSize) line = c.LineSize;
            }
        }
    }
    printf("large_page_min=%zu\n", (size_t)GetLargePageMinimum());
#else
    l1 = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    l2 = sysconf(_SC_LEVEL2_CACHE_SIZE);
    l3 = sysconf(_SC_LEVEL3_CACHE_SIZE);
    long ln = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (ln > 0) line = ln;
    logical = (int)sysconf(_SC_NPROCESSORS_ONLN);
    printf("large_page_min=%d\n", 2 << 20);
#endif
    if (cpu >= 0) pin_cpu(cpu);
    printf("l1d_bytes=%ld\nl2_bytes=%ld\nl3_bytes=%ld\nline_bytes=%ld\n", l1, l2, l3, line);
    printf("physical_cores=%d\nlogical_cpus=%d\nsmt=%d\nphysical_cpus=%s\ncache_heterogeneous=%d\n", cores, logical, smt, phys,
           hetero);
    printf("calib_ghz=%.3f\n", calib_ghz());
    return 0;
}
