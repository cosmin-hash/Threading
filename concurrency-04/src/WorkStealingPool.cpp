// WorkStealingPool.cpp
#include "WorkStealingPool.h"

#include <chrono>
#include <random>

#include <QRandomGenerator>

WorkStealingPool::WorkStealingPool(int workers, int submitMs, int shortMs,
                                   int longMs, QObject* parent)
    : QObject(parent),
      m_count(workers > 0 ? workers : 4),
      m_submitMs(submitMs), m_shortMs(shortMs), m_longMs(longMs) {
    qRegisterMetaType<Task>("Task");   // needed for queued cross-thread signals
}

WorkStealingPool::~WorkStealingPool() { shutdown(); }

void WorkStealingPool::start() {
    if (m_submitter.joinable() || !m_workers.empty()) return;

    m_stopping = false;
    m_next = 0;
    m_pending = 0;
    m_submitted = 0;
    m_ran = 0;
    m_stolen = 0;

    // Fresh, empty deques (one per worker).
    m_deques.clear();
    for (int i = 0; i < m_count; ++i)
        m_deques.push_back(std::make_unique<Deque>());

    for (int i = 0; i < m_count; ++i)
        m_workers.emplace_back([this, i] { workerLoop(i); });
    m_submitter = std::thread([this] { submitLoop(); });
}

void WorkStealingPool::shutdown() {
    if (m_stopping.exchange(true)) {   // already shutting down / never started
        if (m_submitter.joinable()) m_submitter.join();
        for (auto& w : m_workers) if (w.joinable()) w.join();
        return;
    }
    m_wake.notify_all();

    // Stop producing first, then let the workers drain whatever is still pending
    // and exit. No task in flight is abandoned.
    if (m_submitter.joinable()) m_submitter.join();
    for (auto& w : m_workers)
        if (w.joinable()) w.join();
    m_workers.clear();

    emit stopped();
}

// Synthesise the next task. Round-robin home = index % workers; every task whose
// home is worker 0 is HEAVY. With round-robin submit this means worker 0 receives
// a steady stream of expensive tasks while the others get cheap ones -- so the
// others finish early and must steal from worker 0. That is what makes stealing
// visible (idle cores pulling work from the busy one).
Task WorkStealingPool::makeTask(int index) {
    Task t;
    t.id = index + 1;
    t.home = index % m_count;
    t.expensive = (t.home == 0);
    t.shape = QRandomGenerator::global()->bounded(int(ShapeCount));
    const int h = QRandomGenerator::global()->bounded(360);
    t.color = QColor::fromHsv(h, 200 + QRandomGenerator::global()->bounded(56),
                              230 + QRandomGenerator::global()->bounded(26));
    return t;
}

// PRODUCER thread: keep submitting tasks round-robin until stop is requested.
void WorkStealingPool::submitLoop() {
    for (int i = 0; !m_stopping.load(); ++i) {
        Task t = makeTask(i);
        m_deques[t.home]->pushBack(t);
        m_submitted.fetch_add(1, std::memory_order_relaxed);
        m_pending.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(m_wakeMutex);
        }
        m_wake.notify_all();             // wake idle workers to grab/steal it
        emit taskSubmitted(t);

        std::this_thread::sleep_for(std::chrono::milliseconds(m_submitMs));
    }
}

// CONSUMER thread: drain my own deque (LIFO), else steal from a random victim
// (FIFO), else sleep until woken. Exit only once stop is flagged AND every
// submitted task has completed -- so a stop drains the backlog first.
void WorkStealingPool::workerLoop(int id) {
    std::mt19937 rng(id * 2654435761u + 1);
    for (;;) {
        Task task;

        // 1. Try my own deque first (LIFO, contention-free).
        if (m_deques[id]->popBack(task)) {
            runTask(id, id, task);              // ran my OWN task
            continue;
        }

        // 2. My deque is empty -> try to STEAL from a random victim.
        bool stole = false;
        for (int attempt = 0; attempt < m_count; ++attempt) {
            int victim = rng() % m_count;
            if (victim == id) continue;
            if (m_deques[victim]->stealFront(task)) {
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

// Run one task: announce the start, do the work (a sleep sized by the task's
// cost) OUTSIDE any deque lock, announce completion, update counters. `worker`
// is who ran it; `origin` is whose deque it came from (differs => stolen).
void WorkStealingPool::runTask(int worker, int origin, const Task& task) {
    const bool stolen = (worker != origin);
    emit taskStarted(task, worker, stolen);

    std::this_thread::sleep_for(
        std::chrono::milliseconds(task.expensive ? m_longMs : m_shortMs));

    m_ran.fetch_add(1, std::memory_order_relaxed);
    if (stolen) m_stolen.fetch_add(1, std::memory_order_relaxed);
    m_pending.fetch_sub(1, std::memory_order_release);
    emit taskFinished(task, worker, stolen);
}
