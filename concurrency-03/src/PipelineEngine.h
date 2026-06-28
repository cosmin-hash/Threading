// PipelineEngine.h
// The threading backbone, ported faithfully from staged-pipeline-threadpool.cpp:
//
//   SOURCE ──► [BoundedChannel 1] ──► TRANSFORM ──► {ThreadPool} ──► [BoundedChannel 2] ──► SINK
//   (producer)   (cap 4, blocks)      (dispatcher)   (4 workers)      (cap 4, blocks)     (consumer)
//
// Three reusable concurrency primitives compose the pipeline:
//
//   1. BoundedChannel<T> — a blocking MPMC queue with TWO condition variables
//      (not-full / not-empty). A full channel BLOCKS the producer: real
//      BACKPRESSURE that THROTTLES the upstream stage (it never drops). close()
//      drains cleanly.
//   2. ThreadPool — fixed pool sized to the pool size; submit() returns a
//      std::future (via std::packaged_task). Graceful drain-then-join shutdown.
//   3. The coordinated pipeline — SOURCE throttled by chan1, TRANSFORM fanning
//      each item out to the pool and fanning the results back in, SINK draining
//      chan2; deterministic shutdown closes channels stage-by-stage so no thread
//      is ever killed mid-work and no consumer outlives its producer.
//
// BoundedChannel and ThreadPool are plain C++17 and know NOTHING about Qt. Only
// PipelineEngine (a QObject) bridges the real threads to the GUI by emitting
// queued signals; it contains no painting code and only knows about WorkItem.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

#include <QObject>

#include "Shapes.h"

// ===========================================================================
// 1. BoundedChannel<T> — blocking MPMC queue with backpressure. (Qt-free.)
// ===========================================================================
template <typename T>
class BoundedChannel {
public:
    explicit BoundedChannel(std::size_t capacity) : m_capacity(capacity) {}

    // Producer side. Blocks while the channel is full (BACKPRESSURE). Returns
    // false if the channel was closed before the item could be pushed.
    // onBlock, if supplied, is invoked once (under the lock) when this push is
    // about to wait on a full channel — purely so a caller can observe the
    // throttle event; it does not change the queue semantics.
    bool push(T item, const std::function<void()>& onBlock = {}) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_queue.size() >= m_capacity && !m_closed && onBlock)
            onBlock();
        m_notFull.wait(lock, [this] { return m_queue.size() < m_capacity || m_closed; });
        if (m_closed) return false;
        m_queue.push(std::move(item));
        m_notEmpty.notify_one();
        return true;
    }

    // Consumer side. Blocks until an item is available. Returns std::nullopt
    // once the channel is closed AND drained (clean end-of-stream signal).
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_notEmpty.wait(lock, [this] { return !m_queue.empty() || m_closed; });
        if (m_queue.empty()) return std::nullopt;   // closed and drained
        T item = std::move(m_queue.front());
        m_queue.pop();
        m_notFull.notify_one();
        return item;
    }

    // No more items will be pushed. Wakes everyone so blocked threads exit.
    void close() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_closed = true;
        }
        m_notFull.notify_all();
        m_notEmpty.notify_all();
    }

private:
    std::mutex              m_mutex;
    std::condition_variable m_notFull;
    std::condition_variable m_notEmpty;
    std::queue<T>           m_queue;
    std::size_t             m_capacity;
    bool                    m_closed = false;
};

// ===========================================================================
// 2. ThreadPool — fixed pool, submit() returns std::future, clean shutdown.
//    (Qt-free.)
// ===========================================================================
class ThreadPool {
public:
    explicit ThreadPool(unsigned n);
    ~ThreadPool() { shutdown(); }

    // Submit a callable; get back a future for its result. Uses packaged_task
    // so any return type composes through std::future<R>.
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) throw std::runtime_error("submit on stopped pool");
            m_tasks.emplace([task] { (*task)(); });
        }
        m_cv.notify_one();
        return fut;
    }

    // Drain queued tasks, then stop and join every worker. Deterministic.
    void shutdown();

    // The pool-worker index (0..n-1) of the thread currently running a task, or
    // -1 if called from outside a worker. Lets a submitted task report which
    // worker picked it up — exactly what the visualiser wants to show.
    static int currentWorkerId();

private:
    void workerLoop(unsigned id);

    std::vector<std::thread>          m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    bool                              m_stopping = false;
    std::atomic<long>                 m_completed{0};
};

// ===========================================================================
// 3. PipelineEngine — the QObject that runs the real threads and reports each
//    genuine hand-off to the GUI through queued signals.
// ===========================================================================
class PipelineEngine : public QObject {
    Q_OBJECT
public:
    PipelineEngine(int sourceMs, int workMs, QObject* parent = nullptr);
    ~PipelineEngine() override;

    void start();      // launch source + transform + sink threads and the pool
    void shutdown();   // COOPERATIVE: stop flag, close channels, join everything

    long produced()    const { return m_produced.load(std::memory_order_relaxed); }
    long transformed() const { return m_transformed.load(std::memory_order_relaxed); }
    long consumed()    const { return m_consumed.load(std::memory_order_relaxed); }
    long stalls()      const { return m_stalls.load(std::memory_order_relaxed); }

signals:
    // SOURCE thread:
    void itemProduced(WorkItem item);     // pushed into channel 1 (token spawns)
    void backpressure(WorkItem item);     // source blocked on a full channel 1
    // TRANSFORM thread:
    void transformTook(int id);           // popped from channel 1 -> dispatcher
    void resultBuffered(int id, int worker); // result pushed into channel 2
    // POOL worker threads:
    void workerStarted(int id, int worker);  // a pool worker began the task
    // SINK thread:
    void itemConsumed(WorkItem item);     // popped from channel 2 and consumed
    void stopped();                       // all threads have joined

private:
    void     sourceLoop();      // produce items, throttled by chan1 backpressure
    void     transformLoop();   // pop chan1 -> pool -> push chan2
    void     sinkLoop();        // pop chan2 until end-of-stream
    WorkItem makeItem();        // synthesise the next item (source thread only)

    BoundedChannel<WorkItem> m_chan1;   // source -> transform
    BoundedChannel<WorkItem> m_chan2;   // transform -> sink
    ThreadPool*              m_pool = nullptr;

    std::thread m_source;
    std::thread m_transform;
    std::thread m_sink;

    std::atomic<bool> m_stop{false};
    std::atomic<long> m_produced{0};
    std::atomic<long> m_transformed{0};
    std::atomic<long> m_consumed{0};
    std::atomic<long> m_stalls{0};

    int m_nextId = 0;     // source-thread only
    int m_sourceMs;       // produce cadence
    int m_workMs;         // heavy per-item processing on the pool
};
