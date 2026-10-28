/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
// Native concurrency boundary: actual std::thread/mutex/condition_variable,
// not a scheduler or priority model. Explicit barriers make contracts
// deterministic.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
constexpr int K_FOREVER = -1;
#define K_USEC(value) (value)
struct k_mutex {
    std::mutex value;
};
#define K_MUTEX_DEFINE(name) k_mutex name
inline int k_mutex_lock(k_mutex* m, int) {
    m->value.lock();
    return 0;
}
inline int k_mutex_unlock(k_mutex* m) {
    m->value.unlock();
    return 0;
}
struct k_sem {
    std::mutex mutex;
    std::condition_variable cv;
    bool pending = false, stop = false;
    uint64_t issued = 0, taken = 0, completed = 0;
};
#define K_SEM_DEFINE(name, initial, limit) \
    k_sem name;                            \
    struct name##_registration {           \
        name##_registration() {            \
            native_worker_sem = &name;     \
        }                                  \
    } name##_registered
inline k_sem* native_worker_sem = nullptr;
inline void k_sem_give(k_sem* s) {
    std::lock_guard<std::mutex> guard(s->mutex);
    s->pending = true;
    ++s->issued;
    s->cv.notify_all();
}
struct NativeThreadExit {};
inline int k_sem_take(k_sem* s, int timeout) {
    std::unique_lock<std::mutex> guard(s->mutex);
    s->completed = s->taken;
    s->cv.notify_all();
    bool signaled = true;
    if (timeout == K_FOREVER) {
        s->cv.wait(guard, [&] { return s->pending || s->stop; });
    } else {
        signaled = s->cv.wait_for(guard, std::chrono::microseconds(timeout),
                               [&] { return s->pending || s->stop; });
    }
    if (s->stop) {
        throw NativeThreadExit{};
    }
    s->pending = false;
    s->taken = s->issued;
    return signaled ? 0 : -EAGAIN;
}
struct k_thread {
    std::thread value;
};
inline k_thread* native_worker_thread = nullptr;
#define K_THREAD_STACK_DEFINE(name, size) char name[size]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
inline k_thread* k_thread_create(k_thread* t, char*, size_t,
                                 void (*entry)(void*, void*, void*), void* a,
                                 void* b, void* c, int priority, int, int) {
    assert(priority == 0);
    native_worker_thread = t;
    t->value = std::thread([=] {
        try {
            entry(a, b, c);
        } catch (const NativeThreadExit&) {
        }
    });
    return t;
}
inline int k_thread_name_set(k_thread*, const char*) {
    return 0;
}
inline void NativeFlushCanWorker() {
    auto* s = native_worker_sem;
    assert(s);
    std::unique_lock<std::mutex> guard(s->mutex);
    s->pending = true;
    const auto ticket = ++s->issued;
    s->cv.notify_all();
    assert(s->cv.wait_for(guard, std::chrono::seconds(2),
                          [&] { return s->completed >= ticket; }));
}
inline void NativeStopCanWorker() {
    if (!native_worker_thread) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(native_worker_sem->mutex);
        native_worker_sem->stop = true;
        native_worker_sem->cv.notify_all();
    }
    native_worker_thread->value.join();
}
