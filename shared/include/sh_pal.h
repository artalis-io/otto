/*
 * sh_pal.h - Platform abstraction layer.
 *
 * Threading, timing and CPU queries, with one implementation per platform:
 * shared/src/sh_pal_posix.c and shared/src/sh_pal_windows.c.
 *
 * WHY THIS EXISTS
 *   OTTO targets native Windows as well as Linux, and the core libraries
 *   reach for POSIX directly in about 150 places. Keel is not the answer for
 *   this layer: its public platform surface is networking plus a monotonic
 *   clock and a thread pool, and it exports no mutex, condition variable,
 *   once-init or thread-local storage at all. Depending on it here would also
 *   put a transport vendor underneath Ralph and Shared, and drag it into WASM
 *   builds that have no server. See docs/roadmaps/transport.md, Layer 4.
 *
 * WHY THE TYPES ARE OPAQUE BYTE ARRAYS
 *   So this header does not include <windows.h>. Declaring ShMutex as a
 *   CRITICAL_SECTION would pull windows.h into every translation unit that
 *   touches a lock, and windows.h defines `near` and `far` as empty macros --
 *   which already breaks carta/src/ct_render.c, where a local is named `near`.
 *   Each implementation static-asserts that its real type fits.
 *
 * ERROR CONVENTION
 *   Functions that can fail return 0 on success and -1 on failure. Lock and
 *   unlock cannot meaningfully fail on either platform and return void.
 */
#ifndef SH_PAL_H
#define SH_PAL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Opaque storage
 *
 * Sized for the largest real type on any supported platform, with the union
 * members forcing pointer and 8-byte alignment. Each backend static-asserts
 * that its platform type fits, so a mistake here is a compile error rather
 * than a corruption.
 * ============================================================================ */

#define SH_PAL_STORAGE(bytes)          \
    union {                            \
        unsigned char opaque[bytes];   \
        void         *align_ptr;       \
        long long     align_ll;        \
    }

typedef SH_PAL_STORAGE(64) ShMutex;   /* pthread_mutex_t 40, CRITICAL_SECTION 40 */
typedef SH_PAL_STORAGE(64) ShCond;    /* pthread_cond_t 48, CONDITION_VARIABLE 8 */
typedef SH_PAL_STORAGE(16) ShOnce;    /* pthread_once_t 4, INIT_ONCE 8          */
typedef SH_PAL_STORAGE(16) ShTls;     /* pthread_key_t 4, FLS index 4           */
typedef SH_PAL_STORAGE(16) ShThread;  /* pthread_t 8, HANDLE 8                  */

/*
 * Static initialiser for ShOnce. Zero is the correct initial value for both
 * PTHREAD_ONCE_INIT and INIT_ONCE_STATIC_INIT; the backends assert it.
 */
#define SH_ONCE_INIT { { 0 } }

/* ============================================================================
 * Mutex
 * ============================================================================ */

/* Non-recursive. Re-locking from the same thread deadlocks -- that is the
 * intended behaviour, and the reason the recursive variant is separate. */
int  sh_mutex_init(ShMutex *m);

/*
 * Recursive: the owning thread may lock repeatedly, and must unlock as many
 * times as it locked.
 *
 * Kept separate because it is not free. Windows implements the plain mutex
 * with SRWLOCK, which is smaller and faster but NOT recursive, so a recursive
 * mutex is a CRITICAL_SECTION instead. Reach for this only when re-entrancy is
 * genuinely required; ralph/src/lp_external_adapter.c is the one caller.
 */
int  sh_mutex_init_recursive(ShMutex *m);

void sh_mutex_lock(ShMutex *m);
void sh_mutex_unlock(ShMutex *m);
void sh_mutex_destroy(ShMutex *m);

/* ============================================================================
 * Condition variable
 * ============================================================================ */

int  sh_cond_init(ShCond *c);
void sh_cond_wait(ShCond *c, ShMutex *m);

/*
 * Wait with a RELATIVE timeout in milliseconds.
 *
 * Relative, not absolute, because that is what both callers actually want and
 * what Windows provides: SleepConditionVariableCS takes a relative timeout, so
 * an absolute API would mean converting back at every call. The POSIX backend
 * does the one conversion to an absolute timespec internally.
 *
 * Returns 1 if signalled, 0 if the timeout expired, -1 on error. Spurious
 * wakeups are possible, as always -- re-check your predicate.
 */
