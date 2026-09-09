/*
 * sh_pal_windows.c - Windows backend for sh_pal.h.
 *
 * Compiled only on Windows; see sh_pal_posix.c for the other half. The
 * Makefile picks one.
 *
 * This file is the only place in shared/ that includes <windows.h>, which is
 * the point: sh_pal.h keeps it out of every other translation unit. windows.h
 * defines `near` and `far` as empty macros, which already breaks
 * carta/src/ct_render.c where a local is named `near`.
 */
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <io.h>      /* _isatty, _fileno */
#include <stdio.h>

/* windows.h leaves these behind for 16-bit compatibility and they break any
 * code with a variable of the same name. Nothing below needs them. */
#undef near
#undef far

#include "sh_pal.h"

#include <process.h>
#include <stdlib.h>
#include <string.h>

#define SH_PAL_ASSERT_FITS(Storage, Real, tag)                               \
    typedef char sh_pal_fits_##tag[                                          \
        (sizeof(Storage) >= sizeof(Real) &&                                  \
         _Alignof(Storage) >= _Alignof(Real)) ? 1 : -1]

/*
 * The plain mutex is an SRWLOCK: smaller and faster than a CRITICAL_SECTION,
 * but NOT recursive. The recursive variant is a CRITICAL_SECTION, so both
 * shapes must fit the same storage and each mutex records which one it is.
 */
typedef struct {
    int recursive;
    union {
        SRWLOCK          srw;
        CRITICAL_SECTION cs;
    } u;
} WinMutex;

SH_PAL_ASSERT_FITS(ShMutex,  WinMutex,            mutex);
SH_PAL_ASSERT_FITS(ShCond,   CONDITION_VARIABLE,  cond);
SH_PAL_ASSERT_FITS(ShOnce,   INIT_ONCE,           once);
SH_PAL_ASSERT_FITS(ShTls,    DWORD,               tls);
SH_PAL_ASSERT_FITS(ShThread, HANDLE,              thread);

#define M(p)  ((WinMutex *)(void *)(p)->opaque)
#define CV(p) ((CONDITION_VARIABLE *)(void *)(p)->opaque)
#define O(p)  ((INIT_ONCE *)(void *)(p)->opaque)
#define K(p)  ((DWORD *)(void *)(p)->opaque)
#define T(p)  ((HANDLE *)(void *)(p)->opaque)

/* ------------------------------------------------------------------ Mutex */

int sh_mutex_init(ShMutex *m)
{
    WinMutex *w;
    if (!m) return -1;
    memset(m, 0, sizeof(*m));
    w = M(m);
    w->recursive = 0;
    InitializeSRWLock(&w->u.srw);
    return 0;
}

int sh_mutex_init_recursive(ShMutex *m)
{
    WinMutex *w;
    if (!m) return -1;
    memset(m, 0, sizeof(*m));
    w = M(m);
    w->recursive = 1;
    /* A CRITICAL_SECTION is recursive by definition, which is exactly why the
     * recursive flavour cannot share the SRWLOCK path. */
    InitializeCriticalSection(&w->u.cs);
    return 0;
}

void sh_mutex_lock(ShMutex *m)
{
    WinMutex *w;
    if (!m) return;
    w = M(m);
    if (w->recursive) EnterCriticalSection(&w->u.cs);
    else              AcquireSRWLockExclusive(&w->u.srw);
}

void sh_mutex_unlock(ShMutex *m)
{
    WinMutex *w;
    if (!m) return;
    w = M(m);
    if (w->recursive) LeaveCriticalSection(&w->u.cs);
    else              ReleaseSRWLockExclusive(&w->u.srw);
}

void sh_mutex_destroy(ShMutex *m)
{
    WinMutex *w;
    if (!m) return;
    w = M(m);
    /* An SRWLOCK needs no teardown; a CRITICAL_SECTION does. */
    if (w->recursive) DeleteCriticalSection(&w->u.cs);
}

/* ------------------------------------------------------- Condition variable */

int sh_cond_init(ShCond *c)
{
    if (!c) return -1;
    memset(c, 0, sizeof(*c));
    InitializeConditionVariable(CV(c));
    return 0;
}

/*
 * Windows has separate wait calls for the two lock types, so both paths have
 * to be spelled out.
 */
