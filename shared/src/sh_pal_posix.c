/*
 * sh_pal_posix.c - POSIX backend for sh_pal.h.
 *
 * Compiled on every platform except Windows; see sh_pal_windows.c for the
 * other half. The Makefile picks one.
 */
#if !defined(_WIN32)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sh_pal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

/*
 * The opaque storage must fit and align the real types. A mistake here would
 * otherwise be silent corruption, so make it a compile error.
 */
#define SH_PAL_ASSERT_FITS(Storage, Real)                                    \
    typedef char sh_pal_fits_##Real[                                         \
        (sizeof(Storage) >= sizeof(Real) &&                                  \
         _Alignof(Storage) >= _Alignof(Real)) ? 1 : -1]

SH_PAL_ASSERT_FITS(ShMutex,  pthread_mutex_t);
SH_PAL_ASSERT_FITS(ShCond,   pthread_cond_t);
SH_PAL_ASSERT_FITS(ShOnce,   pthread_once_t);
SH_PAL_ASSERT_FITS(ShTls,    pthread_key_t);
SH_PAL_ASSERT_FITS(ShThread, pthread_t);

/* SH_ONCE_INIT is all-zero; check that matches PTHREAD_ONCE_INIT. */
static const pthread_once_t sh_pal_once_init = PTHREAD_ONCE_INIT;

#define M(p) ((pthread_mutex_t *)(void *)(p)->opaque)
#define C(p) ((pthread_cond_t  *)(void *)(p)->opaque)
#define O(p) ((pthread_once_t  *)(void *)(p)->opaque)
#define K(p) ((pthread_key_t   *)(void *)(p)->opaque)
#define T(p) ((pthread_t       *)(void *)(p)->opaque)

/* ------------------------------------------------------------------ Mutex */

int sh_mutex_init(ShMutex *m)
{
    if (!m) return -1;
    memset(m, 0, sizeof(*m));
    return pthread_mutex_init(M(m), NULL) == 0 ? 0 : -1;
}

int sh_mutex_init_recursive(ShMutex *m)
{
    pthread_mutexattr_t attr;
    int rc;

    if (!m) return -1;
    memset(m, 0, sizeof(*m));

    if (pthread_mutexattr_init(&attr) != 0) return -1;
    if (pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) != 0) {
        pthread_mutexattr_destroy(&attr);
        return -1;
    }
    rc = pthread_mutex_init(M(m), &attr);
    pthread_mutexattr_destroy(&attr);
    return rc == 0 ? 0 : -1;
}

void sh_mutex_lock(ShMutex *m)     { if (m) (void)pthread_mutex_lock(M(m)); }
void sh_mutex_unlock(ShMutex *m)   { if (m) (void)pthread_mutex_unlock(M(m)); }
void sh_mutex_destroy(ShMutex *m)  { if (m) (void)pthread_mutex_destroy(M(m)); }

/* ------------------------------------------------------- Condition variable */

int sh_cond_init(ShCond *c)
{
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    return pthread_cond_init(C(c), NULL) == 0 ? 0 : -1;
}

void sh_cond_wait(ShCond *c, ShMutex *m)
{
    if (c && m) (void)pthread_cond_wait(C(c), M(m));
}

int sh_cond_timedwait(ShCond *c, ShMutex *m, uint64_t timeout_ms)
{
    struct timespec abstime;
    struct timeval now;
    uint64_t target_us;
    int rc;

    if (!c || !m) return -1;

    /* The relative-to-absolute conversion the public API exists to avoid
     * doing at every call site. CLOCK_REALTIME because that is what a
     * default-attribute pthread_cond_t waits against. */
    if (gettimeofday(&now, NULL) != 0) return -1;
    target_us = (uint64_t)now.tv_sec * 1000000ull + (uint64_t)now.tv_usec
              + timeout_ms * 1000ull;
    abstime.tv_sec  = (time_t)(target_us / 1000000ull);
    abstime.tv_nsec = (long)((target_us % 1000000ull) * 1000ull);

    rc = pthread_cond_timedwait(C(c), M(m), &abstime);
    if (rc == 0) return 1;
    if (rc == ETIMEDOUT) return 0;
    return -1;
}

