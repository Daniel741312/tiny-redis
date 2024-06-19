#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <vector>

struct Work {
    void (*f)(void *) = NULL;
    void *arg = NULL;
};

static void* worker(void* arg);

struct ThreadPool {
    std::vector<pthread_t> threads;
    std::deque<Work> queue;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;

    ThreadPool(size_t num_threads) {
        assert(num_threads > 0);

        int rv = pthread_mutex_init(&mu, NULL);
        assert(rv == 0);
        rv = pthread_cond_init(&not_empty, NULL);
        assert(rv == 0);

        threads.resize(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            int rv = pthread_create(&threads[i], NULL, &worker, this);
            assert(rv == 0);
        }
    }

    void enqueue(void (*f)(void *), void *arg) {
        Work w;
        w.f = f;
        w.arg = arg;

        pthread_mutex_lock(&mu);
        queue.push_back(w);
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&mu);
    }
};

static void *worker(void *arg) {
    ThreadPool *tp = (ThreadPool *)arg;
    while (true) {
        pthread_mutex_lock(&tp->mu);
        // wait for the condition: a non-empty queue
        while (tp->queue.empty()) {
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        }

        // got the job
        Work w = tp->queue.front();
        tp->queue.pop_front();
        pthread_mutex_unlock(&tp->mu);

        // do the work
        w.f(w.arg);
    }
    return NULL;
}