static BOOL win_cond_wait(ShCond *c, ShMutex *m, DWORD ms)
{
    WinMutex *w = M(m);
    if (w->recursive) {
        return SleepConditionVariableCS(CV(c), &w->u.cs, ms);
    }
    return SleepConditionVariableSRW(CV(c), &w->u.srw, ms, 0);
}

void sh_cond_wait(ShCond *c, ShMutex *m)
{
    if (c && m) (void)win_cond_wait(c, m, INFINITE);
}

int sh_cond_timedwait(ShCond *c, ShMutex *m, uint64_t timeout_ms)
{
    DWORD ms;

    if (!c || !m) return -1;

    /* Relative milliseconds, which is what Windows wants natively -- the
     * reason the public API is relative rather than absolute. */
    ms = (timeout_ms > (uint64_t)(INFINITE - 1)) ? (INFINITE - 1)
                                                 : (DWORD)timeout_ms;

    if (win_cond_wait(c, m, ms)) return 1;
    return (GetLastError() == ERROR_TIMEOUT) ? 0 : -1;
}

void sh_cond_signal(ShCond *c)    { if (c) WakeConditionVariable(CV(c)); }
void sh_cond_broadcast(ShCond *c) { if (c) WakeAllConditionVariable(CV(c)); }
void sh_cond_destroy(ShCond *c)   { (void)c; /* nothing to release */ }

/* ------------------------------------------------------------------- Once */

typedef struct { void (*fn)(void); } OnceCtx;

static BOOL CALLBACK once_trampoline(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    OnceCtx *o = (OnceCtx *)param;
    (void)once; (void)ctx;
    if (o && o->fn) o->fn();
    return TRUE;
}

void sh_once(ShOnce *once, void (*fn)(void))
{
    OnceCtx ctx;
    if (!once || !fn) return;
    ctx.fn = fn;
    /* INIT_ONCE_STATIC_INIT is zero, which is what SH_ONCE_INIT gives, so a
     * statically initialised ShOnce needs no further setup. */
    (void)InitOnceExecuteOnce(O(once), once_trampoline, &ctx, NULL);
}

/* -------------------------------------------------------------------- TLS */

int sh_tls_create(ShTls *key, void (*dtor)(void *))
{
    DWORD idx;
    if (!key) return -1;
    memset(key, 0, sizeof(*key));
    /* FlsAlloc rather than TlsAlloc: TlsAlloc has no destructor support, and
     * the POSIX side promises one. */
    idx = FlsAlloc((PFLS_CALLBACK_FUNCTION)dtor);
    if (idx == FLS_OUT_OF_INDEXES) return -1;
    *K(key) = idx;
    return 0;
}

void sh_tls_destroy(ShTls *key)
{
    if (key) (void)FlsFree(*K(key));
}

void *sh_tls_get(const ShTls *key)
{
    if (!key) return NULL;
    return FlsGetValue(*(const DWORD *)(const void *)key->opaque);
}

int sh_tls_set(ShTls *key, void *value)
{
    if (!key) return -1;
    return FlsSetValue(*K(key), value) ? 0 : -1;
}

/* ---------------------------------------------------------------- Threads */

typedef struct {
    void *(*fn)(void *);
    void *arg;
    void *ret;
} ThreadCtx;

static unsigned __stdcall thread_trampoline(void *param)
{
    ThreadCtx *tc = (ThreadCtx *)param;
    tc->ret = tc->fn(tc->arg);
    return 0;
}

/*
 * The context outlives the thread so join can recover the return value, and
 * is freed by join. A detached-thread API would need a different arrangement;
 * OTTO has exactly one thread-creation site (sh_worker_pool.c) and it joins.
 */
typedef struct {
    HANDLE     handle;
    ThreadCtx *ctx;
} WinThread;

SH_PAL_ASSERT_FITS(ShThread, WinThread, winthread);

