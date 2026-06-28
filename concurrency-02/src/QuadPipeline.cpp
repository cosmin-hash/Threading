// QuadPipeline.cpp
#include "QuadPipeline.h"

#include <algorithm>   // std::min
#include <chrono>

#include <QRandomGenerator>

constexpr int QuadPipeline::kPeriods[kPaneCount];

QuadPipeline::QuadPipeline(QObject* parent) : QObject(parent) {
    qRegisterMetaType<Frame>("Frame");   // needed for queued cross-thread signals
    for (int p = 0; p < kPaneCount; ++p)
        m_queues.push_back(std::make_unique<BoundedFrameQueue>(kCapacity));
}

QuadPipeline::~QuadPipeline() { shutdown(); }

void QuadPipeline::start() {
    if (m_consumer.joinable() || !m_producers.empty()) return;
    m_stop = false;
    m_nextId = 0;
    m_produced = m_dropped = m_skipped = m_displayed = 0;

    // One independent producer thread per pane, plus the single consumer.
    for (int p = 0; p < kPaneCount; ++p)
        m_producers.emplace_back([this, p] { produceLoop(p); });
    m_consumer = std::thread([this] { consumeLoop(); });
}

// DETERMINISTIC SHUTDOWN: flag stop, then join every producer and the consumer
// before the queues are torn down -- no producer outlives its queue.
void QuadPipeline::shutdown() {
    m_stop.store(true);
    for (auto& t : m_producers)
        if (t.joinable()) t.join();
    m_producers.clear();
    if (m_consumer.joinable()) m_consumer.join();
    emit stopped();
}

// Pull the next frame for a pane. Here it is synthesised, but this is exactly
// where a real decode worker would read & decode the next frame for its pane.
Frame QuadPipeline::makeFrame(int pane) {
    Frame f;
    f.id   = m_nextId.fetch_add(1);
    f.pane = pane;
    f.shape = QRandomGenerator::global()->bounded(int(ShapeCount));
    const int h = QRandomGenerator::global()->bounded(360);
    f.color = QColor::fromHsv(h, 200 + QRandomGenerator::global()->bounded(56),
                              230 + QRandomGenerator::global()->bounded(26));
    return f;
}

// Sleep for ms, but wake promptly if a stop is requested mid-wait so shutdown
// never blocks for a whole producer/consumer period.
void QuadPipeline::interruptibleSleep(int ms) {
    const int step = 20;
    for (int slept = 0; slept < ms && !m_stop.load(); slept += step)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(std::min(step, ms - slept)));
}

// PRODUCER thread: generate frames at this pane's cadence and push each into the
// pane's own bounded queue. A push when the queue is full evicts the oldest.
void QuadPipeline::produceLoop(int pane) {
    while (!m_stop.load()) {
        Frame f = makeFrame(pane);
        std::vector<Frame> dropped = m_queues[pane]->push(f);
        m_produced.fetch_add(1);
        // Report the drop-oldest evictions first, then the new arrival, so the
        // GUI removes the evicted token before adding the freshly pushed one.
        for (Frame& d : dropped) {
            m_dropped.fetch_add(1);
            emit frameDropped(d);
        }
        emit framePushed(f);
        interruptibleSleep(kPeriods[pane]);
    }
}

// CONSUMER thread: every tick, poll each pane's queue and take the freshest
// frame, discarding any staler ones (like a render loop pulling the latest
// decoded frame per pane to keep display latency low).
void QuadPipeline::consumeLoop() {
    while (!m_stop.load()) {
        interruptibleSleep(kTickMs);
        if (m_stop.load()) break;
        for (int p = 0; p < kPaneCount; ++p) {
            Frame out;
            std::vector<Frame> skipped;
            if (m_queues[p]->popLatest(out, skipped)) {
                for (Frame& s : skipped) {
                    m_skipped.fetch_add(1);
                    emit frameSkipped(s);
                }
                m_displayed.fetch_add(1);
                emit frameDisplayed(out);
            }
        }
    }
}