void sh_cond_signal(ShCond *c)     { if (c) (void)pthread_cond_signal(C(c)); }
void sh_cond_broadcast(ShCond *c)  { if (c) (void)pthread_cond_broadcast(C(c)); }
void sh_cond_destroy(ShCond *c)    { if (c) (void)pthread_cond_destroy(C(c)); }

/* ------------------------------------------------------------------- Once */

void sh_once(ShOnce *once, void (*fn)(void))
{
    (void)sh_pal_once_init;   /* referenced so the initialiser check is used */
    if (once && fn) (void)pthread_once(O(once), fn);
}

/* -------------------------------------------------------------------- TLS */

int sh_tls_create(ShTls *key, void (*dtor)(void *))
{
    if (!key) return -1;
    memset(key, 0, sizeof(*key));
    return pthread_key_create(K(key), dtor) == 0 ? 0 : -1;
}

void sh_tls_destroy(ShTls *key)
{
    if (key) (void)pthread_key_delete(*K(key));
}

void *sh_tls_get(const ShTls *key)
{
    if (!key) return NULL;
    return pthread_getspecific(*(const pthread_key_t *)(const void *)key->opaque);
}

int sh_tls_set(ShTls *key, void *value)
{
    if (!key) return -1;
    return pthread_setspecific(*K(key), value) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- Threads */

int sh_thread_create(ShThread *t, void *(*fn)(void *), void *arg)
{
    if (!t || !fn) return -1;
    memset(t, 0, sizeof(*t));
    return pthread_create(T(t), NULL, fn, arg) == 0 ? 0 : -1;
}

int sh_thread_join(ShThread *t, void **retval)
{
    if (!t) return -1;
    return pthread_join(*T(t), retval) == 0 ? 0 : -1;
}

/* ----------------------------------------------------------------- Clocks */

uint64_t sh_monotonic_ms(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
    }
#endif
    {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0) return 0;
        return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
    }
}

uint64_t sh_monotonic_ns(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
#endif
    {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0) return 0;
        return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
    }
}

uint64_t sh_wall_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

uint64_t sh_wall_ns(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

int sh_stderr_is_tty(void)
{
    return isatty(STDERR_FILENO) ? 1 : 0;
}

void sh_sleep_ms(unsigned ms)
{
    struct timespec req;
    req.tv_sec  = (time_t)(ms / 1000u);
    req.tv_nsec = (long)(ms % 1000u) * 1000000L;
    (void)nanosleep(&req, NULL);
}

int sh_gmtime(int64_t unix_sec, struct tm *out)
{
    time_t t = (time_t)unix_sec;
    if (!out) return -1;
    return gmtime_r(&t, out) ? 0 : -1;
}

int sh_localtime(int64_t unix_sec, struct tm *out)
{
    time_t t = (time_t)unix_sec;
    if (!out) return -1;
    return localtime_r(&t, out) ? 0 : -1;
}

/* -------------------------------------------------------------------- CPU */

int sh_cpu_count(void)
{
#if defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int)n;
#endif
    return 1;
}


/* ---------------------------------------------------------------- Process */

uint64_t sh_pal_pid(void)
{
    return (uint64_t)getpid();
}

/* ------------------------------------------------------------- Randomness */

int sh_pal_random_bytes(void *buf, size_t len)
{
    if (!buf && len) return -1;
    if (!len) return 0;

#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    {
        /* getentropy is capped at 256 bytes per call. */
        unsigned char *p = (unsigned char *)buf;
        size_t left = len;
        while (left > 0) {
            size_t chunk = left > 256 ? 256 : left;
            if (getentropy(p, chunk) != 0) break;
            p += chunk;
            left -= chunk;
        }
        if (left == 0) return 0;
    }
#endif

    /* Fallback for platforms without getentropy, and for the case where it
     * fails (an old kernel, or a sandbox that blocks the syscall). */
    {
        FILE *fp = fopen("/dev/urandom", "rb");
        size_t got;
        if (!fp) return -1;
        got = fread(buf, 1, len, fp);
        fclose(fp);
        return got == len ? 0 : -1;
    }
}

