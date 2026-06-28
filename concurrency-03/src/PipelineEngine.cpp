// PipelineEngine.cpp
#include "PipelineEngine.h"

#include <chrono>
#include <stdexcept>

#include <QRandomGenerator>

#include "Token.h"   // shared topology constants: kPoolSize, kChannelCap

// How many in-flight pool tasks the transform batches before draining their
// futures. Draining is also what lets chan1 fill and the source feel
// backpressure, so this is a tuning knob for how visible the throttle is.
static constexpr std::size_t kInflightCap = 6;

// ---------------------------------------------------------------------------
// ThreadPool
// ---------------------------------------------------------------------------
namespace {
// Which pool worker the current thread is (set at the top of workerLoop). -1 on
// any non-pool thread. thread_local so each worker reports its own index.
thread_local int t_workerId = -1;
}

ThreadPool::ThreadPool(unsigned n) {
    if (n == 0) n = 4;
    for (unsigned i = 0; i < n; ++i)
        m_workers.emplace_back([this, i] { workerLoop(i); });
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) return;
        m_stopping = true;
    }
    m_cv.notify_all();
    for (auto& w : m_workers)
        if (w.joinable()) w.join();
}

int ThreadPool::currentWorkerId() { return t_workerId; }

void ThreadPool::workerLoop(unsigned id) {
    t_workerId = int(id);
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

// ---------------------------------------------------------------------------
// PipelineEngine
// ---------------------------------------------------------------------------
PipelineEngine::PipelineEngine(int sourceMs, int workMs, QObject* parent)
    : QObject(parent),
      m_chan1(kChannelCap),
      m_chan2(kChannelCap),
      m_sourceMs(sourceMs),
      m_workMs(workMs) {
    qRegisterMetaType<WorkItem>("WorkItem");   // needed for queued cross-thread signals
}

PipelineEngine::~PipelineEngine() { shutdown(); }

void PipelineEngine::start() {
    if (m_source.joinable() || m_transform.joinable() || m_sink.joinable())
        return;
    m_stop.store(false);
    m_produced.store(0);
    m_transformed.store(0);
    m_consumed.store(0);
    m_stalls.store(0);
    m_nextId = 0;

    m_pool = new ThreadPool(kPoolSize);

    // Launch consumers first, producer last, so nothing piles up before the
    // downstream stages are ready (mirrors the skeleton's ordering intent).
    m_sink      = std::thread([this] { sinkLoop(); });
    m_transform = std::thread([this] { transformLoop(); });
    m_source    = std::thread([this] { sourceLoop(); });
}

// COOPERATIVE, deterministic shutdown. Flag stop, close channel 1 (which wakes
// the source blocked in push() and the transform blocked in pop()), then join
// the threads in flow order; closing each channel cascades end-of-stream
// downstream. The pool is shut down LAST, after the transform has joined, so its
// in-flight tasks are never killed mid-work.
void PipelineEngine::shutdown() {
    if (!m_source.joinable() && !m_transform.joinable() && !m_sink.joinable())
        return;

    m_stop.store(true);
    m_chan1.close();                 // wake source (push) + transform (pop)

    if (m_source.joinable())    m_source.join();
    if (m_transform.joinable()) m_transform.join();   // closes chan2 on its way out
    if (m_sink.joinable())      m_sink.join();

    if (m_pool) { m_pool->shutdown(); delete m_pool; m_pool = nullptr; }

    emit stopped();
}

// Synthesise the next work item (source thread only). In a real system this is
// where the source would read from a socket, a file, a sensor, etc.
WorkItem PipelineEngine::makeItem() {
    static const char* topics[] = {"AAPL", "MSFT", "GOOG", "AMZN", "TSLA", "NVDA"};
    WorkItem it;
    it.id = ++m_nextId;
    it.text = QString("%1 tick#%2").arg(topics[m_nextId % 6]).arg(m_nextId);
    it.shape = QRandomGenerator::global()->bounded(int(ShapeCount));
    const int h = QRandomGenerator::global()->bounded(360);
    it.color = QColor::fromHsv(h, 200 + QRandomGenerator::global()->bounded(56),
                               230 + QRandomGenerator::global()->bounded(26));
    return it;
}

// SOURCE: emit work items, throttled by channel 1's backpressure. push() BLOCKS
// while chan1 is full, so the source can never outrun the rest of the pipeline.
void PipelineEngine::sourceLoop() {
    while (!m_stop.load(std::memory_order_relaxed)) {
        WorkItem it = makeItem();
        const WorkItem copy = it;   // keep an identity to announce after the push

        // If this push has to wait on a full channel, that is genuine
        // backpressure — record and announce the stall (the item is NOT dropped;
        // it still goes through once a slot frees).
        const bool pushed = m_chan1.push(std::move(it), [this, copy] {
            m_stalls.fetch_add(1, std::memory_order_relaxed);
            emit backpressure(copy);
        });
        if (!pushed) break;          // channel closed -> stop

        m_produced.fetch_add(1, std::memory_order_relaxed);
        emit itemProduced(copy);

        std::this_thread::sleep_for(std::chrono::milliseconds(m_sourceMs));
    }
    m_chan1.close();   // end-of-stream for the transform stage
}

// TRANSFORM: pull from chan1, fan each item out to the POOL for parallel
// processing, await the results via future, push finished items to chan2.
void PipelineEngine::transformLoop() {
    // result of one pool task: the processed item plus which worker ran it
    struct Done { WorkItem item; int worker; };

    std::vector<std::future<Done>> inflight;

    auto drain = [&] {
        for (auto& f : inflight) {
            Done d = f.get();                  // await the pool result
            m_chan2.push(std::move(d.item));   // may block on chan2 backpressure
            m_transformed.fetch_add(1, std::memory_order_relaxed);
            emit resultBuffered(d.item.id, d.worker);
        }
        inflight.clear();
    };

    while (auto item = m_chan1.pop()) {        // nullopt once chan1 is drained
        WorkItem w = std::move(*item);
        emit transformTook(w.id);

        inflight.push_back(m_pool->submit([this, w]() -> Done {
            const int worker = ThreadPool::currentWorkerId();
            emit workerStarted(w.id, worker);
            // The (simulated heavy) per-item compute, running on a pool thread.
            std::this_thread::sleep_for(std::chrono::milliseconds(m_workMs));
            WorkItem out = w;
            out.text = QString("processed[%1] %2").arg(w.id).arg(w.text);
            return Done{out, worker};
        }));

        // Keep memory bounded: draining the batch is also what lets chan1 fill
        // and the source feel backpressure.
        if (inflight.size() >= kInflightCap)
            drain();
    }
    drain();             // finish any tail tasks
    m_chan2.close();     // end-of-stream for the sink stage
}

// SINK: pull finished items from chan2 until the stream ends.
void PipelineEngine::sinkLoop() {
    while (auto result = m_chan2.pop()) {
        m_consumed.fetch_add(1, std::memory_order_relaxed);
        emit itemConsumed(*result);
    }
}
