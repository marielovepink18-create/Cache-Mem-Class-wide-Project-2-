/* runner.c - no-install replacement for scripts/run_all.py + scripts/sysinfo.py.
 *
 *   runner.exe [--quick] [--smt] [--only a,b,c] [--tag NAME] [--cpu N] [--cores N] [--mem MB] [--list] [--nopause]
 *
 * Runs the other tools in the same folder (lat, bw, loaded, kernel, sysprobe), appends their CSV output to
 * ../data/raw/ CSV files (identical layout to run_all.py, so scripts/analyze.py works unchanged) and writes
 * ../data/sysinfo.json. Needs nothing installed: on Windows it uses PowerShell/powercfg only to record the
 * CPU name, DIMMs and power plan for the report (missing pieces are simply left blank).
 */
#include "common.h"
#include <math.h>
#include <stdarg.h>
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define POPEN _popen
#define PCLOSE _pclose
#define MKDIR(p) _mkdir(p)
#define SEP "\\"
#define EXE ".exe"
#else
#include <sys/stat.h>
#include <sys/types.h>
#define POPEN popen
#define PCLOSE pclose
#define MKDIR(p) mkdir(p, 0755)
#define SEP "/"
#define EXE ""
#endif

#define KiB ((unsigned long long)1 << 10)
#define MiB ((unsigned long long)1 << 20)
#define GiB ((unsigned long long)1 << 30)
#define U(x) ((unsigned long long)(x))
#define MAXCPU 256

static char g_bin[600], g_data[600], g_raw[600];
static int g_count = 0, g_fail = 0, g_smt = 0;

typedef struct {
    int quick, reps, ms, msbw, nseeds, per_oct;
    unsigned long long l1, l2, l3, dram, maxmem;
    double ghz;
    int cpu1, lat_cpu, nall, nload, tmax;
    int cpus_all[MAXCPU], load_cpus[MAXCPU];
    char tag[64];
} cfg_t;
static cfg_t C;