int sh_thread_create(ShThread *t, void *(*fn)(void *), void *arg)
{
    WinThread *wt;
    ThreadCtx *tc;
    uintptr_t h;

    if (!t || !fn) return -1;
    memset(t, 0, sizeof(*t));

    tc = (ThreadCtx *)calloc(1, sizeof(*tc));
    if (!tc) return -1;
    tc->fn = fn;
    tc->arg = arg;

    /* _beginthreadex, not CreateThread: the CRT needs its per-thread state
     * initialised, and CreateThread skips that. */
    h = _beginthreadex(NULL, 0, thread_trampoline, tc, 0, NULL);
    if (h == 0) {
        free(tc);
        return -1;
    }

    wt = (WinThread *)(void *)t->opaque;
    wt->handle = (HANDLE)h;
    wt->ctx = tc;
    return 0;
}

int sh_thread_join(ShThread *t, void **retval)
{
    WinThread *wt;

    if (!t) return -1;
    wt = (WinThread *)(void *)t->opaque;
    if (!wt->handle) return -1;

    if (WaitForSingleObject(wt->handle, INFINITE) != WAIT_OBJECT_0) return -1;
    if (retval) *retval = wt->ctx ? wt->ctx->ret : NULL;

    CloseHandle(wt->handle);
    free(wt->ctx);
    wt->handle = NULL;
    wt->ctx = NULL;
    return 0;
}

/* ----------------------------------------------------------------- Clocks */

uint64_t sh_monotonic_ms(void)
{
    LARGE_INTEGER freq, now;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) return 0;
    if (!QueryPerformanceCounter(&now)) return 0;
    return (uint64_t)((now.QuadPart * 1000LL) / freq.QuadPart);
}

uint64_t sh_monotonic_ns(void)
{
    LARGE_INTEGER freq, now;
    uint64_t whole, rest;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0) return 0;
    if (!QueryPerformanceCounter(&now)) return 0;
    /* Split the division: ticks * 1000000000 overflows 64 bits within days
     * on a 10 MHz timer, which is the usual QPC frequency here. */
    whole = (uint64_t)(now.QuadPart / freq.QuadPart);
    rest  = (uint64_t)(now.QuadPart % freq.QuadPart);
    return whole * 1000000000ull + (rest * 1000000000ull) / (uint64_t)freq.QuadPart;
}

uint64_t sh_wall_ms(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* FILETIME counts 100ns ticks from 1601-01-01; shift to the Unix epoch. */
    return (uint64_t)((u.QuadPart - 116444736000000000ull) / 10000ull);
}

uint64_t sh_wall_ns(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100ns FILETIME ticks scale up exactly; the resolution of the underlying
     * clock is coarser than that, but the unit is honest. */
    return (uint64_t)((u.QuadPart - 116444736000000000ull) * 100ull);
}

int sh_stderr_is_tty(void)
{
    return _isatty(_fileno(stderr)) ? 1 : 0;
}

void sh_sleep_ms(unsigned ms)
{
    Sleep((DWORD)ms);
}

int sh_gmtime(int64_t unix_sec, struct tm *out)
{
    time_t t = (time_t)unix_sec;
    if (!out) return -1;
    return gmtime_s(out, &t) == 0 ? 0 : -1;
}

int sh_localtime(int64_t unix_sec, struct tm *out)
{
    time_t t = (time_t)unix_sec;
    if (!out) return -1;
    return localtime_s(out, &t) == 0 ? 0 : -1;
}

/* -------------------------------------------------------------------- CPU */

int sh_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}


/* ---------------------------------------------------------------- Process */

uint64_t sh_pal_pid(void)
{
    return (uint64_t)GetCurrentProcessId();
}

/* ------------------------------------------------------------- Randomness */

