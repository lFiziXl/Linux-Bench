#pragma once

#include <cstddef>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "TileDispatcher.h"

// Persistent fixed-size thread pool.
//
// All worker threads are spawned ONCE in the constructor and joined ONCE in
// the destructor. Between frames they park on a condition variable (zero
// CPU, no wake-up storms), so a long-running benchmark reuses the same OS
// threads instead of recreating them per frame and churning the scheduler.
//
// Work model: one "frame" at a time. executeFrameAndWait() posts the
// per-frame worker lambda, wakes every worker, and blocks the caller until
// the dispatcher is empty and all workers have finished the frame. A frame
// is provably complete when the call returns — the pool is then idle again
// and ready for the next frame.
class ThreadPool {
public:
    using WorkerFn = std::function<void(std::size_t)>;

    // Spawns `thread_count` workers immediately (threads live until
    // destruction). A zero count falls back to one worker, because
    // hardware_concurrency() may legitimately report 0.
    explicit ThreadPool(std::size_t thread_count = std::thread::hardware_concurrency()) {
        if (thread_count == 0) {
            thread_count = 1;
        }
        m_threads.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            m_threads.emplace_back([this, i] { workerLoop(i); });
        }
    }

    // Flags shutdown, wakes every parked worker, and joins all threads —
    // the only place threads are ever joined.
    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shutdown = true;
        }
        m_wakeCV.notify_all();
        for (auto& t : m_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Runs one frame to completion and blocks the caller until it is done.
    //
    // Every worker is woken and calls worker(worker_id) exactly once. The
    // worker lambda owns the frame's work and must consume the dispatcher
    // until it is exhausted: the call returns only when the dispatcher is
    // empty AND every worker has reported back. A worker that stops early
    // without draining the dispatcher leaves the pool blocked — by design,
    // a frame is only ever "complete" when the dispatcher is truly empty.
    // After the call returns the pool is idle again and ready for the next
    // frame.
    //
    // Only one frame may be in flight at a time: concurrent calls, calls
    // with an empty worker, and calls after the pool is destroyed throw.
    void executeFrameAndWait(TileDispatcher& dispatcher, WorkerFn worker) {
        if (!worker) {
            throw std::invalid_argument("ThreadPool::executeFrameAndWait: worker must not be empty");
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shutdown) {
                throw std::logic_error("ThreadPool::executeFrameAndWait: pool is shutting down");
            }
            if (m_frameActive) {
                throw std::logic_error("ThreadPool::executeFrameAndWait: a frame is already in progress");
            }
            m_worker = std::move(worker);
            ++m_frameId;         // new generation: workers that already ran
                                 // the previous frame must not re-enter it
            m_frameActive = true;
            m_activeWorkers = 0;
        }
        m_wakeCV.notify_all();   // outside the lock: workers may proceed right away

        std::unique_lock<std::mutex> lock(m_mutex);
        m_drainCV.wait(lock, [this, &dispatcher] {
            return m_activeWorkers == 0 && dispatcher.empty();
        });

        // All workers are back to sleep. Drop the per-frame lambda now — it
        // may hold references to frame-local objects (e.g. the dispatcher)
        // that are about to be destroyed.
        m_worker = WorkerFn{};
        m_frameActive = false;
    }

    [[nodiscard]] std::size_t threadCount() const noexcept {
        return m_threads.size();
    }

private:
    // Park until a new frame generation appears (or shutdown), run the
    // per-frame worker exactly once, then report back and park again.
    //
    // `processedFrame` is per-thread bookkeeping: it remembers the last
    // frame generation this worker finished, so a worker that is already
    // done with the current frame stays asleep instead of re-entering the
    // frame while the other workers are still draining it.
    void workerLoop(std::size_t worker_id) {
        std::unique_lock<std::mutex> lock(m_mutex);
        std::size_t processedFrame = 0;
        while (true) {
            m_wakeCV.wait(lock, [this, &processedFrame] {
                return m_shutdown || (m_frameActive && processedFrame < m_frameId);
            });
            if (m_shutdown) {
                return; // Destructor is joining us.
            }

            const WorkerFn worker = m_worker; // Copy while holding the lock.
            ++m_activeWorkers;
            lock.unlock();

            worker(worker_id); // Runs until the dispatcher is exhausted.

            lock.lock();
            --m_activeWorkers;
            if (m_activeWorkers == 0) {
                m_drainCV.notify_all();
            }
            processedFrame = m_frameId; // Done with this frame; sleep for the next one.
        }
    }

    std::vector<std::thread> m_threads;

    // All state below is guarded by m_mutex.
    std::mutex              m_mutex;
    std::condition_variable m_wakeCV;   // Workers park here when idle.
    std::condition_variable m_drainCV;  // Callers park here until a frame drains.

    bool         m_shutdown = false;     // Set by the destructor; workers exit.
    bool         m_frameActive = false;  // A frame is posted / in progress.
    std::size_t  m_frameId = 0;          // Frame generation counter.
    std::size_t  m_activeWorkers = 0;    // Workers inside the current frame.
    WorkerFn     m_worker;               // Worker lambda of the current frame.
};