/* ------------------------------------------------------------- helpers */
static char *capture(const char *cmd, int *rc) {
    char full[4096];
#ifdef _WIN32
    snprintf(full, sizeof full, "\"%s\"", cmd); /* cmd /c strips one pair of outer quotes */
#else
    snprintf(full, sizeof full, "%s", cmd);
#endif
    FILE *p = POPEN(full, "r");
    if (!p) { if (rc) *rc = -1; return NULL; }
    size_t cap = 1 << 16, n = 0;
    char *buf = (char *)malloc(cap);
    size_t r;
    while ((r = fread(buf + n, 1, cap - n - 1, p)) > 0) {
        n += r;
        if (cap - n < 4096) { cap *= 2; buf = (char *)realloc(buf, cap); }
    }
    buf[n] = 0;
    int st = PCLOSE(p);
    if (rc) *rc = st;
    return buf;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

static int file_nonempty(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n > 0;
}

static void rawpath(char *out, size_t n, const char *name) { snprintf(out, n, "%s%s%s", g_raw, SEP, name); }
static void reset(const char *a, const char *b, const char *c) {
    char p[800];
    const char *l[3] = {a, b, c};
    for (int i = 0; i < 3; i++)
        if (l[i]) { rawpath(p, sizeof p, l[i]); remove(p); }
}

/* run one tool invocation; append its CSV rows to data/raw/<out> */
static int run(const char *tool, const char *out, const char *fmt, ...) {
    char args[1024], cmd[2048], path[800];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(args, sizeof args, fmt, ap);
    va_end(ap);
    rawpath(path, sizeof path, out);
    int hdr = !file_nonempty(path);
    snprintf(cmd, sizeof cmd, "\"%s%s%s%s\" %s%s", g_bin, SEP, tool, EXE, args, hdr ? " -H" : "");
    g_count++;
    double t0 = now_sec();
    int rc = 0;
    char *o = capture(cmd, &rc);
    if (rc != 0 || !o || !*o) {
        g_fail++;
        printf("   !! FAILED (rc=%d): %s\n", rc, cmd);
        fflush(stdout);
        free(o);
        return 0;
    }
    FILE *f = fopen(path, "a");
    if (f) { fputs(o, f); fclose(f); }
    printf("   [%4d] %s %s  (%.1fs)\n", g_count, tool, args, now_sec() - t0);
    fflush(stdout);
    free(o);
    return 1;
}

static int sweep(double lo, double hi, int per_oct, unsigned long long *out, int max) {
    double f = pow(2.0, 1.0 / per_oct), s = lo;
    int n = 0;
    while (s <= hi * 1.0001 && n < max) {
        unsigned long long v = s >= 4096 ? (unsigned long long)floor(s / 4096.0 + 0.5) * 4096ULL : (unsigned long long)s;
        if (n == 0 || v != out[n - 1]) out[n++] = v;
        s *= f;
    }
    return n;
}

static const char *cpu_list(const int *arr, int n) {
    static char bufs[4][1024];
    static int k = 0;
    char *b = bufs[k++ & 3];
    b[0] = 0;
    for (int i = 0; i < n; i++) {
        char t[16];
        snprintf(t, sizeof t, "%s%d", i ? "," : "", arr[i]);
        strncat(b, t, 1023 - strlen(b));
    }
    return b;
}

static int counts(int n, int *out) {
    static const int base[] = {1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 32};
    int k = 0;
    for (size_t i = 0; i < sizeof base / sizeof *base; i++)
        if (base[i] <= n) out[k++] = base[i];
    if (k == 0 || out[k - 1] != n) out[k++] = n;
    return k;
}

static unsigned long long r4k(unsigned long long x) {
    x = x / 4096 * 4096;
    return x < 4096 ? 4096 : x;
}
static unsigned long long bwsize(int T) {
    if (T == 1) return C.dram;
    unsigned long long v = C.maxmem / (unsigned long long)T;
    if (v > C.dram) v = C.dram;
    return v < 64 * MiB ? 64 * MiB : v;
}

/* ----------------------------------------------------------- experiments */
#define MAXS 128
static void exp_lat_ws(void) {
    unsigned long long s[MAXS];
    int n = sweep(4 * KiB, (double)C.dram, C.per_oct, s, MAXS);
    reset("lat_ws.csv", NULL, NULL);
    for (int sd = 1; sd <= C.nseeds; sd++)
        for (int i = 0; i < n; i++)
            run("lat", "lat_ws.csv", "size=%llu stride=64 pattern=rand chains=1 cpu=%d reps=%d ms=%d seed=%d", s[i], C.cpu1, C.reps, C.ms, sd);
}

static void exp_zero_queue(void) {
    reset("lat_zeroq.csv", NULL, NULL);
    const char *lv[4] = {"L1", "L2", "L3", "DRAM"};
    unsigned long long sz[4] = {C.l1 / 2, C.l2 / 2, C.l3 ? C.l3 / 2 : 8 * MiB, C.dram};
    for (int i = 0; i < 4; i++)
        for (int sd = 1; sd <= C.nseeds; sd++)
            run("lat", "lat_zeroq.csv", "size=%llu stride=64 pattern=rand chains=1 cpu=%d reps=%d ms=%d seed=%d tag=%s", r4k(sz[i]), C.cpu1, C.reps * 2,
                C.ms, sd, lv[i]);
}

static void thread_set(int *T, int *n) {
    T[0] = 1; *n = 1;
    if (C.tmax > 1) T[(*n)++] = C.tmax;
}

static void exp_patterns(void) {
    reset("lat_patterns.csv", "bw_patterns.csv", "bw_stride.csv");
    const char *pats[2] = {"seq", "rand"};
    int strides[3] = {64, 256, 1024};
    for (int p = 0; p < 2; p++)
        for (int s = 0; s < 3; s++)
            run("lat", "lat_patterns.csv", "size=%llu stride=%d pattern=%s chains=1 cpu=%d reps=%d ms=%d", C.dram, strides[s], pats[p], C.cpu1, C.reps, C.ms);
    int Ts[2], nT;
    thread_set(Ts, &nT);
    int grans[6] = {64, 128, 256, 512, 1024, 4096};
    for (int t = 0; t < nT; t++) {
        int T = Ts[t];
        const char *cp = cpu_list(C.cpus_all, T);
        for (int p = 0; p < 2; p++)
            for (int g = 0; g < 6; g++)
                run("bw", "bw_patterns.csv", "size=%llu threads=%d cpus=%s pattern=%s gran=%d stride=%d rw=100 reps=%d ms=%d", bwsize(T), T, cp, pats[p],
                    grans[g], grans[g], C.reps, C.msbw);
        for (int g = 0; g < 6; g++)
            run("bw", "bw_stride.csv", "size=%llu threads=%d cpus=%s pattern=seq gran=64 stride=%d rw=100 reps=%d ms=%d", bwsize(T), T, cp, grans[g], C.reps,
                C.msbw);
    }
}

static void exp_rw_mix(void) {
    reset("bw_rw.csv", NULL, NULL);
    int Ts[2], nT, rws[4] = {100, 70, 50, 0};
    thread_set(Ts, &nT);
    for (int t = 0; t < nT; t++) {
        const char *cp = cpu_list(C.cpus_all, Ts[t]);
        for (int r = 0; r < 4; r++)
            for (int nt = 0; nt <= (rws[r] < 100 ? 1 : 0); nt++)
                run("bw", "bw_rw.csv", "size=%llu threads=%d cpus=%s pattern=seq gran=1024 stride=1024 rw=%d nt=%d reps=%d ms=%d", bwsize(Ts[t]), Ts[t], cp,
                    rws[r], nt, C.reps, C.msbw);
    }
}

static void exp_bw_ws(void) {
    unsigned long long s[MAXS];
    int n = sweep(8 * KiB, (double)C.dram, C.per_oct, s, MAXS);
    reset("bw_ws.csv", NULL, NULL);
    for (int i = 0; i < n; i++)
        run("bw", "bw_ws.csv", "size=%llu threads=1 cpus=%d pattern=seq gran=1024 stride=1024 rw=100 reps=%d ms=%d", s[i], C.cpu1, C.reps, C.msbw);
}

static void exp_loaded(void) {
    reset("loaded_intensity.csv", "loaded_rw.csv", NULL);
    static const int dfull[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 16384, 65536};
    static const int dquick[] = {0, 4, 16, 64, 256, 1024, 8192, 65536};
    const int *dl = C.quick ? dquick : dfull;
    int nd = C.quick ? 8 : 16;
    char common[512];
    snprintf(common, sizeof common, "latsize=%llu size=%llu latcpu=%d reps=%d ms=%d", C.dram, C.dram, C.lat_cpu, C.reps, C.msbw);
    run("loaded", "loaded_intensity.csv", "threads=0 delay=0 rw=100 pattern=seq %s", common);
    int tl[16], nt;
    if (C.quick) { tl[0] = 1; nt = 1; if (C.nload > 1) tl[nt++] = C.nload; }
    else nt = counts(C.nload, tl);
    for (int t = 0; t < nt; t++)
        for (int d = 0; d < nd; d++)
            run("loaded", "loaded_intensity.csv", "threads=%d cpus=%s delay=%d rw=100 pattern=seq %s", tl[t], cpu_list(C.load_cpus, tl[t]), dl[d], common);
    int rws[4] = {100, 70, 50, 0};
    for (int r = 0; r < 4; r++)
        run("loaded", "loaded_rw.csv", "threads=%d cpus=%s delay=0 rw=%d pattern=seq %s", C.nload, cpu_list(C.load_cpus, C.nload), rws[r], common);
}

static void exp_chains(void) {
    reset("lat_chains.csv", NULL, NULL);
    static const int kf[] = {1, 2, 3, 4, 6, 8, 10, 12, 16, 20, 24, 32, 48, 64};
    static const int kq[] = {1, 2, 4, 8, 12, 16, 32};
    const int *k = C.quick ? kq : kf;
    int n = C.quick ? 7 : 14;
    for (int i = 0; i < n; i++)
        run("lat", "lat_chains.csv", "size=%llu stride=64 pattern=rand chains=%d cpu=%d reps=%d ms=%d", C.dram, k[i], C.cpu1, C.reps, C.ms);
}

static void exp_tlb(void) {
    unsigned long long s[MAXS];
    int n = sweep(64 * KiB, (double)C.dram, C.per_oct, s, MAXS);
    reset("lat_tlb.csv", "kernel_tlb.csv", NULL);
    for (int h = 0; h <= 1; h++)
        for (int i = 0; i < n; i++)
            run("lat", "lat_tlb.csv", "size=%llu stride=4160 pattern=rand chains=1 huge=%d cpu=%d reps=%d ms=%d", s[i], h, C.cpu1, C.reps, C.ms);
    for (int i = 0; i < n; i++)
        run("lat", "lat_tlb.csv", "size=%llu stride=64 pattern=rand chains=1 huge=1 cpu=%d reps=%d ms=%d", s[i], C.cpu1, C.reps, C.ms);
    unsigned long long ksz = C.dram / 2;
    int blks[2] = {4096, 65536};
    for (int h = 0; h <= 1; h++) {
        run("kernel", "kernel_tlb.csv", "size=%llu kernel=mul order=seq blk=0 huge=%d cpu=%d ghz=%.3f reps=%d ms=%d tag=seq", ksz, h, C.cpu1, C.ghz, C.reps, C.ms);
        for (int b = 0; b < 2; b++)
            run("kernel", "kernel_tlb.csv", "size=%llu kernel=mul order=rand blk=%d huge=%d cpu=%d ghz=%.3f reps=%d ms=%d tag=pagerand", ksz, blks[b], h, C.cpu1,
                C.ghz, C.reps, C.ms);
    }
}

static void exp_kernel_ws(void) {
    unsigned long long s[MAXS];
    int n = sweep(4 * KiB, (double)(C.dram / 2), C.per_oct, s, MAXS);
    reset("kernel_ws.csv", NULL, NULL);
    const char *k[2] = {"mul", "saxpy"};
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < n; i++)
            run("kernel", "kernel_ws.csv", "size=%llu kernel=%s order=seq blk=0 cpu=%d ghz=%.3f reps=%d ms=%d", s[i], k[j], C.cpu1, C.ghz, C.reps, C.ms);
}