int  sh_cond_timedwait(ShCond *c, ShMutex *m, uint64_t timeout_ms);

void sh_cond_signal(ShCond *c);
void sh_cond_broadcast(ShCond *c);
void sh_cond_destroy(ShCond *c);

/* ============================================================================
 * One-time initialisation
 * ============================================================================ */

/* Run `fn` exactly once for this ShOnce, no matter how many threads call. */
void sh_once(ShOnce *once, void (*fn)(void));

/* ============================================================================
 * Thread-local storage
 * ============================================================================ */

/*
 * `dtor`, if non-NULL, runs on thread exit for any non-NULL value. Windows
 * uses fibre-local storage (FlsAlloc) rather than TlsAlloc precisely because
 * TlsAlloc has no destructor support.
 */
int   sh_tls_create(ShTls *key, void (*dtor)(void *));
void  sh_tls_destroy(ShTls *key);
void *sh_tls_get(const ShTls *key);
int   sh_tls_set(ShTls *key, void *value);

/* ============================================================================
 * Threads
 * ============================================================================ */

int sh_thread_create(ShThread *t, void *(*fn)(void *), void *arg);
int sh_thread_join(ShThread *t, void **retval);

/* ============================================================================
 * Clocks
 * ============================================================================ */

/* Milliseconds from an arbitrary fixed origin. Never goes backwards; use this
 * for durations and deadlines, never wall time. */
uint64_t sh_monotonic_ms(void);

/* Nanoseconds from the same origin as sh_monotonic_ms(). For callers timing
 * work too short for a millisecond to resolve; the ms form stays for everyone
 * else. */
uint64_t sh_monotonic_ns(void);

/* Milliseconds since the Unix epoch. Subject to clock adjustments; use this
 * for timestamps, never for measuring elapsed time. */
uint64_t sh_wall_ms(void);

/* Nanoseconds since the Unix epoch. Same caveats as sh_wall_ms(). */
uint64_t sh_wall_ns(void);

/* Thread-safe calendar conversions (gmtime_r / localtime_r, gmtime_s /
 * localtime_s). Return 0 on success, -1 on failure. */
int sh_gmtime(int64_t unix_sec, struct tm *out);
int sh_localtime(int64_t unix_sec, struct tm *out);

/* Whether stderr is attached to a terminal, for deciding on colour output.
 * Returns 1 if it is, 0 otherwise. */
int sh_stderr_is_tty(void);

/* Sleep for at least this many milliseconds. Coarser than nanosleep, which is
 * all any caller here needs -- it is used for poll backoff, not pacing. */
void sh_sleep_ms(unsigned ms);

/* ============================================================================
 * Process
 * ============================================================================ */

/* This process's id. Used for seeding, not for process control. */
uint64_t sh_pal_pid(void);

/* ============================================================================
 * Environment
 * ============================================================================ */

/*
 * Set an environment variable for this process, overwriting any current value.
 *
 * Returns 0 on success, -1 on failure. POSIX setenv(name, value, 1); Windows
 * _putenv_s, which has no "do not overwrite" mode -- so neither does this.
 * Nothing in OTTO wanted one.
 *
 * `name` must not be empty or contain '='.
 */
int sh_pal_setenv(const char *name, const char *value);

/*
 * Remove an environment variable.
 *
 * Returns 0 on success -- including when the variable was not set, matching
 * sh_pal_mkdir's treatment of "already exists" -- and -1 on failure.
 *
 * On Windows this is _putenv_s(name, "") : assigning an empty value is how
 * the CRT deletes a variable, and getenv() then returns NULL rather than "".
 */
int sh_pal_unsetenv(const char *name);

/* ============================================================================
 * Randomness
 * ============================================================================ */

/*
 * Fill `buf` with cryptographically-usable random bytes.
 *
 * Returns 0 on success, -1 if the platform could not supply entropy -- which
 * callers must treat as fatal rather than falling back to something weaker.
 * getrandom/getentropy on POSIX (with a /dev/urandom fallback),
 * BCryptGenRandom on Windows.
 */
int sh_pal_random_bytes(void *buf, size_t len);

/* ============================================================================
 * Filesystem
 * ============================================================================ */

/*
 * Create a single directory.
 *
 * Returns 0 if it was created OR already exists, -1 otherwise. Folding
 * "already exists" into success is deliberate: every caller wanted that, and
 * it keeps errno/GetLastError handling out of them.
 */
