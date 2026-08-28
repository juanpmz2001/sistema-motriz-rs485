#ifndef HOST_THREADS_H
#define HOST_THREADS_H

/* Host tests use pthread directly on POSIX. Keep that path unchanged, while
 * providing only the small mutex/condition/thread surface the test fixtures
 * need when they run as native Windows executables under MSVC. */
#if !defined(_WIN32) || defined(__MINGW32__)
#include <pthread.h>
#else

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>

typedef struct {
    CRITICAL_SECTION native;
} pthread_mutex_t;

typedef struct {
    CONDITION_VARIABLE native;
} pthread_cond_t;

typedef struct {
    HANDLE handle;
    void *result;
} pthread_t;

typedef void *(*host_thread_start_t)(void *);

typedef struct {
    host_thread_start_t start;
    void *argument;
    pthread_t *thread;
} host_thread_context_t;

static DWORD WINAPI host_thread_entry(LPVOID argument)
{
    host_thread_context_t *context = argument;
    context->thread->result = context->start(context->argument);
    free(context);
    return 0U;
}

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attributes)
{
    (void) attributes;
    if (!mutex) {
        return EINVAL;
    }
    InitializeCriticalSection(&mutex->native);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    if (!mutex) {
        return EINVAL;
    }
    DeleteCriticalSection(&mutex->native);
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (!mutex) {
        return EINVAL;
    }
    EnterCriticalSection(&mutex->native);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    if (!mutex) {
        return EINVAL;
    }
    LeaveCriticalSection(&mutex->native);
    return 0;
}

static inline int pthread_cond_init(pthread_cond_t *condition, const void *attributes)
{
    (void) attributes;
    if (!condition) {
        return EINVAL;
    }
    InitializeConditionVariable(&condition->native);
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *condition)
{
    return condition ? 0 : EINVAL;
}

static inline int pthread_cond_broadcast(pthread_cond_t *condition)
{
    if (!condition) {
        return EINVAL;
    }
    WakeAllConditionVariable(&condition->native);
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    if (!condition || !mutex) {
        return EINVAL;
    }
    return SleepConditionVariableCS(&condition->native, &mutex->native, INFINITE)
               ? 0
               : EINVAL;
}

static inline int pthread_cond_timedwait(pthread_cond_t *condition,
                                         pthread_mutex_t *mutex,
                                         const struct timespec *deadline)
{
    if (!condition || !mutex || !deadline) {
        return EINVAL;
    }
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return EINVAL;
    }
    int64_t remaining_ms = ((int64_t) deadline->tv_sec - (int64_t) now.tv_sec) * 1000LL +
                           ((int64_t) deadline->tv_nsec - (int64_t) now.tv_nsec) / 1000000LL;
    DWORD timeout = remaining_ms <= 0 ? 0U :
        remaining_ms > (int64_t) UINT32_MAX ? UINT32_MAX : (DWORD) remaining_ms;
    if (SleepConditionVariableCS(&condition->native, &mutex->native, timeout)) {
        return 0;
    }
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}

static inline int pthread_create(pthread_t *thread,
                                 const void *attributes,
                                 host_thread_start_t start,
                                 void *argument)
{
    (void) attributes;
    if (!thread || !start) {
        return EINVAL;
    }
    host_thread_context_t *context = malloc(sizeof(*context));
    if (!context) {
        return ENOMEM;
    }
    *context = (host_thread_context_t) { .start = start, .argument = argument, .thread = thread };
    thread->result = NULL;
    thread->handle = CreateThread(NULL, 0U, host_thread_entry, context, 0U, NULL);
    if (!thread->handle) {
        free(context);
        return EAGAIN;
    }
    return 0;
}

static inline int pthread_join(pthread_t thread, void **result)
{
    if (!thread.handle || WaitForSingleObject(thread.handle, INFINITE) != WAIT_OBJECT_0) {
        return EINVAL;
    }
    if (result) {
        *result = thread.result;
    }
    CloseHandle(thread.handle);
    return 0;
}

#endif
#endif