static void exp_kernel_cache(void) {
    unsigned long long s[MAXS], ksz = C.dram / 2;
    reset("kernel_cache.csv", NULL, NULL);
    run("kernel", "kernel_cache.csv", "size=%llu kernel=mul order=seq blk=0 cpu=%d ghz=%.3f reps=%d ms=%d tag=blk_sweep", ksz, C.cpu1, C.ghz, C.reps, C.ms);
    static const int blks[] = {64, 128, 256, 512, 1024, 4096, 16384, 65536, 262144};
    for (int i = 0; i < 9; i++)
        run("kernel", "kernel_cache.csv", "size=%llu kernel=mul order=rand blk=%d cpu=%d ghz=%.3f reps=%d ms=%d tag=blk_sweep", ksz, blks[i], C.cpu1, C.ghz,
            C.reps, C.ms);
    int n = sweep(4 * KiB, (double)ksz, C.per_oct, s, MAXS);
    for (int i = 0; i < n; i++)
        run("kernel", "kernel_cache.csv", "size=%llu kernel=mul order=rand blk=64 cpu=%d ghz=%.3f reps=%d ms=%d tag=fp_rand64", s[i], C.cpu1, C.ghz, C.reps, C.ms);
}

static void exp_pitfall_pin(void) {
    reset("pitfall_pin.csv", "pitfall_pin_bw.csv", NULL);
    int reps = C.reps * 3 > 10 ? C.reps * 3 : 10;
    const char *lbl[3] = {"L2", "L3", "DRAM"};
    unsigned long long sz[3] = {C.l2 / 2, C.l3 ? C.l3 / 2 : 8 * MiB, C.dram};
    for (int i = 0; i < 3; i++)
        for (int pinned = 1; pinned >= 0; pinned--)
            run("lat", "pitfall_pin.csv", "size=%llu stride=64 pattern=rand chains=1 cpu=%d reps=%d ms=%d tag=%s_%s", r4k(sz[i]), pinned ? C.cpu1 : -1, reps,
                C.ms, lbl[i], pinned ? "pinned" : "unpinned");
    run("bw", "pitfall_pin_bw.csv", "size=%llu threads=1 cpus=%d pattern=seq gran=1024 stride=1024 rw=100 reps=%d ms=%d tag=BW_pinned", C.dram, C.cpu1, reps,
        C.msbw);
    run("bw", "pitfall_pin_bw.csv", "size=%llu threads=1 pattern=seq gran=1024 stride=1024 rw=100 reps=%d ms=%d tag=BW_unpinned", C.dram, reps, C.msbw);
}

