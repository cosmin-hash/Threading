// WorkStealingPool.h
// The threading backbone, a faithful port of work_stealing_pool_core.cpp:
//
//   SUBMIT (round-robin) ─► per-worker DEQUES ─► N WORKER threads ─► completed
//
//   * Each worker owns its OWN double-ended queue (deque). It pushes/pops its own
//     tasks from the BACK (LIFO, cache-friendly, contention-free in the common
//     case).
//   * When a worker's deque is EMPTY it becomes a THIEF: it steals a task from the
//     FRONT of a RANDOM victim's deque (FIFO end, so owner and thief touch
//     opposite ends and rarely collide).
//   * A submitter thread distributes tasks round-robin across the workers; load
//     then self-balances via stealing - idle cores pull work from busy ones.
//   * Each deque is guarded by its own mutex (correct and clear, not lock-free);
//     idle workers sleep on a shared condition variable until a submit wakes them.
//   * Shutdown is COOPERATIVE: flag stop, wake everyone, drain what is pending,
//     then join the submitter and every worker. No thread is killed mid-task.
//
// This class contains NO GUI/painting code -- it only knows about Task and emits
// Qt signals (queued to the GUI thread).
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QObject>

#include "Shapes.h"

class WorkStealingPool : public QObject {
    Q_OBJECT
public:
    WorkStealingPool(int workers, int submitMs, int shortMs, int longMs,
                     QObject* parent = nullptr);
    ~WorkStealingPool() override;

    // Launch the worker threads and the round-robin submitter thread.
    void start();

    // COOPERATIVE shutdown: flag stop, wake all, drain pending, join everyone.
    void shutdown();

    int  workers()  const { return m_count; }
    long submitted() const { return m_submitted.load(); }
    long ran()       const { return m_ran.load(); }
    long stolen()    const { return m_stolen.load(); }

signals:
    // Emitted from the SUBMITTER thread (queued to the GUI):
    void taskSubmitted(Task task);                          // pushed to home deque
    // Emitted from a WORKER thread (queued to the GUI):
    void taskStarted(Task task, int byWorker, bool stolen); // worker began running it
    void taskFinished(Task task, int byWorker, bool stolen);// worker finished it
    void stopped();                                          // all threads joined

private:
    // One deque per worker. Owner uses the BACK (LIFO); thieves use the FRONT
    // (FIFO). Mutex-guarded for clarity (a real one would be lock-free).
    struct Deque {
        mutable std::mutex m;
        std::deque<Task>   q;

        void pushBack(const Task& t) {                 // OWNER: submit / push
            std::lock_guard<std::mutex> lock(m);
            q.push_back(t);
        }
        bool popBack(Task& out) {                      // OWNER: most-recent first
            std::lock_guard<std::mutex> lock(m);
            if (q.empty()) return false;
            out = q.back();
            q.pop_back();
            return true;
        }
        bool stealFront(Task& out) {                   // THIEF: oldest first
            std::lock_guard<std::mutex> lock(m);
            if (q.empty()) return false;
            out = q.front();
            q.pop_front();
            return true;
        }
    };

    void submitLoop();                 // PRODUCER side: round-robin distribute
    void workerLoop(int id);           // CONSUMER side: own deque, else steal
    void runTask(int worker, int origin, const Task& task);  // run + emit + count
    Task makeTask(int index);          // synthesise the next task

    int m_count = 0;
    int m_submitMs;                    // cadence between submits
    int m_shortMs;                     // cheap task duration
    int m_longMs;                      // heavy task duration

    std::vector<std::unique_ptr<Deque>> m_deques;   // one per worker
    std::vector<std::thread>            m_workers;
    std::thread                         m_submitter;

    std::atomic<unsigned> m_next{0};      // round-robin submit cursor
    std::atomic<long>     m_pending{0};   // submitted but not yet completed
    std::atomic<long>     m_submitted{0};
    std::atomic<long>     m_ran{0};
    std::atomic<long>     m_stolen{0};
    std::atomic<bool>     m_stopping{false};

    std::mutex              m_wakeMutex;   // guards the wait below
    std::condition_variable m_wake;        // idle workers sleep here
};
