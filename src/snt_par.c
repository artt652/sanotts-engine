/* snt_par -- see snt_par.h. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE                 /* sched_getaffinity, CPU_COUNT */
#endif
#include "snt_par.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600         /* SRW locks, condition variables: Vista+ */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK par_mutex;
typedef CONDITION_VARIABLE par_cond;
#define par_lock(m) AcquireSRWLockExclusive(m)
#define par_unlock(m) ReleaseSRWLockExclusive(m)
#define par_wait(c, m) SleepConditionVariableSRW(c, m, INFINITE, 0)
#define par_wake_all(c) WakeAllConditionVariable(c)
#define par_wake_one(c) WakeConditionVariable(c)
static par_mutex mu = SRWLOCK_INIT;
static par_cond cv_job = CONDITION_VARIABLE_INIT, cv_done = CONDITION_VARIABLE_INIT;
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
typedef pthread_mutex_t par_mutex;
typedef pthread_cond_t par_cond;
#define par_lock(m) pthread_mutex_lock(m)
#define par_unlock(m) pthread_mutex_unlock(m)
#define par_wait(c, m) pthread_cond_wait(c, m)
#define par_wake_all(c) pthread_cond_broadcast(c)
#define par_wake_one(c) pthread_cond_signal(c)
static par_mutex mu = PTHREAD_MUTEX_INITIALIZER;
static par_cond cv_job = PTHREAD_COND_INITIALIZER, cv_done = PTHREAD_COND_INITIALIZER;
#endif

static int n_threads = 1;           /* caller + workers */
static unsigned gen;                /* job generation; workers wait for a new one */
static int busy;                    /* workers still in the current job */
static snt_par_fn job_fn;
static void *job_ctx;
static long job_n, job_chunk;
static long job_next;               /* next free index, taken atomically */

static void run_chunks(void) {
    for (;;) {
        long lo = __atomic_fetch_add(&job_next, job_chunk, __ATOMIC_RELAXED);
        if (lo >= job_n) break;
        long hi = lo + job_chunk < job_n ? lo + job_chunk : job_n;
        job_fn(job_ctx, lo, hi);
    }
}

#ifdef _WIN32
static DWORD WINAPI worker(LPVOID arg)
#else
static void *worker(void *arg)
#endif
{
    unsigned seen = 0;
    (void)arg;
    for (;;) {
        par_lock(&mu);
        while (gen == seen) par_wait(&cv_job, &mu);
        seen = gen;
        par_unlock(&mu);
        run_chunks();
        par_lock(&mu);
        if (--busy == 0) par_wake_one(&cv_done);
        par_unlock(&mu);
    }
    return 0;
}

static int start_worker(void) {
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 256 * 1024, worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) return -1;
    CloseHandle(h);
    return 0;
#else
    pthread_t t;
    pthread_attr_t a;
    int rc;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256 * 1024);   /* musl's default is only 128 KB */
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&t, &a, worker, NULL);
    pthread_attr_destroy(&a);
    return rc == 0 ? 0 : -1;
#endif
}

#ifndef _WIN32
/* CPU quota of the cgroup (Docker --cpus, systemd CPUQuota=), rounded up; 0 = none. */
static int cgroup_cpus(void) {
    char buf[128];
    long long q = -1, p = 0;
    FILE *f = fopen("/sys/fs/cgroup/cpu.max", "r");             /* cgroup v2: "max 100000" | "150000 100000" */
    if (f) {
        if (fgets(buf, sizeof buf, f) && strncmp(buf, "max", 3) != 0) sscanf(buf, "%lld %lld", &q, &p);
        fclose(f);
    } else {                                                      /* cgroup v1 */
        static const char *dirs[] = { "/sys/fs/cgroup/cpu", "/sys/fs/cgroup/cpu,cpuacct" };
        for (int i = 0; i < 2 && q < 0; i++) {
            char path[96];
            snprintf(path, sizeof path, "%s/cpu.cfs_quota_us", dirs[i]);
            if (!(f = fopen(path, "r"))) continue;
            if (fscanf(f, "%lld", &q) != 1) q = -1;
            fclose(f);
            snprintf(path, sizeof path, "%s/cpu.cfs_period_us", dirs[i]);
            if ((f = fopen(path, "r"))) { if (fscanf(f, "%lld", &p) != 1) p = 0; fclose(f); }
        }
    }
    if (q <= 0 || p <= 0) return 0;
    return (int)((q + p - 1) / p);
}
#endif

int snt_par_cpus(void) {
    int n = 0;
#ifdef _WIN32
    DWORD_PTR proc = 0, sys = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys))
        for (; proc; proc &= proc - 1) n++;
    if (n <= 0) { SYSTEM_INFO si; GetSystemInfo(&si); n = (int)si.dwNumberOfProcessors; }
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) n = CPU_COUNT(&set);
    if (n <= 0) n = (int)sysconf(_SC_NPROCESSORS_ONLN);
    {
        int q = cgroup_cpus();
        if (q > 0 && q < n) n = q;
    }
#endif
    return n > 0 ? n : 1;
}

int snt_par_init(int n) {
    if (n_threads > 1) return n_threads;                 /* already running */
    if (n <= 0) {
        n = snt_par_cpus();
        if (n > SNT_PAR_AUTO_MAX) n = SNT_PAR_AUTO_MAX;
    }
    if (n > 64) n = 64;
    for (int i = 1; i < n; i++) {
        if (start_worker() != 0) break;                  /* fewer threads, still correct */
        n_threads++;
    }
    return n_threads;
}

int snt_par_threads(void) { return n_threads; }

void snt_par_for(long n, long chunk, snt_par_fn fn, void *ctx) {
    if (n <= 0) return;
    if (chunk < 1) chunk = 1;
    if (n_threads <= 1 || n <= chunk) { fn(ctx, 0, n); return; }
    par_lock(&mu);
    job_fn = fn; job_ctx = ctx; job_n = n; job_chunk = chunk;
    __atomic_store_n(&job_next, 0, __ATOMIC_RELAXED);
    busy = n_threads - 1;
    gen++;
    par_wake_all(&cv_job);
    par_unlock(&mu);
    run_chunks();                                        /* the caller works too */
    par_lock(&mu);
    while (busy > 0) par_wait(&cv_done, &mu);
    par_unlock(&mu);
}