/* drop rows whose tag (before ':') equals `tag` from a CSV; keeps header and other tags */
static void drop_tag_rows(const char *name, const char *tag) {
    char path[800];
    rawpath(path, sizeof path, name);
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t cap = 1 << 20, n = 0;
    char *keep = (char *)malloc(cap), line[4096];
    while (fgets(line, sizeof line, f)) {
        int drop = 0;
        if (strncmp(line, "tool,", 5) != 0) {
            const char *c1 = strchr(line, ',');
            if (c1) {
                c1++;
                size_t k = 0;
                while (c1[k] && c1[k] != ',' && c1[k] != ':') k++;
                drop = strlen(tag) == k && strncmp(c1, tag, k) == 0;
            }
        }
        size_t L = strlen(line);
        if (!drop) {
            if (n + L + 1 > cap) { cap *= 2; keep = (char *)realloc(keep, cap); }
            memcpy(keep + n, line, L);
            n += L;
        }
    }
    fclose(f);
    f = fopen(path, "w");
    if (f) { fwrite(keep, 1, n, f); fclose(f); }
    free(keep);
}

static void exp_pitfall_freq(void) {
    const char *tag = C.tag[0] ? C.tag : "default";
    drop_tag_rows("pitfall_freq.csv", tag);
    drop_tag_rows("pitfall_freq_kernel.csv", tag);
    const char *lbl[3] = {"L1", "L2", "DRAM"};
    unsigned long long sz[3] = {C.l1 / 2, C.l2 / 2, C.dram};
    for (int i = 0; i < 3; i++)
        run("lat", "pitfall_freq.csv", "size=%llu stride=64 pattern=rand chains=1 cpu=%d reps=%d ms=%d tag=%s:%s", r4k(sz[i]), C.cpu1, C.reps * 2, C.ms, tag,
            lbl[i]);
    run("kernel", "pitfall_freq_kernel.csv", "size=%llu kernel=mul order=seq blk=0 cpu=%d ghz=%.3f reps=%d ms=%d tag=%s", r4k(C.l1 / 2), C.cpu1, C.ghz, C.reps * 2,
        C.ms, tag);
}

