/* Linux pthread-backed opaque PS4 mutex handles. No guest threads yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
typedef struct { int type; } GuestAttr;
/* The game's libc writes into the mutex object itself after scePthreadMutexInit
 * (`or dword [handle+0x20], 0x100`, the PS4 private-mutex flag), so the handle
 * points at a zeroed block the size of a PS4 mutex object, not just at the host
 * mutex. */
#define GUEST_MUTEX_BYTES 0x100
/* owner/depth: which thread holds the mutex and how often, for the cond-wait check only
 * (the host mutex enforces the type's rules). They sit below +0x20, the word libc writes. */
typedef struct {
    pthread_mutex_t native;
    uintptr_t owner;
    uint32_t depth;
    int32_t type;
    unsigned char ps4_fields[GUEST_MUTEX_BYTES - sizeof(pthread_mutex_t) - sizeof(uintptr_t) - 2 * sizeof(uint32_t)];
} GuestMutex;
_Static_assert(sizeof(GuestMutex) == GUEST_MUTEX_BYTES, "GuestMutex must cover the PS4 mutex object");
_Static_assert(offsetof(GuestMutex, ps4_fields) <= 0x20, "libc's word at +0x20 must lie in ps4_fields");
static size_t created, locks, unlocks;
/* A per-thread address identifies the calling thread. */
static _Thread_local char thread_tag;
static uintptr_t self_id(void) { return (uintptr_t)&thread_tag; }
static void note_locked(GuestMutex *m) {
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == self_id()) { ++m->depth; return; }
    m->depth = 1;
    __atomic_store_n(&m->owner, self_id(), __ATOMIC_RELAXED);
}
static void note_unlocked(GuestMutex *m) {
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == self_id() && m->depth > 1) { --m->depth; return; }
    m->depth = 0;
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
}
static int32_t orbis_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case EDEADLK: code=11; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); runtime_quit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
static ABI int32_t attr_init(GuestAttr **out) {
    if (!out) return orbis_error(EINVAL);
    GuestAttr *attr = malloc(sizeof(*attr));
    if (!attr) return orbis_error(ENOMEM);
    attr->type = 1; *out = attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*attr)->type = type; return 0;
}
/* Priority protocols need a PS4 scheduler; the value is validated and recorded only. */
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    return 0;
}
static ABI int32_t attr_destroy(GuestAttr **attr) {
    if (!attr || !*attr) return orbis_error(EINVAL);
    free(*attr); *attr = NULL; return 0;
}
static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return orbis_error(EINVAL);
    int type = attr ? (*attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *mutex = calloc(1, sizeof(*mutex));
    if (!mutex) return orbis_error(ENOMEM);
    mutex->type = type;
    pthread_mutexattr_t native_attr;
    int e = pthread_mutexattr_init(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    int native_type = type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 3 ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
    e = pthread_mutexattr_settype(&native_attr, native_type);
    if (!e) e = pthread_mutex_init(&mutex->native, &native_attr);
    pthread_mutexattr_destroy(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    *out = mutex; ++created; return 0;
}
static pthread_mutex_t static_init = PTHREAD_MUTEX_INITIALIZER;
/* Statically initialized handles (0/1) are created once, even under contention. */
static int32_t ensure_mutex(GuestMutex **mutex) {
    if (!mutex) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)mutex, __ATOMIC_ACQUIRE);
    if (value == 2) return orbis_error(EINVAL);
    if (value >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*mutex < 2) {
        GuestMutex *created_mutex = NULL;
        e = mutex_init(&created_mutex, NULL, NULL);
        if (!e) __atomic_store_n(mutex, created_mutex, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t mutex_lock(GuestMutex **mutex) {
    runtime_freeze_point();
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(pthread_mutex_lock(&(*mutex)->native));
    if (!e) { ++locks; note_locked(*mutex); }
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **mutex) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(pthread_mutex_trylock(&(*mutex)->native));
    if (!e) { ++locks; note_locked(*mutex); }
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return orbis_error(EPERM);
    GuestMutex *m = *mutex;
    /* The holder's bookkeeping goes first: once the host mutex is free another thread may take it. */
    const uintptr_t owner = __atomic_load_n(&m->owner, __ATOMIC_RELAXED);
    const uint32_t depth = m->depth;
    if (owner == self_id()) note_unlocked(m);
    int32_t e = orbis_error(pthread_mutex_unlock(&m->native));
    if (!e) {
        ++unlocks;
        /* NORMAL type: released by a thread that does not hold it; a new holder may already have set its own. */
        uintptr_t expected = owner;
        if (owner != self_id() && owner)
            __atomic_compare_exchange_n(&m->owner, &expected, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    } else if (owner == self_id()) {
        m->depth = depth;
        __atomic_store_n(&m->owner, owner, __ATOMIC_RELAXED);
    }
    return e;
}
static ABI int32_t mutex_destroy(GuestMutex **mutex) {
    runtime_freeze_point();
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return 0;
    int e = pthread_mutex_destroy(&(*mutex)->native);
    if (!e) { free(*mutex); *mutex = (GuestMutex *)(uintptr_t)2; }
    return orbis_error(e);
}
static int deadline_after(struct timespec *end, uint64_t usec) {
    if (clock_gettime(CLOCK_REALTIME, end)) return errno;
    uint64_t ns = (uint64_t)end->tv_nsec + (usec % 1000000) * 1000;
    end->tv_sec += (time_t)(usec / 1000000 + ns / 1000000000);
    end->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
static int32_t timed_error(int e) { return e == ETIMEDOUT ? (int32_t)UINT32_C(0x8002003c) : orbis_error(e); }
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    e = timed_error(pthread_mutex_timedlock(&(*mutex)->native, &end));
    if (!e) { ++locks; note_locked(*mutex); }
    return e;
}
/* Condition variables: opaque handles over host pthread_cond (CLOCK_REALTIME,
 * like FreeBSD's default), sharing the host mutex behind GuestMutex. */
typedef struct { pthread_cond_t native; } GuestCond;
/* A cond wait whose caller does not hold the mutex is logged once (the PS4 fails such a wait with EPERM;
 * here the host wait runs as before). The host wait releases the mutex and takes it again, so the
 * holder's depth is cleared for the wait and put back after it. */
static int unheld_wait_logged;
static uint32_t wait_release(GuestMutex *m, uint64_t caller) {
    const uintptr_t owner = __atomic_load_n(&m->owner, __ATOMIC_RELAXED);
    if (owner != self_id()) {
        if (!__atomic_exchange_n(&unheld_wait_logged, 1, __ATOMIC_RELAXED)) {
            const uintptr_t base = runtime_image_base();
            printf("Runtime: cond wait on a mutex the caller does not hold (%s, type %d) from guest %#llx "
                   "(only the first one is logged)\n",
                   owner ? "held by another thread" : "not held", (int)m->type,
                   (unsigned long long)(caller >= base ? caller - base : caller));
            fflush(stdout);
        }
        return 0;
    }
    const uint32_t depth = m->depth;
    m->depth = 0;
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
    return depth;
}
static void wait_reacquired(GuestMutex *m, uint32_t depth) {
    if (!depth) return;
    m->depth = depth;
    __atomic_store_n(&m->owner, self_id(), __ATOMIC_RELAXED);
}
static size_t conds, waits, wakeups;
static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return orbis_error(EINVAL);
    GuestCond *c = malloc(sizeof(*c));
    if (!c) return orbis_error(ENOMEM);
    int e = pthread_cond_init(&c->native, NULL);
    if (e) { free(c); return orbis_error(e); }
    *out = c; ++conds; return 0;
}
static int32_t ensure_cond(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if (__atomic_load_n((uintptr_t *)cond, __ATOMIC_ACQUIRE) >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*cond < 2) {
        GuestCond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(cond, c, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if ((uintptr_t)*cond < 2) return 0;
    int e = pthread_cond_destroy(&(*cond)->native);
    if (!e) { free(*cond); *cond = NULL; }
    return orbis_error(e);
}
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    runtime_freeze_point();
    runtime_wait_caller((uint64_t)(uintptr_t)__builtin_return_address(0), 0);
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3) return orbis_error(EINVAL);
    ++waits;
    const uint32_t depth = wait_release(*mutex, (uint64_t)(uintptr_t)__builtin_return_address(0));
    const double t0 = runtime_wait_begin();
    const int32_t r = orbis_error(pthread_cond_wait(&(*cond)->native, &(*mutex)->native));
    runtime_wait_end("cond", t0);
    wait_reacquired(*mutex, depth);
    return r;
}
static int32_t cond_wait_until(GuestCond **cond, GuestMutex **mutex, const struct timespec *end,
                               uint64_t caller) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3 || !end) return orbis_error(EINVAL);
    ++waits;
    const uint32_t depth = wait_release(*mutex, caller);
    const double t0 = runtime_wait_begin();
    const int32_t r = timed_error(pthread_cond_timedwait(&(*cond)->native, &(*mutex)->native, end));
    runtime_wait_end("cond timed", t0);
    wait_reacquired(*mutex, depth);
    return r;
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    const uint64_t caller = (uint64_t)(uintptr_t)__builtin_return_address(0);
    runtime_wait_caller(caller, usec);
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    return cond_wait_until(cond, mutex, &end, caller);
}
static ABI int32_t cond_signal(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; return orbis_error(pthread_cond_signal(&(*cond)->native));
}
static ABI int32_t cond_broadcast(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    ++wakeups; return orbis_error(pthread_cond_broadcast(&(*cond)->native));
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static ABI int32_t cond_abs_timedwait(GuestCond **cond, GuestMutex **mutex, const GuestTimespec *abs) {
    const uint64_t caller = (uint64_t)(uintptr_t)__builtin_return_address(0);
    runtime_wait_caller(caller, 1);
    if (!abs || abs->nsec < 0 || abs->nsec >= 1000000000) return orbis_error(EINVAL);
    struct timespec end = {.tv_sec = (time_t)abs->sec, .tv_nsec = (long)abs->nsec};
    return cond_wait_until(cond, mutex, &end, caller);
}
/* Condition attributes carry nothing the host condvar uses (clock and pshared are fixed). */
static char condattr_handle;
static ABI int32_t condattr_init(void **out) {
    if (!out) return orbis_error(EINVAL);
    *out = &condattr_handle; return 0;
}
static ABI int32_t condattr_destroy(void **attr) {
    if (!attr || *attr != &condattr_handle) return orbis_error(EINVAL);
    *attr = NULL; return 0;
}
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
static ABI int32_t posix_attr_init(GuestAttr **out) { return posix_result(attr_init(out)); }
static ABI int32_t posix_attr_type(GuestAttr **attr,int type) { return posix_result(attr_type(attr,type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **attr) { return posix_result(attr_destroy(attr)); }
static ABI int32_t posix_mutex_init(GuestMutex **out,GuestAttr **attr) { return posix_result(mutex_init(out,attr,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) { return posix_result(mutex_lock(m)); }
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock(m)); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock(m)); }
static ABI int32_t posix_condattr_init(void **a) { return posix_result(condattr_init(a)); }
static ABI int32_t posix_condattr_destroy(void **a) { return posix_result(condattr_destroy(a)); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) { return posix_result(cond_wait(c, m)); }
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) { return posix_result(cond_abs_timedwait(c, m, t)); }
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }
uintptr_t runtime_mutex_resolve(const char *name) {
    if (!strcmp(name,"m5-2bsNfv7s#p#J")) return (uintptr_t)condattr_init;
    if (!strcmp(name,"waPcxYiR3WA#p#J")) return (uintptr_t)condattr_destroy;
    if (!strcmp(name,"mKoTx03HRWA#I#J")) return (uintptr_t)posix_condattr_init;
    if (!strcmp(name,"dJcuQVn6-Iw#I#J")) return (uintptr_t)posix_condattr_destroy;
    if (!strcmp(name,"0TyVk4MSLt0#I#J")) return (uintptr_t)posix_cond_init;
    if (!strcmp(name,"RXXqi4CtF8w#I#J")) return (uintptr_t)posix_cond_destroy;
    if (!strcmp(name,"Op8TBGY5KHg#I#J")) return (uintptr_t)posix_cond_wait;
    if (!strcmp(name,"27bAgiJmOh0#I#J")) return (uintptr_t)posix_cond_timedwait;
    if (!strcmp(name,"2MOy+rUfuhQ#I#J")) return (uintptr_t)posix_cond_signal;
    if (!strcmp(name,"mkx2fVhNMsg#I#J")) return (uintptr_t)posix_cond_broadcast;
    if (!strcmp(name,"2Tb92quprl0#p#J")) return (uintptr_t)cond_init;
    if (!strcmp(name,"g+PZd2hiacg#p#J")) return (uintptr_t)cond_destroy;
    if (!strcmp(name,"WKAXJ4XBPQ4#p#J")) return (uintptr_t)cond_wait;
    if (!strcmp(name,"BmMjYxmew1w#p#J")) return (uintptr_t)cond_timedwait;
    if (!strcmp(name,"kDh-NfxgMtE#p#J")) return (uintptr_t)cond_signal;
    if (!strcmp(name,"JGgj7Uvrl+A#p#J")) return (uintptr_t)cond_broadcast;
    if (!strcmp(name,"IafI2PxcPnQ#p#J")) return (uintptr_t)mutex_timedlock;
    if (!strcmp(name,"dQHWEsJtoE4#I#J")) return (uintptr_t)posix_attr_init;
    if (!strcmp(name,"mDmgMOGVUqg#I#J")) return (uintptr_t)posix_attr_type;
    if (!strcmp(name,"HF7lK46xzjY#I#J")) return (uintptr_t)posix_attr_destroy;
    if (!strcmp(name,"ttHNfU+qDBU#I#J")) return (uintptr_t)posix_mutex_init;
    if (!strcmp(name,"ltCfaGr2JGE#I#J")) return (uintptr_t)posix_mutex_destroy;
    if (!strcmp(name,"7H0iTOciTLo#I#J")) return (uintptr_t)posix_mutex_lock;
    if (!strcmp(name,"K-jXhbt2gn4#I#J")) return (uintptr_t)posix_mutex_trylock;
    if (!strcmp(name,"2Z+PpY6CaJg#I#J")) return (uintptr_t)posix_mutex_unlock;
    if (!strcmp(name, "F8bUHwAG284#p#J")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#p#J")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#p#J")) return (uintptr_t)attr_destroy;
    if (!strcmp(name, "1FGvU0i9saQ#p#J")) return (uintptr_t)attr_protocol;
    if (!strcmp(name, "cmo1RIYva9o#p#J")) return (uintptr_t)mutex_init;
    if (!strcmp(name, "9UK1vLZQft4#p#J")) return (uintptr_t)mutex_lock;
    if (!strcmp(name, "upoVrzMHFeE#p#J")) return (uintptr_t)mutex_trylock;
    if (!strcmp(name, "tn3VlD0hG60#p#J")) return (uintptr_t)mutex_unlock;
    if (!strcmp(name, "2Of0f+3mhhE#p#J")) return (uintptr_t)mutex_destroy;
    return 0;
}
void runtime_mutex_report(void) {
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", created, locks, unlocks);
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", conds, waits, wakeups);
}