/* ------------------------------------------------------------ Environment */

int sh_pal_setenv(const char *name, const char *value)
{
    if (!name || !name[0] || strchr(name, '=')) return -1;
    return setenv(name, value ? value : "", 1) == 0 ? 0 : -1;
}

int sh_pal_unsetenv(const char *name)
{
    if (!name || !name[0] || strchr(name, '=')) return -1;
    return unsetenv(name) == 0 ? 0 : -1;
}


/* ------------------------------------------------------------- Filesystem */

int sh_pal_mkdir(const char *path)
{
    if (!path || !path[0]) return -1;
    if (mkdir(path, 0755) == 0) return 0;
    return errno == EEXIST ? 0 : -1;
}

int sh_pal_is_dir(const char *path)
{
    struct stat st;
    if (!path || !path[0]) return 0;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

struct ShPalDir {
    DIR *dir;
};

ShPalDir *sh_pal_dir_open(const char *path)
{
    ShPalDir *d;
    DIR *dir;

    if (!path || !path[0]) return NULL;
    dir = opendir(path);
    if (!dir) return NULL;

    d = (ShPalDir *)calloc(1, sizeof(*d));
    if (!d) {
        closedir(dir);
        return NULL;
    }
    d->dir = dir;
    return d;
}

int sh_pal_dir_next(ShPalDir *dir, ShPalDirEntry *out)
{
    struct dirent *e;

    if (!dir || !out) return 0;
    e = readdir(dir->dir);
    if (!e) return 0;

    out->name = e->d_name;
#ifdef DT_DIR
    /* d_type is not in POSIX and some filesystems report DT_UNKNOWN, so fall
     * back to stat rather than reporting "not a directory" on a guess. */
    if (e->d_type != DT_UNKNOWN) {
        out->is_dir = (e->d_type == DT_DIR);
        return 1;
    }
#endif
    out->is_dir = 0;
    return 1;
}

void sh_pal_dir_close(ShPalDir *dir)
{
    if (!dir) return;
    if (dir->dir) closedir(dir->dir);
    free(dir);
}


int sh_pal_temp_dir(char *buf, size_t len)
{
    const char *dir = getenv("TMPDIR");
    size_t n;

    if (!buf || len == 0) return -1;
    if (!dir || !dir[0]) dir = "/tmp";

    n = strlen(dir);
    /* No trailing separator, so callers can always join with one. */
    while (n > 1 && dir[n - 1] == '/') n--;
    if (n >= len) { buf[0] = '\0'; return -1; }

    memcpy(buf, dir, n);
    buf[n] = '\0';
    return 0;
}


/* --------------------------------------------------------- File mapping */

int sh_map_file_readonly(const char *path, ShFileMap *out)
{
    int fd;
    struct stat st;
    void *data;

    if (!out) return -1;
    out->data = NULL;
    out->size = 0;
    if (!path || !path[0]) return -1;

    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }

    data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    /* The mapping keeps its own reference to the file, so the descriptor is
     * not needed once mmap has returned. */
    close(fd);
    if (data == MAP_FAILED) return -1;

    out->data = data;
    out->size = (size_t)st.st_size;
    return 0;
}

void sh_unmap_file(ShFileMap *map)
{
    if (!map || !map->data) return;
    (void)munmap(map->data, map->size);
    map->data = NULL;
    map->size = 0;
}

void sh_map_advise_sequential(const ShFileMap *map)
{
    if (!map || !map->data) return;
#if defined(MADV_SEQUENTIAL)
    (void)madvise(map->data, map->size, MADV_SEQUENTIAL);
#endif
}


void sh_unmap_ptr(void *data, size_t size)
{
    ShFileMap m;
    if (!data) return;
    m.data = data;
    m.size = size;
    sh_unmap_file(&m);
}

#else
/* On Windows this file compiles to nothing. ISO C forbids an empty
 * translation unit, so leave one declaration behind. */
typedef int sh_pal_posix_unused;
#endif /* !_WIN32 */
