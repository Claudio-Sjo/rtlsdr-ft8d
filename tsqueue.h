/*
 * tsqueue.h -- minimal thread-safe helpers for the std::vector-based queues
 * used to pass messages between the RX/decoder, PSK-reporter, ncurses UI and
 * QSO-handler threads.
 *
 * std::vector is NOT thread-safe: a producer's push_back may reallocate the
 * backing store while a consumer reads size()/front()/iterates. Previously the
 * consumers tested size() (and sometimes read front()) OUTSIDE the mutex, which
 * is a data race and undefined behaviour. These helpers always perform the
 * size-check, the read and the erase while holding the queue's mutex.
 *
 * Header-only, template-based, no platform assumptions (portable ARM/PC).
 */
#pragma once

#include <pthread.h>
#include <vector>

/* Push one element under the lock. */
template <typename T>
static inline void tsq_push(std::vector<T>& q, pthread_mutex_t* m, const T& v) {
    pthread_mutex_lock(m);
    q.push_back(v);
    pthread_mutex_unlock(m);
}

/* Current size, read under the lock. */
template <typename T>
static inline size_t tsq_size(std::vector<T>& q, pthread_mutex_t* m) {
    pthread_mutex_lock(m);
    size_t n = q.size();
    pthread_mutex_unlock(m);
    return n;
}

/*
 * Pop the front element into *out under the lock.
 * Returns true if an element was popped, false if the queue was empty.
 * The size-check, read and erase are atomic w.r.t. the producer.
 */
template <typename T>
static inline bool tsq_pop(std::vector<T>& q, pthread_mutex_t* m, T* out) {
    bool got = false;
    pthread_mutex_lock(m);
    if (!q.empty()) {
        *out = q.front();
        q.erase(q.begin());
        got = true;
    }
    pthread_mutex_unlock(m);
    return got;
}
