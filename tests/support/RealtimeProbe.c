#include "RealtimeProbe.h"

#include <dispatch/dispatch.h>
#include <execinfo.h>
#include <fcntl.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// A replacement's own call to the replaced function reaches the original:
// dyld never applies an image's interposing to that image.
#define TALAS_INTERPOSE(replacement, replaced)                                  \
    __attribute__((used)) static const struct                               \
    {                                                                        \
        const void* replacementFunction;                                     \
        const void* replacedFunction;                                        \
    } talasInterpose_##replaced __attribute__((section("__DATA,__interpose"))) = \
        {(const void*)(uintptr_t)&replacement,                               \
            (const void*)(uintptr_t)&replaced}

static atomic_int scopeActive;
static _Atomic(uintptr_t) scopeThread;
static atomic_int capturingStack;
static _Atomic(uint64_t) counts[TalasRealtimeProbeKindCount];
static _Atomic(const char*) firstFunction;
static void* firstFrames[TalasRealtimeProbeMaximumFrames];
static atomic_int firstFrameCount;

static int inScope(void)
{
    return atomic_load_explicit(&scopeActive, memory_order_acquire)
        && atomic_load_explicit(&scopeThread, memory_order_relaxed)
            == (uintptr_t)pthread_self()
        && !atomic_load_explicit(&capturingStack, memory_order_relaxed);
}

static void note(enum TalasRealtimeProbeKind kind, const char* function)
{
    if (!inScope())
        return;
    atomic_fetch_add_explicit(&counts[kind], 1, memory_order_relaxed);
    const char* none = NULL;
    if (!atomic_compare_exchange_strong(&firstFunction, &none, function))
        return;
    // Capturing the stack must not count its own calls.
    atomic_store_explicit(&capturingStack, 1, memory_order_relaxed);
    const int frames = backtrace(firstFrames, TalasRealtimeProbeMaximumFrames);
    atomic_store_explicit(&firstFrameCount, frames, memory_order_relaxed);
    atomic_store_explicit(&capturingStack, 0, memory_order_relaxed);
}

__attribute__((visibility("default"))) void talas_realtime_probe_begin(void)
{
    for (int kind = 0; kind < TalasRealtimeProbeKindCount; ++kind)
        atomic_store_explicit(&counts[kind], 0, memory_order_relaxed);
    atomic_store(&firstFunction, NULL);
    atomic_store(&firstFrameCount, 0);
    atomic_store_explicit(
        &scopeThread, (uintptr_t)pthread_self(), memory_order_relaxed);
    atomic_store_explicit(&scopeActive, 1, memory_order_release);
}

__attribute__((visibility("default"))) void talas_realtime_probe_end(
    struct TalasRealtimeProbeReport* report)
{
    atomic_store_explicit(&scopeActive, 0, memory_order_release);
    if (report == NULL)
        return;
    for (int kind = 0; kind < TalasRealtimeProbeKindCount; ++kind)
    {
        report->counts[kind] =
            atomic_load_explicit(&counts[kind], memory_order_relaxed);
    }
    report->firstFunction = atomic_load(&firstFunction);
    report->firstFrameCount = atomic_load(&firstFrameCount);
    memcpy(report->firstFrames, firstFrames, sizeof firstFrames);
}

static void* probeMalloc(size_t size)
{
    note(TalasRealtimeProbeAllocation, "malloc");
    return malloc(size);
}
TALAS_INTERPOSE(probeMalloc, malloc);

static void* probeCalloc(size_t count, size_t size)
{
    note(TalasRealtimeProbeAllocation, "calloc");
    return calloc(count, size);
}
TALAS_INTERPOSE(probeCalloc, calloc);

static void* probeRealloc(void* pointer, size_t size)
{
    note(TalasRealtimeProbeAllocation, "realloc");
    return realloc(pointer, size);
}
TALAS_INTERPOSE(probeRealloc, realloc);

static int probePosixMemalign(void** pointer, size_t alignment, size_t size)
{
    note(TalasRealtimeProbeAllocation, "posix_memalign");
    return posix_memalign(pointer, alignment, size);
}
TALAS_INTERPOSE(probePosixMemalign, posix_memalign);

static void* probeAlignedAlloc(size_t alignment, size_t size)
{
    note(TalasRealtimeProbeAllocation, "aligned_alloc");
    return aligned_alloc(alignment, size);
}
TALAS_INTERPOSE(probeAlignedAlloc, aligned_alloc);

static void probeFree(void* pointer)
{
    if (pointer != NULL)
        note(TalasRealtimeProbeDeallocation, "free");
    free(pointer);
}
TALAS_INTERPOSE(probeFree, free);

static int probeMutexLock(pthread_mutex_t* mutex)
{
    note(TalasRealtimeProbeLock, "pthread_mutex_lock");
    return pthread_mutex_lock(mutex);
}
TALAS_INTERPOSE(probeMutexLock, pthread_mutex_lock);

static int probeRwlockReadLock(pthread_rwlock_t* lock)
{
    note(TalasRealtimeProbeLock, "pthread_rwlock_rdlock");
    return pthread_rwlock_rdlock(lock);
}
TALAS_INTERPOSE(probeRwlockReadLock, pthread_rwlock_rdlock);

static int probeRwlockWriteLock(pthread_rwlock_t* lock)
{
    note(TalasRealtimeProbeLock, "pthread_rwlock_wrlock");
    return pthread_rwlock_wrlock(lock);
}
TALAS_INTERPOSE(probeRwlockWriteLock, pthread_rwlock_wrlock);