int sh_pal_mkdir(const char *path);

/*
 * Directory iteration.
 *
 * POSIX has opendir/readdir; Windows has FindFirstFile, which needs a wildcard
 * appended to the path and reports the first entry from the open call. That is
 * enough of a shape difference that callers cannot paper over it, so it lives
 * here alongside sh_pal_mkdir.
 *
 * Semantics follow readdir: entries arrive in no particular order and "." and
 * ".." are included, because every caller already filters them and inventing a
 * different rule here would surprise the POSIX side.
 *
 *     ShPalDir *d = sh_pal_dir_open(path);
 *     ShPalDirEntry e;
 *     while (sh_pal_dir_next(d, &e)) { ... e.name ... }
 *     sh_pal_dir_close(d);
 */
/* Whether the path exists and is a directory. POSIX spells the test S_ISDIR,
 * which MSVC does not define at all; Windows has a file-attribute bit instead. */
int sh_pal_is_dir(const char *path);

/*
 * Create a uniquely-named file in the platform temp directory.
 *
 * Fills `path` with its full name. The file exists and is ours on return, as
 * mkstemp guarantees; unlike mkstemp it is closed, because every caller here
 * wanted a path rather than a descriptor and closing one to get the other is
 * the dance this replaces.
 *
 * `prefix` seeds the name. Returns 0 on success, -1 on failure.
 */
int sh_pal_make_tempfile(const char *prefix, char *path, size_t path_size);

typedef struct ShPalDir ShPalDir;

typedef struct {
    /* Entry name only, never a path. Valid until the next call on this ShPalDir. */
    const char *name;
    int is_dir;
} ShPalDirEntry;

/* Returns NULL if the directory cannot be opened. */
ShPalDir *sh_pal_dir_open(const char *path);

/* Returns 1 and fills *out for each entry, 0 once the directory is exhausted. */
int sh_pal_dir_next(ShPalDir *dir, ShPalDirEntry *out);

/* Safe on NULL. */
void sh_pal_dir_close(ShPalDir *dir);

/*
 * Write this process's temporary directory into `buf`, with no trailing
 * separator.
 *
 * Returns 0 on success, -1 if it would not fit. POSIX answers $TMPDIR, or
 * "/tmp" when that is unset; Windows answers GetTempPathA, which consults
 * TMP, TEMP and USERPROFILE in turn.
 *
 * A hardcoded "/tmp" is not portable even under MSYS2: a mingw-compiled
 * binary is a native Windows program, so it reads "/tmp/x" as "C:\tmp\x",
 * a directory that need not exist.
 */
int sh_pal_temp_dir(char *buf, size_t len);

/* ============================================================================
 * Read-only file mapping
 * ============================================================================ */

/*
 * A mapped file.
 *
 * Only the base pointer and length are needed to unmap on either platform:
 * POSIX takes both, and a Windows view stays valid after its file and mapping
 * handles are closed, so nothing else has to be kept alive.
 */
typedef struct {
    void  *data;   /* mapped bytes; NULL when nothing is mapped */
    size_t size;   /* bytes mapped */
} ShFileMap;

/*
 * Map an entire file read-only.
 *
 * Returns 0 on success with `out` filled in, -1 on failure with `out` zeroed.
 * Mapping an empty file fails: a zero-length mapping is an error on Windows
 * and useless everywhere, so callers get one consistent answer.
 */
int sh_map_file_readonly(const char *path, ShFileMap *out);

/* Release a mapping. Safe on NULL and on an already-released map. */
void sh_unmap_file(ShFileMap *map);

/*
 * Release a mapping given its base pointer and length.
 *
 * For callers that store those two fields inside their own struct rather
 * than an ShFileMap -- which is most of them, and not worth restructuring
 * their public types over. Safe on NULL.
 */
void sh_unmap_ptr(void *data, size_t size);

/*
 * Hint that access will be sequential.
 *
 * Advisory and best-effort: madvise(MADV_SEQUENTIAL) on POSIX, and nothing at
 * all on Windows, whose prefetch API has a different shape and is not worth
 * the complexity for a hint. Never fails, because no caller should branch on
 * whether a hint was taken.
 */
void sh_map_advise_sequential(const ShFileMap *map);

/* ============================================================================
 * CPU
 * ============================================================================ */

/* Online logical processors; at least 1, never 0, even if the query fails. */
int sh_cpu_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_PAL_H */
