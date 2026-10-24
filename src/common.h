/* common.h - portable helpers (Windows via MinGW-w64 GCC, and Linux) shared by all tools.
 *
 * Timing      : QueryPerformanceCounter / clock_gettime(CLOCK_MONOTONIC)
 * Pinning     : SetThreadAffinityMask / sched_setaffinity
 * Memory      : VirtualAlloc (optionally MEM_LARGE_PAGES) / mmap (optionally THP)
 * Threads     : Win32 threads / pthreads, plus a tiny spin-barrier "window" protocol
 *
 * Requires GCC or Clang (uses __atomic builtins, __attribute__, inline asm in sysprobe.c).
 */
#ifndef CM_COMMON_H
#define CM_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#endif

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#define UNUSED __attribute__((unused))
#define LINE 64

/* ------------------------------------------------------------------ timing */
static UNUSED double now_sec(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static UNUSED void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000u);
#endif
}

static UNUSED inline void cpu_relax(void) {
#if defined(__SSE2__)
    _mm_pause();
#else
    __asm__ volatile("" ::: "memory");
#endif
}

/* ------------------------------------------------------- pinning / priority */
/* cpu < 0 means "do not pin" (used deliberately for the unpinned pitfall run). */
static UNUSED int pin_cpu(int cpu) {
    if (cpu < 0) return 0;
#ifdef _WIN32
    if (cpu >= 64) return -1; /* processor groups (>64 logical CPUs) not supported */
    DWORD_PTR m = SetThreadAffinityMask(GetCurrentThread(), ((DWORD_PTR)1) << cpu);
    return m ? 0 : -1;
#else
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    return sched_setaffinity(0, sizeof(s), &s);
#endif
}

static UNUSED void setup_thread(int cpu) {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    if (pin_cpu(cpu) != 0) fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
}

/* ------------------------------------------------------------------ memory */
typedef struct {
    void  *p;        /* usable base (page aligned, 2 MiB aligned on Linux)   */
    size_t bytes;    /* usable bytes                                         */
    int    huge_req; /* huge/large pages requested                           */
    int    huge_ok;  /* 1 = large pages actually obtained (Windows), -1 = verify later (Linux) */
    void  *raw;      /* Linux: mmap base for munmap                          */
    size_t rawbytes;
} mem_t;

#ifdef _WIN32
static int enable_lock_privilege(void) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return 0;
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &tp.Privileges[0].Luid)) {
        CloseHandle(tok);
        return 0;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    DWORD e = GetLastError();
    CloseHandle(tok);
    return ok && e == ERROR_SUCCESS;
}
#endif

static UNUSED mem_t mem_alloc(size_t bytes, int huge) {
    mem_t m;
    memset(&m, 0, sizeof(m));
    m.huge_req = huge;
#ifdef _WIN32
    if (huge) {
        size_t lp = GetLargePageMinimum();
        if (lp && enable_lock_privilege()) {
            size_t b = (bytes + lp - 1) / lp * lp;
            void *p = VirtualAlloc(NULL, b, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
            if (p) {
                m.p = p; m.bytes = b; m.huge_ok = 1;
                return m;
            }
        }
        /* fall through to normal pages; huge_ok stays 0 so the CSV shows the request failed */
    }
    m.p = VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    m.bytes = bytes;
    return m;
#else
    size_t al = (size_t)2 << 20;
    size_t b = (bytes + al - 1) / al * al;
    void *raw = mmap(NULL, b + al, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return m;
    uintptr_t base = ((uintptr_t)raw + al - 1) & ~(uintptr_t)(al - 1);
    madvise((void *)base, b, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    m.raw = raw; m.rawbytes = b + al;
    m.p = (void *)base; m.bytes = b;
    m.huge_ok = huge ? -1 : 0;
    return m;
#endif
}

static UNUSED void mem_free(mem_t *m) {
    if (!m->p) return;
#ifdef _WIN32
    VirtualFree(m->p, 0, MEM_RELEASE);
#else
    munmap(m->raw, m->rawbytes);
#endif
    m->p = NULL;
}

/* After the buffer has been written: did we really get huge pages?  (process-wide on Linux) */
static UNUSED int mem_huge_verified(const mem_t *m) {
    if (!m->huge_req) return 0;
#ifdef _WIN32
    return m->huge_ok == 1;
#else
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "AnonHugePages:", 14) == 0) { kb = atol(line + 14); break; }
    fclose(f);
    return kb > 0 ? 1 : 0;
#endif
}

/* --------------------------------------------------------------------- rng */
typedef struct { uint64_t s; } rng_t;
static UNUSED inline uint64_t rng_next(rng_t *r) {
    uint64_t x = r->s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}
static UNUSED void shuffle_u32(uint32_t *a, size_t n, rng_t *r) {
    for (size_t i = n; i > 1; i--) {
        size_t j = (size_t)(rng_next(r) % i);
        uint32_t t = a[i - 1]; a[i - 1] = a[j]; a[j] = t;
    }
}

/* -------------------------------------------------------------------- args */
/* arguments look like key=value ; a bare flag such as -H is looked up with arg_has */
static UNUSED const char *arg_str(int argc, char **argv, const char *key, const char *def) {
    size_t kl = strlen(key);
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], key, kl) == 0 && argv[i][kl] == '=') return argv[i] + kl + 1;
    return def;
}
static UNUSED int arg_has(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], flag) == 0) return 1;
    return 0;
}
static UNUSED size_t parse_size(const char *s) {
    char *e;
    double v = strtod(s, &e);
    switch (toupper((unsigned char)*e)) {
        case 'K': v *= 1024.0; break;
        case 'M': v *= 1048576.0; break;
        case 'G': v *= 1073741824.0; break;
        default: break;
    }
    return (size_t)v;
}
static UNUSED size_t arg_size(int argc, char **argv, const char *key, const char *def) {
    return parse_size(arg_str(argc, argv, key, def));
}
static UNUSED long arg_long(int argc, char **argv, const char *key, long def) {
    const char *s = arg_str(argc, argv, key, NULL);
    return s ? strtol(s, NULL, 10) : def;
}
static UNUSED double arg_dbl(int argc, char **argv, const char *key, double def) {
    const char *s = arg_str(argc, argv, key, NULL);
    return s ? strtod(s, NULL) : def;
}
/* "2,4,6" -> out[]; returns count */
static UNUSED int parse_cpu_list(const char *s, int *out, int max) {
    int n = 0;
    if (!s || !*s) return 0;
    while (*s && n < max) {
        out[n++] = (int)strtol(s, (char **)&s, 10);
        if (*s == ',') s++; else break;
    }
    return n;
}