static int probeConditionWait(pthread_cond_t* condition, pthread_mutex_t* mutex)
{
    note(TalasRealtimeProbeLock, "pthread_cond_wait");
    return pthread_cond_wait(condition, mutex);
}
TALAS_INTERPOSE(probeConditionWait, pthread_cond_wait);

static int probeConditionTimedWait(pthread_cond_t* condition,
    pthread_mutex_t* mutex,
    const struct timespec* deadline)
{
    note(TalasRealtimeProbeLock, "pthread_cond_timedwait");
    return pthread_cond_timedwait(condition, mutex, deadline);
}
TALAS_INTERPOSE(probeConditionTimedWait, pthread_cond_timedwait);

static void probeUnfairLock(os_unfair_lock_t lock)
{
    note(TalasRealtimeProbeLock, "os_unfair_lock_lock");
    os_unfair_lock_lock(lock);
}
TALAS_INTERPOSE(probeUnfairLock, os_unfair_lock_lock);

static intptr_t probeSemaphoreWait(
    dispatch_semaphore_t semaphore, dispatch_time_t timeout)
{
    note(TalasRealtimeProbeLock, "dispatch_semaphore_wait");
    return dispatch_semaphore_wait(semaphore, timeout);
}
TALAS_INTERPOSE(probeSemaphoreWait, dispatch_semaphore_wait);

static int probeUsleep(useconds_t microseconds)
{
    note(TalasRealtimeProbeSleep, "usleep");
    return usleep(microseconds);
}
TALAS_INTERPOSE(probeUsleep, usleep);

static int probeNanosleep(const struct timespec* duration, struct timespec* rest)
{
    note(TalasRealtimeProbeSleep, "nanosleep");
    return nanosleep(duration, rest);
}
TALAS_INTERPOSE(probeNanosleep, nanosleep);

static int probeOpen(const char* path, int flags, ...)
{
    int mode = 0;
    if ((flags & O_CREAT) != 0)
    {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
    }
    note(TalasRealtimeProbeFileIo, "open");
    return open(path, flags, mode);
}
TALAS_INTERPOSE(probeOpen, open);

static int probeOpenAt(int directory, const char* path, int flags, ...)
{
    int mode = 0;
    if ((flags & O_CREAT) != 0)
    {
        va_list arguments;
        va_start(arguments, flags);
        mode = va_arg(arguments, int);
        va_end(arguments);
    }
    note(TalasRealtimeProbeFileIo, "openat");
    return openat(directory, path, flags, mode);
}
TALAS_INTERPOSE(probeOpenAt, openat);

static ssize_t probeRead(int descriptor, void* buffer, size_t size)
{
    note(TalasRealtimeProbeFileIo, "read");
    return read(descriptor, buffer, size);
}
TALAS_INTERPOSE(probeRead, read);

static ssize_t probeWrite(int descriptor, const void* buffer, size_t size)
{
    note(TalasRealtimeProbeFileIo, "write");
    return write(descriptor, buffer, size);
}
TALAS_INTERPOSE(probeWrite, write);

static ssize_t probePread(
    int descriptor, void* buffer, size_t size, off_t offset)
{
    note(TalasRealtimeProbeFileIo, "pread");
    return pread(descriptor, buffer, size, offset);
}
TALAS_INTERPOSE(probePread, pread);

static ssize_t probePwrite(
    int descriptor, const void* buffer, size_t size, off_t offset)
{
    note(TalasRealtimeProbeFileIo, "pwrite");
    return pwrite(descriptor, buffer, size, offset);
}
TALAS_INTERPOSE(probePwrite, pwrite);

static int probeClose(int descriptor)
{
    note(TalasRealtimeProbeFileIo, "close");
    return close(descriptor);
}
TALAS_INTERPOSE(probeClose, close);

static FILE* probeFopen(const char* path, const char* mode)
{
    note(TalasRealtimeProbeFileIo, "fopen");
    return fopen(path, mode);
}
TALAS_INTERPOSE(probeFopen, fopen);

static int probeFclose(FILE* file)
{
    note(TalasRealtimeProbeFileIo, "fclose");
    return fclose(file);
}
TALAS_INTERPOSE(probeFclose, fclose);

static size_t probeFread(void* buffer, size_t size, size_t count, FILE* file)
{
    note(TalasRealtimeProbeFileIo, "fread");
    return fread(buffer, size, count, file);
}
TALAS_INTERPOSE(probeFread, fread);

static size_t probeFwrite(
    const void* buffer, size_t size, size_t count, FILE* file)
{
    note(TalasRealtimeProbeFileIo, "fwrite");
    return fwrite(buffer, size, count, file);
}
TALAS_INTERPOSE(probeFwrite, fwrite);

static int probeStat(const char* path, struct stat* status)
{
    note(TalasRealtimeProbeFileIo, "stat");
    return stat(path, status);
}
TALAS_INTERPOSE(probeStat, stat);

static int probeLstat(const char* path, struct stat* status)
{
    note(TalasRealtimeProbeFileIo, "lstat");
    return lstat(path, status);
}
TALAS_INTERPOSE(probeLstat, lstat);

static int probeFstat(int descriptor, struct stat* status)
{
    note(TalasRealtimeProbeFileIo, "fstat");
    return fstat(descriptor, status);
}
TALAS_INTERPOSE(probeFstat, fstat);
