/* snt_par -- a tiny thread pool for the decoder's convolutions.
 *
 * One pool per process, created once (snt_par_init) and reused by every
 * convolution. snt_par_for() splits [0, n) into chunks that the calling
 * thread and the workers take from a shared counter: faster cores (big.LITTLE
 * phones) simply take more chunks. Every index is processed by exactly one
 * thread with the same code, so results do not depend on the thread count.
 *
 * POSIX threads on Linux/Android (musl, bionic), Win32 threads on Windows.
 * With one thread (or if a thread cannot be created) snt_par_for() runs the
 * whole range in the caller -- the plain single-threaded path.
 */
#ifndef SNT_PAR_H
#define SNT_PAR_H

typedef void (*snt_par_fn)(void *ctx, long lo, long hi);

/* Start the pool. n > 0: that many threads (the caller included); n <= 0:
 * automatic -- CPUs this process may run on (affinity mask, cgroup CPU quota),
 * at most SNT_PAR_AUTO_MAX. Returns the number of threads in use. */
int snt_par_init(int n);
/* Threads in use (1 before snt_par_init). */
int snt_par_threads(void);
/* CPUs available to this process (affinity, cgroup quota), >= 1. */
int snt_par_cpus(void);
/* fn(ctx, lo, hi) over [0, n) in chunks of `chunk` (>= 1). */
void snt_par_for(long n, long chunk, snt_par_fn fn, void *ctx);

#define SNT_PAR_AUTO_MAX 4

#endif