/* ----------------------------------------------------------------- atomics */
#define ATOMIC_ADD(p, v) __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define ATOMIC_LOAD(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define ATOMIC_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)

/* ----------------------------------------------------------------- threads */
typedef void (*thr_fn)(void *);
#ifdef _WIN32
typedef struct { HANDLE h; thr_fn fn; void *arg; } thr_t;
static DWORD WINAPI thr_tramp(LPVOID p) { thr_t *t = (thr_t *)p; t->fn(t->arg); return 0; }
static UNUSED void thr_start(thr_t *t, thr_fn fn, void *arg) {
    t->fn = fn; t->arg = arg;
    t->h = CreateThread(NULL, 0, thr_tramp, t, 0, NULL);
}
static UNUSED void thr_join(thr_t *t) { WaitForSingleObject(t->h, INFINITE); CloseHandle(t->h); }
#else
typedef struct { pthread_t h; thr_fn fn; void *arg; } thr_t;
static void *thr_tramp(void *p) { thr_t *t = (thr_t *)p; t->fn(t->arg); return NULL; }
static UNUSED void thr_start(thr_t *t, thr_fn fn, void *arg) {
    t->fn = fn; t->arg = arg;
    pthread_create(&t->h, NULL, thr_tramp, t);
}
static UNUSED void thr_join(thr_t *t) { pthread_join(t->h, NULL); }
#endif

/* Measurement-window protocol.
 * Workers: allocate + initialise, sync_ready(), then for each window g=1,2,..:
 *          sync_wait_gen(g); run while sync_running(g); record; sync_done().
 * Main   : sync_wait_ready(n); for each window: sync_window(g, n, ms).            */
typedef struct { volatile long gen, stop, ready, done; } sync_t;
static UNUSED void sync_ready(sync_t *s) { ATOMIC_ADD(&s->ready, 1); }
static UNUSED void sync_wait_gen(sync_t *s, long g) { while (ATOMIC_LOAD(&s->gen) < g) cpu_relax(); }
static UNUSED int  sync_running(sync_t *s, long g) { return ATOMIC_LOAD(&s->stop) < g; }
static UNUSED void sync_done(sync_t *s) { ATOMIC_ADD(&s->done, 1); }
static UNUSED void sync_wait_ready(sync_t *s, long n) { while (ATOMIC_LOAD(&s->ready) < n) sleep_ms(1); }
static UNUSED void sync_window(sync_t *s, long g, long n, int ms) {
    ATOMIC_STORE(&s->done, 0);
    ATOMIC_STORE(&s->gen, g);
    sleep_ms(ms);
    ATOMIC_STORE(&s->stop, g);
    while (ATOMIC_LOAD(&s->done) < n) sleep_ms(1);
}

#endif /* CM_COMMON_H */
