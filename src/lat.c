/* lat.c - pointer-chasing latency (zero-queue when chains=1) and MLP sweep (chains>1).
 *
 * Usage (key=value):
 *   size=64M          working-set bytes (nodes = size/stride)
 *   stride=64         bytes between nodes (4160 = page+line stride for TLB tests: avoids cache-set aliasing)
 *   pattern=rand|seq  rand = random cycle (defeats prefetchers), seq = address-order walk (prefetchable)
 *   chains=1          independent dependent-load chains issued in lock-step (= outstanding misses)
 *   huge=0|1          request large/huge pages
 *   cpu=N             pin to logical CPU N (-1 = do NOT pin: used for the "unpinned" pitfall)
 *   reps=5 ms=100     timed windows per invocation and target length of each
 *   warmup=1 seed=..  tag=label  -H (print CSV header)
 * Output (CSV): one row per rep.
 *   ns_per_step = time for ALL chains to advance one node = per-access latency while chains are not queueing.
 *   gbps        = chains*64B/ns_per_step  (line-granular traffic; meaningful when stride>=64)
 */
#include "common.h"
#include "kern.h"

#define MAXK 64
static void *volatile g_sink; /* volatile: keeps the compiler from deleting the dependent-load loops */

int main(int argc, char **argv) {
    size_t size = arg_size(argc, argv, "size", "64M");
    size_t stride = arg_size(argc, argv, "stride", "64");
    const char *pat = arg_str(argc, argv, "pattern", "rand");
    int K = (int)arg_long(argc, argv, "chains", 1);
    int huge = (int)arg_long(argc, argv, "huge", 0);
    int cpu = (int)arg_long(argc, argv, "cpu", -1);
    int reps = (int)arg_long(argc, argv, "reps", 5);
    int ms = (int)arg_long(argc, argv, "ms", 100);
    int warm = (int)arg_long(argc, argv, "warmup", 1);
    uint64_t seed = (uint64_t)arg_long(argc, argv, "seed", 12345);
    const char *tag = arg_str(argc, argv, "tag", "");
    int rnd = strcmp(pat, "seq") != 0;

    if (stride < 16) stride = 16;
    stride = (stride + 7) & ~(size_t)7;
    if (K < 1) K = 1;
    if (K > MAXK) K = MAXK;
    size_t nn = size / stride;
    if (nn < 2) { fprintf(stderr, "size too small for stride\n"); return 1; }
    if ((size_t)K * 2 > nn) K = (int)(nn / 2);

    setup_thread(cpu);
    mem_t m = mem_alloc(nn * stride, huge);
    if (!m.p) { fprintf(stderr, "allocation of %zu bytes failed\n", nn * stride); return 1; }
    char *base = (char *)m.p;
    memset(base, 0, nn * stride); /* force physical backing before timing (avoid zero-page / first-touch effects) */

    void *p[MAXK];
    if (build_chains(base, nn, stride, K, rnd, seed, p) != 0) { fprintf(stderr, "oom\n"); return 1; }
    int hok = mem_huge_verified(&m);

    size_t seg = nn / (size_t)K;
    if (warm) { /* touch the whole chain once (capped) so caches/TLBs hold what they can hold */
        size_t w = seg < ((size_t)1 << 22) ? seg : ((size_t)1 << 22);
        w = (w + 7) & ~(size_t)7;
        if (K == 1) p[0] = chase1(p[0], w); else chasek(p, K, w);
    }

    if (arg_has(argc, argv, "-H"))
        printf("tool,tag,size_bytes,stride,pattern,chains,huge_req,huge_ok,cpu,seed,rep,ns_per_step,gbps\n");

    size_t ch = K == 1 ? ((size_t)1 << 16) : (((size_t)1 << 16) / (size_t)K + 8) & ~(size_t)7;
    for (int r = 0; r < reps; r++) {
        size_t steps = 0;
        double t0 = now_sec(), t1;
        do {
            if (K == 1) p[0] = chase1(p[0], ch); else chasek(p, K, ch);
            steps += ch;
            t1 = now_sec();
        } while ((t1 - t0) * 1e3 < ms);
        double ns = (t1 - t0) * 1e9 / (double)steps;
        printf("lat,%s,%zu,%zu,%s,%d,%d,%d,%d,%llu,%d,%.4f,%.4f\n", tag, nn * stride, stride, rnd ? "rand" : "seq", K,
               huge, hok, cpu, (unsigned long long)seed, r, ns, (double)K * LINE / ns);
        fflush(stdout);
    }
    g_sink = p[0];
    mem_free(&m);
    return 0;
}
