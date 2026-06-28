// work_stealing_pool_core.cpp
// ---------------------------------------------------------------------------
// A WORK-STEALING thread-pool scheduler, distilled to a single runnable file.
//
// This is the step ABOVE a shared-queue thread pool. Instead of every worker
// contending on one central task queue (a lock bottleneck at scale), each
// worker owns its OWN double-ended queue (deque):
//
//   * A worker pushes/pops its own tasks from the BACK of its deque (LIFO,
//     cache-friendly, contention-free in the common case).
//   * When a worker's deque is EMPTY, it becomes a thief: it STEALS a task
//     from the FRONT of a RANDOM victim's deque (FIFO end, so owner and thief
//     touch opposite ends and rarely collide).
//   * Submitted tasks are distributed round-robin across workers; load then
//     self-balances via stealing - idle cores pull work from busy ones.
//
// This is the architecture behind real schedulers (Intel TBB, Go runtime,
// Rust Rayon/Tokio). Here it is mutex-per-deque (correct and clear) rather
// than fully lock-free - the work-stealing STRUCTURE is the lesson, not a
// lock-free deque (which is a separate, much harder topic).
//
// "Tasks" print text so the stealing is visible: each line shows which worker
// RAN a task and whether it was its OWN or STOLEN from another worker.
//
// Build:  g++ -std=c++23 -O2 -pthread -static work_stealing_pool_core.cpp -o work_stealing_pool_core
//         ./work_stealing_pool_core
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ===========================================================================
// WorkStealingDeque - one per worker. Owner uses the BACK (LIFO); thieves use
// the FRONT (FIFO). Mutex-guarded for clarity (a real one would be lock-free).
// ===========================================================================
class WorkStealingDeque {
public:
    using Task = std::function<void()>;

    // OWNER side: push to the back.
    void pushBack(Task t) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deque.push_back(std::move(t));
    }

    // OWNER side: pop from the back (most-recent first -> cache-friendly).
    bool popBack(Task& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_deque.empty()) return false;
        out = std::move(m_deque.back());
        m_deque.pop_back();
        return true;
    }

    // THIEF side: steal from the front (oldest first -> opposite end of owner).
    bool stealFront(Task& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_deque.empty()) return false;
        out = std::move(m_deque.front());
        m_deque.pop_front();
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_deque.empty();
    }

private:
    mutable std::mutex m_mutex;
    std::deque<Task>   m_deque;
};

// ===========================================================================
// WorkStealingPool - N workers, each with its own deque. Round-robin submit,
// self-balancing via stealing, deterministic drain-then-join shutdown.
// ===========================================================================
class WorkStealingPool {
public:
    using Task = std::function<void()>;

    explicit WorkStealingPool(unsigned n = std::thread::hardware_concurrency()) {
        if (n == 0) n = 4;
        m_count = n;
        m_deques = std::vector<WorkStealingDeque>(n);
        for (unsigned i = 0; i < n; ++i)
            m_workers.emplace_back([this, i] { workerLoop(i); });
        std::cout << "[pool] started " << n << " work-stealing workers\n";
    }

    ~WorkStealingPool() { shutdown(); }

    // Distribute submitted tasks round-robin across workers. From there,
    // stealing rebalances the load automatically.
    void submit(Task t) {
        unsigned idx = m_next.fetch_add(1, std::memory_order_relaxed) % m_count;
        m_deques[idx].pushBack(std::move(t));
        m_pending.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_wakeMutex);
        }
        m_wake.notify_all();   // wake idle workers to grab/steal it
    }

    void shutdown() {
        if (m_stopping.exchange(true)) return;
        m_wake.notify_all();
        for (auto& w : m_workers)
            if (w.joinable()) w.join();
        std::cout << "[pool] all workers joined  (ran="
                  << m_ran.load() << ", stolen=" << m_stolen.load() << ")\n";
    }

    // Wait until every submitted task has actually completed.
    void waitIdle() {
        while (m_pending.load(std::memory_order_acquire) > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

private:
    void workerLoop(unsigned id) {
        std::mt19937 rng(id * 2654435761u + 1);
        for (;;) {
            Task task;

            // 1. Try my own deque first (LIFO, contention-free).
            if (m_deques[id].popBack(task)) {
                runTask(id, id, task);              // ran my OWN task
                continue;
            }

            // 2. My deque is empty -> try to STEAL from a random victim.
            bool stole = false;
            for (unsigned attempt = 0; attempt < m_count; ++attempt) {
                unsigned victim = rng() % m_count;
                if (victim == id) continue;
                if (m_deques[victim].stealFront(task)) {
                    m_stolen.fetch_add(1, std::memory_order_relaxed);
                    runTask(id, victim, task);      // ran a STOLEN task
                    stole = true;
                    break;
                }
            }
            if (stole) continue;

            // 3. Nothing to do. Exit if stopping and everything is drained;
            //    otherwise sleep until woken by a new submit.
            if (m_stopping.load() && m_pending.load() == 0)
                return;

            std::unique_lock<std::mutex> lock(m_wakeMutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(2));
        }
    }

    void runTask(unsigned worker, unsigned origin, Task& task) {
        task();
        m_ran.fetch_add(1, std::memory_order_relaxed);
        m_pending.fetch_sub(1, std::memory_order_release);
        if (worker == origin)
            std::cout << "  [worker " << worker << "] ran OWN task\n";
        else
            std::cout << "  [worker " << worker << "] STOLE task from worker "
                      << origin << "\n";
    }

    unsigned                         m_count = 0;
    std::vector<WorkStealingDeque>   m_deques;
    std::vector<std::thread>         m_workers;
    std::atomic<unsigned>            m_next{0};       // round-robin submit cursor
    std::atomic<long>                m_pending{0};     // submitted but not done
    std::atomic<long>                m_ran{0};
    std::atomic<long>                m_stolen{0};
    std::atomic<bool>                m_stopping{false};
    std::mutex                       m_wakeMutex;
    std::condition_variable          m_wake;
};

int main() {
    std::cout << "=== Work-stealing scheduler: per-worker deques + stealing ===\n\n";

    WorkStealingPool pool(4);

    // Submit an UNBALANCED burst: because submit is round-robin but task COSTS
    // vary wildly, some workers finish early and must steal from the others.
    // This is what makes stealing visible - idle cores pull work from busy ones.
    std::cout << "[main] submitting 16 tasks of uneven cost...\n";
    for (int i = 0; i < 16; ++i) {
        int cost = (i % 4 == 0) ? 40 : 5;   // every 4th task is expensive
        pool.submit([i, cost] {
            std::this_thread::sleep_for(std::chrono::milliseconds(cost));
            // (the task's "work" is just sleeping; the print is in runTask)
            (void)i;
        });
    }

    pool.waitIdle();                 // block until all 16 have completed
    std::cout << "[main] all tasks complete; shutting down...\n";
    pool.shutdown();                 // deterministic drain + join

    std::cout << "[main] done\n";
    return 0;
}