typedef struct { const char *name; void (*fn)(void); const char *doc; int in_default; } exp_t;
static const exp_t EXPS[] = {
    {"lat_ws", exp_lat_ws, "zero-queue latency vs working set", 1},
    {"zero_queue", exp_zero_queue, "per-level latency table points", 1},
    {"patterns", exp_patterns, "sequential/random, stride, granularity", 1},
    {"rw_mix", exp_rw_mix, "read/write mixes, normal vs non-temporal stores", 1},
    {"bw_ws", exp_bw_ws, "single-thread bandwidth vs working set", 1},
    {"loaded", exp_loaded, "throughput-latency intensity sweep + rw mix under load", 1},
    {"chains", exp_chains, "memory-level parallelism of one core", 1},
    {"tlb", exp_tlb, "TLB reach, 4 KiB vs large pages", 1},
    {"kernel_ws", exp_kernel_ws, "kernel runtime vs footprint", 1},
    {"kernel_cache", exp_kernel_cache, "kernel vs spatial locality / footprint", 1},
    {"pitfall_pin", exp_pitfall_pin, "pinned vs unpinned", 1},
    {"pitfall_freq", exp_pitfall_freq, "power/frequency policy (run once per --tag)", 0},
};
#define NEXP ((int)(sizeof EXPS / sizeof *EXPS))

/* -------------------------------------------------- system information */
static char *kv(const char *text, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    const char *p = text;
    out[0] = 0;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t i = 0;
            p += kl + 1;
            while (p[i] && p[i] != '\n' && p[i] != '\r' && i < n - 1) { out[i] = p[i]; i++; }
            out[i] = 0;
            return out;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return out;
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') { fputc('\\', f); fputc(*s, f); }
        else if ((unsigned char)*s < 32) fputc(' ', f);
        else fputc(*s, f);
    }
    fputc('"', f);
}

static char *ps(const char *query) {
    char cmd[2048];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "powershell -NoProfile -Command \"%s\"", query);
    char *o = capture(cmd, NULL);
    if (o) trim(o);
    return o;
#else
    (void)query;
    (void)cmd;
    return strdup("");
#endif
}

