// SPDX-FileCopyrightText: 2026 Ben Jarvis
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#include <bcrypt.h>
#define SYMBOL_EXPORT __declspec(dllexport)
#define OTEL_THREAD_LOCAL __declspec(thread)
typedef SRWLOCK otel_mutex;
typedef SRWLOCK otel_rwlock;
typedef CONDITION_VARIABLE otel_cond;
typedef HANDLE otel_thread;
#define otel_mutex_init(p) InitializeSRWLock(p)
#define otel_mutex_destroy(p) ((void) (p))
#define otel_mutex_lock(p) AcquireSRWLockExclusive(p)
#define otel_mutex_unlock(p) ReleaseSRWLockExclusive(p)
#define otel_rwlock_init(p) InitializeSRWLock(p)
#define otel_rwlock_destroy(p) ((void) (p))
#define otel_rwlock_wrlock(p) AcquireSRWLockExclusive(p)
#define otel_rwlock_rdlock(p) AcquireSRWLockShared(p)
#define otel_rwlock_write_unlock(p) ReleaseSRWLockExclusive(p)
#define otel_rwlock_read_unlock(p) ReleaseSRWLockShared(p)
#define otel_cond_init(p) InitializeConditionVariable(p)
#define otel_cond_destroy(p) ((void) (p))
#define otel_cond_signal(p) WakeConditionVariable(p)
static inline void otel_cond_wait_ms(otel_cond *cond, otel_mutex *lock, unsigned int ms)
{
    SleepConditionVariableSRW(cond, lock, ms, 0);
}
struct otel_thread_start { void *(*fn)(void *); void *arg; };
static unsigned int __stdcall otel_thread_entry(void *param)
{
    struct otel_thread_start start = *(struct otel_thread_start *) param;
    free(param);
    start.fn(start.arg);
    return 0;
}
static inline int otel_thread_create(otel_thread *thread, void *(*fn)(void *), void *arg)
{
    struct otel_thread_start *start = malloc(sizeof(*start));
    if (!start) return -1;
    start->fn = fn;
    start->arg = arg;
    *thread = (HANDLE) _beginthreadex(NULL, 0, otel_thread_entry, start, 0, NULL);
    if (!*thread) { free(start); return -1; }
    return 0;
}
static inline void otel_thread_join(otel_thread thread)
{
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
static inline int otel_hostname(char *name, size_t size)
{
    DWORD length = (DWORD) size;
    return GetComputerNameExA(ComputerNameDnsHostname, name, &length) ? 0 : -1;
}
static inline void otel_random_seed(void *seed)
{
    if (BCryptGenRandom(NULL, seed, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) abort();
}
static inline void otel_realtime(struct timespec *ts)
{
    FILETIME ft;
    ULARGE_INTEGER ticks;
    GetSystemTimePreciseAsFileTime(&ft);
    ticks.LowPart = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;
    uint64_t unix_ticks = ticks.QuadPart - UINT64_C(116444736000000000);
    ts->tv_sec = (time_t) (unix_ticks / UINT64_C(10000000));
    ts->tv_nsec = (long) ((unix_ticks % UINT64_C(10000000)) * 100);
}
#else
#include <pthread.h>
#include <unistd.h>
#include <uuid/uuid.h>
#define SYMBOL_EXPORT __attribute__((visibility("default")))
#define OTEL_THREAD_LOCAL _Thread_local
typedef pthread_mutex_t otel_mutex;
typedef pthread_rwlock_t otel_rwlock;
typedef pthread_cond_t otel_cond;
typedef pthread_t otel_thread;
#define otel_mutex_init(p) pthread_mutex_init(p, NULL)
#define otel_mutex_destroy(p) pthread_mutex_destroy(p)
#define otel_mutex_lock(p) pthread_mutex_lock(p)
#define otel_mutex_unlock(p) pthread_mutex_unlock(p)
#define otel_rwlock_init(p) pthread_rwlock_init(p, NULL)
#define otel_rwlock_destroy(p) pthread_rwlock_destroy(p)
#define otel_rwlock_wrlock(p) pthread_rwlock_wrlock(p)
#define otel_rwlock_rdlock(p) pthread_rwlock_rdlock(p)
#define otel_rwlock_write_unlock(p) pthread_rwlock_unlock(p)
#define otel_rwlock_read_unlock(p) pthread_rwlock_unlock(p)
#define otel_cond_init(p) pthread_cond_init(p, NULL)
#define otel_cond_destroy(p) pthread_cond_destroy(p)
#define otel_cond_signal(p) pthread_cond_signal(p)
#define otel_thread_create(p, fn, arg) pthread_create(p, NULL, fn, arg)
#define otel_thread_join(p) pthread_join(p, NULL)
#define otel_hostname(name, size) gethostname(name, size)
#define otel_random_seed(seed) uuid_generate((unsigned char *) (seed))
static inline void otel_realtime(struct timespec *ts) { clock_gettime(CLOCK_REALTIME, ts); }
static inline void otel_cond_wait_ms(otel_cond *cond, otel_mutex *lock, unsigned int ms)
{
    struct timespec ts;
    otel_realtime(&ts);
    ts.tv_nsec += (long) (ms % 1000) * 1000000L;
    ts.tv_sec += (time_t) (ms / 1000) + ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(cond, lock, &ts);
}
#endif

/* Explicit bytes give the gRPC header its five-byte wire layout on every ABI. */
static inline uint32_t otel_load_be32(const uint8_t bytes[4])
{
    return ((uint32_t) bytes[0] << 24) | ((uint32_t) bytes[1] << 16) |
           ((uint32_t) bytes[2] << 8) | bytes[3];
}
static inline void otel_store_be32(uint8_t bytes[4], uint32_t value)
{
    bytes[0] = (uint8_t) (value >> 24);
    bytes[1] = (uint8_t) (value >> 16);
    bytes[2] = (uint8_t) (value >> 8);
    bytes[3] = (uint8_t) value;
}