int sh_pal_random_bytes(void *buf, size_t len)
{
    if (!buf && len) return -1;
    if (!len) return 0;

    /* BCryptGenRandom with the system-preferred RNG: no handle to manage and
     * no CryptoAPI provider lifetime to get wrong. */
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------ Environment */

int sh_pal_setenv(const char *name, const char *value)
{
    if (!name || !name[0] || strchr(name, '=')) return -1;
    return _putenv_s(name, value ? value : "") == 0 ? 0 : -1;
}

int sh_pal_unsetenv(const char *name)
{
    if (!name || !name[0] || strchr(name, '=')) return -1;
    /* An empty value deletes the variable in the MSVC CRT; getenv() then
     * answers NULL rather than "". */
    return _putenv_s(name, "") == 0 ? 0 : -1;
}


/* ------------------------------------------------------------- Filesystem */

int sh_pal_mkdir(const char *path)
{
    if (!path || !path[0]) return -1;
    if (CreateDirectoryA(path, NULL)) return 0;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
}

int sh_pal_is_dir(const char *path)
{
    DWORD attr;
    if (!path || !path[0]) return 0;
    attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

struct ShPalDir {
    HANDLE           handle;
    WIN32_FIND_DATAA data;
    /* FindFirstFile already returned an entry; hand it out before advancing. */
    int              pending;
};

ShPalDir *sh_pal_dir_open(const char *path)
{
    ShPalDir *d;
    char pattern[MAX_PATH];
    size_t len;

    if (!path || !path[0]) return NULL;

    len = strlen(path);
    /* room for the separator, the wildcard and the terminator */
    if (len + 3 > sizeof(pattern)) return NULL;

    memcpy(pattern, path, len);
    if (path[len - 1] != (char)92 && path[len - 1] != 0x2F) {
        pattern[len++] = 0x2F;
    }
    pattern[len++] = 0x2A;   /* the wildcard FindFirstFile requires */
    pattern[len]   = 0;

    d = (ShPalDir *)calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->handle = FindFirstFileA(pattern, &d->data);
    if (d->handle == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->pending = 1;
    return d;
}

int sh_pal_dir_next(ShPalDir *dir, ShPalDirEntry *out)
{
    if (!dir || !out) return 0;

    if (dir->pending) {
        dir->pending = 0;
    } else if (!FindNextFileA(dir->handle, &dir->data)) {
        return 0;
    }

    out->name   = dir->data.cFileName;
    out->is_dir = (dir->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 1;
}

void sh_pal_dir_close(ShPalDir *dir)
{
    if (!dir) return;
    if (dir->handle != INVALID_HANDLE_VALUE) FindClose(dir->handle);
    free(dir);
}


int sh_pal_temp_dir(char *buf, size_t len)
{
    char tmp[MAX_PATH + 1];
    DWORD n;

    if (!buf || len == 0) return -1;

    /* GetTempPathA returns the length written, or the required size when the
     * buffer is too small -- so a return >= the buffer size is a failure. */
    n = GetTempPathA((DWORD)sizeof(tmp), tmp);
    if (n == 0 || n >= sizeof(tmp)) { buf[0] = '\0'; return -1; }

    /* It always ends in a backslash; drop it so callers can always join with
     * one, matching the POSIX side. */
    while (n > 1 && (tmp[n - 1] == '\\' || tmp[n - 1] == '/')) n--;
    if ((size_t)n >= len) { buf[0] = '\0'; return -1; }

    memcpy(buf, tmp, n);
    buf[n] = '\0';
    return 0;
}


/* --------------------------------------------------------- File mapping */

int sh_map_file_readonly(const char *path, ShFileMap *out)
{
    HANDLE fh, mh;
    LARGE_INTEGER size;
    void *data;

    if (!out) return -1;
    out->data = NULL;
    out->size = 0;
    if (!path || !path[0]) return -1;

    fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return -1;

    if (!GetFileSizeEx(fh, &size) || size.QuadPart <= 0) {
        CloseHandle(fh);
        return -1;
    }

    mh = CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mh) {
        CloseHandle(fh);
        return -1;
    }

    data = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);

    /* The view holds its own references, so both handles can go now and the
     * mapping stays valid until UnmapViewOfFile. That is what lets ShFileMap
     * carry only a pointer and a length. */
    CloseHandle(mh);
    CloseHandle(fh);

    if (!data) return -1;

    out->data = data;
    out->size = (size_t)size.QuadPart;
    return 0;
}

void sh_unmap_file(ShFileMap *map)
{
    if (!map || !map->data) return;
    (void)UnmapViewOfFile(map->data);
    map->data = NULL;
    map->size = 0;
}

void sh_map_advise_sequential(const ShFileMap *map)
{
    /* No-op. PrefetchVirtualMemory has a different shape and needs a
     * WIN32_MEMORY_RANGE_ENTRY plus a Win8+ check, which is not worth it for
     * an advisory hint. */
    (void)map;
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
/* On POSIX this file compiles to nothing. ISO C forbids an empty
 * translation unit, so leave one declaration behind. */
typedef int sh_pal_windows_unused;
#endif /* _WIN32 */