static void collect_sysinfo(int cores_limit, int cpu_override) {
    char cmd[1024], b[1024], b2[1024];
    snprintf(cmd, sizeof cmd, "\"%s%ssysprobe%s\" cpu=-1", g_bin, SEP, EXE);
    char *p1 = capture(cmd, NULL);
    if (!p1 || !*p1) { printf("sysprobe failed - are the .exe files in the same folder as runner?\n"); exit(1); }
    int phys[MAXCPU], nphys = 0;
    kv(p1, "physical_cpus", b, sizeof b);
    for (char *s = b; *s;) {
        phys[nphys++] = (int)strtol(s, &s, 10);
        if (*s == ',') s++; else break;
    }
    int logical = atoi(kv(p1, "logical_cpus", b, sizeof b));
    if (nphys == 0) for (int i = 0; i < logical && i < MAXCPU; i++) phys[nphys++] = i;
    int cal = nphys > 1 ? phys[1] : phys[0];
    snprintf(cmd, sizeof cmd, "\"%s%ssysprobe%s\" cpu=%d", g_bin, SEP, EXE, cal);
    char *p2 = capture(cmd, NULL);
    double ghz = p2 ? atof(kv(p2, "calib_ghz", b, sizeof b)) : 0;
    int ncore_all = nphys;
    if (cores_limit > 0 && cores_limit < nphys) nphys = cores_limit;

    C.l1 = (unsigned long long)atoll(kv(p1, "l1d_bytes", b, sizeof b));
    C.l2 = (unsigned long long)atoll(kv(p1, "l2_bytes", b, sizeof b));
    C.l3 = (unsigned long long)atoll(kv(p1, "l3_bytes", b, sizeof b));
    C.ghz = ghz;
    if (C.l1 == 0) C.l1 = 32 * KiB;
    if (C.l2 == 0) C.l2 = 256 * KiB;

    /* cpu assignment (same rules as run_all.py) */
    C.cpu1 = cpu_override >= 0 ? cpu_override : (nphys > 1 ? phys[1] : phys[0]);
    C.lat_cpu = (nphys > 1 && C.cpu1 != phys[0]) ? phys[0] : phys[nphys - 1];
    C.nall = 0;
    C.cpus_all[C.nall++] = C.cpu1;
    for (int i = 0; i < nphys; i++) if (phys[i] != C.cpu1) C.cpus_all[C.nall++] = phys[i];
    int nl = 0;
    if (nphys <= 2 && logical > nphys && !g_smt) {
        g_smt = 1;
        printf("NOTE: only %d physical cores - load threads will also use hyper-thread siblings so memory can be saturated (state this in the report).\n", nphys);
    }
    if (!g_smt) for (int i = 0; i < nphys; i++) if (phys[i] != C.lat_cpu) C.load_cpus[nl++] = phys[i];
    /* --smt: load generators may also use hyper-thread siblings (needed to saturate DRAM on 2-core CPUs) */
    if (nl == 0) for (int i = 0; i < logical && i < MAXCPU; i++) if (i != C.lat_cpu) C.load_cpus[nl++] = i;

    /* memory plan */
    unsigned long long dram = 1ULL << 28;
    while (dram < 8 * C.l3 && dram < GiB) dram <<= 1;
    while (dram * 2 > C.maxmem && dram > 64 * MiB) dram >>= 1;
    C.dram = dram;
    int tm = (int)(C.maxmem / C.dram);
    C.tmax = C.nall < tm ? C.nall : tm;
    if (C.tmax < 1) C.tmax = 1;
    int nlm = tm - 1;
    C.nload = nl < nlm ? nl : nlm;
    if (C.nload < 1) C.nload = 1;
    if (C.nload > 15) C.nload = 15;

    /* descriptive fields for the report */
    char *model, *plan, *dimms, *ram, *osv;
#ifdef _WIN32
    model = ps("(Get-CimInstance Win32_Processor | Select -First 1).Name");
    plan = capture("powercfg /getactivescheme", NULL);
    dimms = ps("Get-CimInstance Win32_PhysicalMemory | ForEach-Object { '{0} GB @ {1} MT/s (rated {2})' -f ($_.Capacity/1GB), $_.ConfiguredClockSpeed, $_.Speed }");
    ram = ps("[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB,1)");
    osv = capture("ver", NULL);
#else
    model = capture("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2", NULL);
    plan = strdup("");
    dimms = strdup("");
    ram = capture("awk '/MemTotal/ {printf \"%.1f\", $2/1048576}' /proc/meminfo", NULL);
    osv = capture("uname -sr", NULL);
#endif
    if (model) trim(model);
    if (plan) trim(plan);
    if (osv) trim(osv);
    if (ram) trim(ram);

    char info[600];
    snprintf(info, sizeof info, "%s%sBUILD_INFO.txt", g_bin, SEP);
    char build[512] = "prebuilt (see bin/BUILD_INFO.txt)", flags[512] = "";
    FILE *bf = fopen(info, "r");
    if (bf) {
        char ln[512];
        int first = 1;
        while (fgets(ln, sizeof ln, bf)) {
            trim(ln);
            if (first) { snprintf(build, sizeof build, "%s", ln); first = 0; }
            else if (ln[0] == '-') snprintf(flags, sizeof flags, "%s", ln);
        }
        fclose(bf);
    }

    char path[800];
    snprintf(path, sizeof path, "%s%ssysinfo.json", g_data, SEP);
    FILE *f = fopen(path, "w");
    if (!f) { printf("cannot write %s\n", path); exit(1); }
    fprintf(f, "{\n  \"os\": "); json_str(f, osv ? osv : "");
    fprintf(f, ",\n  \"machine\": \"x86_64\",\n  \"compiler\": "); json_str(f, build);
    fprintf(f, ",\n  \"cflags\": "); json_str(f, flags);
    fprintf(f, ",\n  \"cache\": {\"l1d_bytes\": %llu, \"l2_bytes\": %llu, \"l3_bytes\": %llu, \"line_bytes\": %s},\n", U(C.l1), U(C.l2), U(C.l3),
            *kv(p1, "line_bytes", b2, sizeof b2) ? b2 : "64");
    fprintf(f, "  \"cache_heterogeneous\": %s,\n", strcmp(kv(p1, "cache_heterogeneous", b, sizeof b), "1") == 0 ? "true" : "false");
    fprintf(f, "  \"logical_cpus\": %d,\n  \"physical_cpus\": [", logical);
    for (int i = 0; i < nphys; i++) fprintf(f, "%s%d", i ? ", " : "", phys[i]);
    fprintf(f, "],\n  \"physical_cores_detected\": %d,\n  \"physical_cores\": %d,\n  \"smt_enabled\": %s,\n", ncore_all, nphys,
            logical > ncore_all ? "true" : "false");
    fprintf(f, "  \"large_page_min\": %s,\n  \"measured_core_ghz\": %.3f,\n  \"freq_calibration_cpu\": %d,\n",
            *kv(p1, "large_page_min", b, sizeof b) ? b : "0", ghz, cal);
    fprintf(f, "  \"cpu_model\": "); json_str(f, model ? model : "");
    fprintf(f, ",\n  \"power_plan\": "); json_str(f, plan ? plan : "");
    fprintf(f, ",\n  \"total_ram_gb\": "); json_str(f, ram ? ram : "");
    fprintf(f, ",\n  \"dimms\": ["); {
        int first = 1;
        char *d = dimms ? dimms : (char *)"";
        char *line = strtok(d, "\r\n");
        while (line) { if (*line) { if (!first) fputs(", ", f); json_str(f, line); first = 0; } line = strtok(NULL, "\r\n"); }
    }
    fprintf(f, "]\n}\n");
    fclose(f);

    printf("CPU: %s\n  %d physical cores (%d logical, SMT %s), L1d %llu KiB, L2 %llu KiB, L3 %llu MiB, measured %.2f GHz\n", model ? model : "?", nphys, logical,
           logical > ncore_all ? "on" : "off", U(C.l1 / KiB), U(C.l2 / KiB), U(C.l3 / MiB), ghz);
    if (plan && *plan) printf("  %s\n", plan);
    if (strcmp(kv(p1, "cache_heterogeneous", b, sizeof b), "1") == 0)
        printf("  NOTE: cache sizes differ between cores (hybrid CPU). Use --cores <number of P-cores>.\n");
    free(p1); free(p2); free(model); free(plan); free(dimms); free(ram); free(osv);
}

