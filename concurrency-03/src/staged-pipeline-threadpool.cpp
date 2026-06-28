// pipeline_pool_core.cpp
// ---------------------------------------------------------------------------
// An ADVANCED concurrency backbone, distilled to a single runnable file.
//
// This is a step up from a one-producer/one-consumer drop-queue. It composes
// three reusable primitives that real schedulers (TBB, Tokio, Rayon) are built
// from, then drives them with a 3-stage parallel pipeline:
//
//   SOURCE ---> [channel] ---> TRANSFORM (parallel pool) ---> [channel] ---> SINK
//
// Primitives demonstrated (the "senior concurrency" checklist):
//
//   1. BoundedChannel<T> : a blocking MPMC queue with TWO condition variables
//      (not-full / not-empty). When full it BLOCKS the producer -> true
//      backpressure that THROTTLES (not just drops). close() drains cleanly.
//
//   2. ThreadPool : fixed pool sized to hardware_concurrency(). submit()
//      returns a std::future<R> (via std::packaged_task) so callers compose
//      and await results. Graceful drain-then-join shutdown.
//
//   3. Coordinated pipeline : SOURCE throttled by backpressure, TRANSFORM
//      fanned out across the pool, SINK fanning back in; deterministic
//      shutdown closes channels stage-by-stage so no thread is ever killed
//      mid-work and no consumer outlives its producer.
//
//   std::atomic with explicit memory ordering is used for the cross-thread
//   counters and the running flag.
//
// "Work items" are text strings so the concurrency is visible on the console.
// Build:  g++ -std=c++17 -O2 -pthread pipeline_pool_core.cpp -o pipeline
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ===========================================================================
// 1. BoundedChannel<T> - blocking MPMC queue with backpressure.
// ===========================================================================
template <typename T>
class BoundedChannel {
public:
    explicit BoundedChannel(std::size_t capacity) : m_capacity(capacity) {}

    // Producer side. Blocks while the channel is full (BACKPRESSURE). Returns
    // false if the channel was closed before the item could be pushed.
    bool push(T item) {
        std::unique_lock<std::mutex> lock(m_mutex);
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
// 2. ThreadPool - fixed pool, submit() returns std::future, clean shutdown.
// ===========================================================================
class ThreadPool {
public:
    explicit ThreadPool(unsigned n = std::thread::hardware_concurrency()) {
        if (n == 0) n = 4;
        for (unsigned i = 0; i < n; ++i)
            m_workers.emplace_back([this, i] { workerLoop(i); });
        std::cout << "[pool] started " << n << " workers\n";
    }

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
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) return;
            m_stopping = true;
        }
        m_cv.notify_all();
        for (auto& w : m_workers)
            if (w.joinable()) w.join();
        std::cout << "[pool] all workers joined ("
                  << m_completed.load(std::memory_order_relaxed) << " tasks done)\n";
    }

private:
    void workerLoop(unsigned id) {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
                if (m_stopping && m_tasks.empty()) return;   // drained -> exit
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            task();
            m_completed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::vector<std::thread>          m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex                        m_mutex;
    std::condition_variable           m_cv;
    bool                              m_stopping = false;
    std::atomic<long>                 m_completed{0};
};

// ===========================================================================
// 3. The 3-stage pipeline driven by the primitives above.
//    SOURCE -> chan1 -> TRANSFORM (on the pool) -> chan2 -> SINK
// ===========================================================================
struct WorkItem {
    int         id;
    std::string text;
};

int main() {
    std::cout << "=== Advanced backbone: bounded-channel pipeline + thread pool ===\n\n";

    // Bounded channels between stages (small capacities so backpressure shows).
    BoundedChannel<WorkItem>    chan1(/*capacity=*/4);   // source -> transform
    BoundedChannel<std::string> chan2(/*capacity=*/4);   // transform -> sink

    ThreadPool pool(4);

    std::atomic<long> produced{0}, transformed{0}, consumed{0};

    // ---- SOURCE: one thread emitting work, throttled by chan1's backpressure.
    std::thread source([&] {
        const char* topics[] = {"AAPL","MSFT","GOOG","AMZN","TSLA","NVDA"};
        for (int i = 0; i < 24; ++i) {
            WorkItem item{i, std::string(topics[i % 6]) + " tick#" + std::to_string(i)};
            // push() BLOCKS here when chan1 is full -> source is throttled to
            // the speed the rest of the pipeline can sustain.
            if (!chan1.push(std::move(item))) break;
            produced.fetch_add(1, std::memory_order_relaxed);
        }
        chan1.close();   // end-of-stream for the transform stage
        std::cout << "[source] emitted " << produced.load() << " items, channel closed\n";
    });

    // ---- TRANSFORM: pulls from chan1, fans each item out to the POOL for
    //      parallel processing, awaits the result via future, pushes to chan2.
    std::thread transform([&] {
        std::vector<std::future<std::string>> inflight;
        while (auto item = chan1.pop()) {                 // nullopt when drained
            WorkItem w = std::move(*item);
            // Submit the (simulated heavy) per-item compute to the pool.
            inflight.push_back(pool.submit([w] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                std::ostringstream os;
                os << "processed[" << w.id << "] " << w.text;
                return os.str();
            }));
            // Keep memory bounded: drain completed futures as we go.
            if (inflight.size() >= 8) {
                for (auto& f : inflight) { chan2.push(f.get()); transformed.fetch_add(1, std::memory_order_relaxed); }
                inflight.clear();
            }
        }
        for (auto& f : inflight) { chan2.push(f.get()); transformed.fetch_add(1, std::memory_order_relaxed); }
        chan2.close();   // end-of-stream for the sink stage
        std::cout << "[transform] completed " << transformed.load() << " items, channel closed\n";
    });

    // ---- SINK: pulls finished results from chan2 until the stream ends.
    std::thread sink([&] {
        while (auto result = chan2.pop()) {
            consumed.fetch_add(1, std::memory_order_relaxed);
            std::cout << "  [sink] " << *result << "\n";
        }
        std::cout << "[sink] consumed " << consumed.load() << " results\n";
    });

    // ---- DETERMINISTIC SHUTDOWN: join stages in flow order, then the pool.
    source.join();
    transform.join();
    sink.join();
    pool.shutdown();

    std::cout << "\nproduced=" << produced.load()
              << "  transformed=" << transformed.load()
              << "  consumed=" << consumed.load() << "\n";
    std::cout << "[main] pipeline drained and shut down cleanly\n";
    return 0;
}