/* ---------------------------------------------------------------- main */
static void pause_exit(int nopause) {
    if (nopause) return;
    printf("\nPress Enter to close...");
    fflush(stdout);
    getchar();
}

int main(int argc, char **argv) {
    int quick = 0, cores = 0, cpu = -1, nopause = 0, list = 0;
    unsigned long long memmb = 1024;
    char only[512] = "";
    C.tag[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick")) quick = 1;
        else if (!strcmp(argv[i], "--nopause")) nopause = 1;
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--smt")) g_smt = 1;
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) snprintf(only, sizeof only, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--tag") && i + 1 < argc) snprintf(C.tag, sizeof C.tag, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cores") && i + 1 < argc) cores = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc) memmb = (unsigned long long)atoll(argv[++i]);
        else { printf("unknown argument %s\n", argv[i]); return 1; }
    }
    if (list) {
        for (int i = 0; i < NEXP; i++) printf("%-14s %s\n", EXPS[i].name, EXPS[i].doc);
        return 0;
    }
    /* locate bin dir from argv[0]; data dir is ../data */
    snprintf(g_bin, sizeof g_bin, "%s", argv[0]);
    char *sl = strrchr(g_bin, '\\');
    char *sl2 = strrchr(g_bin, '/');
    if (sl2 && (!sl || sl2 > sl)) sl = sl2;
    if (sl) *sl = 0; else snprintf(g_bin, sizeof g_bin, ".");
    snprintf(g_data, sizeof g_data, "%s%s..%sdata", g_bin, SEP, SEP);
    snprintf(g_raw, sizeof g_raw, "%s%sraw", g_data, SEP);
    MKDIR(g_data);
    MKDIR(g_raw);

    C.quick = quick;
    C.maxmem = memmb * MiB;
    C.reps = quick ? 3 : 5;
    C.ms = quick ? 60 : 100;
    C.msbw = quick ? 200 : 300;
    C.nseeds = quick ? 1 : 2;
    C.per_oct = quick ? 1 : 2;
    collect_sysinfo(cores, cpu);

    int sel[NEXP], nsel = 0;
    if (only[0]) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", only);
        for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
            int found = -1;
            for (int i = 0; i < NEXP; i++) if (!strcmp(EXPS[i].name, t)) found = i;
            if (found < 0) { printf("unknown experiment '%s' (try --list)\n", t); return 1; }
            sel[nsel++] = found;
        }
    } else {
        for (int i = 0; i < NEXP; i++) if (EXPS[i].in_default) sel[nsel++] = i;
    }
    printf("\nsingle-thread cpu %d | latency-thread cpu %d | load cpus %s | DRAM buffer %llu MiB | threads: bw<=%d, load<=%d | memory cap %llu MB | %s\n",
           C.cpu1, C.lat_cpu, cpu_list(C.load_cpus, C.nload), U(C.dram / MiB), C.tmax, C.nload, U(memmb), quick ? "QUICK" : "FULL");
    if (C.l3 && C.dram < 4 * C.l3)
        printf("WARNING: DRAM-sized buffer (%llu MiB) is < 4x L3 (%llu MiB): raise --mem or DRAM numbers may be partly cache hits.\n", U(C.dram / MiB),
               U(C.l3 / MiB));
    double t0 = now_sec();
    for (int k = 0; k < nsel; k++) {
        const exp_t *e = &EXPS[sel[k]];
        printf("\n=== %s: %s\n", e->name, e->doc);
        fflush(stdout);
        e->fn();
    }
    printf("\nDone: %d tool invocations in %.1f min, %d failures.\nResults are in the data folder next to bin: send me data\\raw and data\\sysinfo.json.\n",
           g_count, (now_sec() - t0) / 60.0, g_fail);
    char path[800];
    snprintf(path, sizeof path, "%s%srun_meta.json", g_data, SEP);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "{\"quick\": %s, \"cpu_single\": %d, \"cpu_latency_thread\": %d, \"dram_bytes\": %llu, \"reps\": %d, \"ms\": %d, \"ms_bw\": %d, "
                   "\"seeds\": %d, \"failures\": %d}\n",
                quick ? "true" : "false", C.cpu1, C.lat_cpu, U(C.dram), C.reps, C.ms, C.msbw, C.nseeds, g_fail);
        fclose(f);
    }
    pause_exit(nopause);
    return g_fail ? 2 : 0;
}