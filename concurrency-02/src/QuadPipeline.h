// QuadPipeline.h
// The threading backbone, distilled from multi-producer-bounded-queue.cpp:
//
//   N independent PRODUCER threads (one decode worker per pane)
//        |  each pushes into its OWN bounded, thread-safe queue
//        v
//   BoundedFrameQueue  (mutex-guarded, capacity kCapacity) -- one per pane
//        |  DROP-OLDEST backpressure: when full, the oldest frame is discarded
//        |  so a slow consumer can never grow memory unbounded
//        v
//   1 CONSUMER thread (the render loop) takes the FRESHEST frame per pane and
//        discards (SKIPS) any staler ones still queued (low display latency)
//
//   * Each producer and the consumer run on their own std::thread.
//   * Shutdown is COOPERATIVE/DETERMINISTIC: flag stop, then join every thread,
//     THEN the queues are destroyed -- no producer can outlive its queue.
//
// This class owns the genuine threading and knows nothing about painting; it
// only deals in Frame and emits queued Qt signals for the GUI to animate.
#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QObject>

#include "Shapes.h"
#include "Token.h"   // for kPaneCount / kCapacity

// ---------------------------------------------------------------------------
// BoundedFrameQueue: bounded, thread-safe buffer (mirrors the skeleton).
//   - push():      producer side. Drops the OLDEST frame(s) when full and
//                  returns them so the GUI can animate the backpressure.
//   - popLatest(): consumer side. Returns the freshest frame and reports the
//                  older frames it skipped to get to it.
// ---------------------------------------------------------------------------
class BoundedFrameQueue {
public:
    explicit BoundedFrameQueue(int capacity = kCapacity) : m_capacity(capacity) {}

    // Producer side. Returns the frames evicted (drop-oldest) to make room.
    std::vector<Frame> push(Frame frame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Frame> dropped;
        while (static_cast<int>(m_queue.size()) >= m_capacity) {
            dropped.push_back(std::move(m_queue.front()));
            m_queue.pop_front();             // drop the OLDEST -> backpressure
        }
        m_queue.push_back(std::move(frame));
        return dropped;
    }

    // Consumer side. Hands back the most recent frame and fills `skipped` with
    // the older frames (front..back-1) that were bypassed to reach it.
    bool popLatest(Frame& out, std::vector<Frame>& skipped) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;
        out = std::move(m_queue.back());
        m_queue.pop_back();
        while (!m_queue.empty()) {           // everything older is stale
            skipped.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        return true;
    }

private:
    mutable std::mutex m_mutex;
    std::deque<Frame>  m_queue;
    int                m_capacity;
};

class QuadPipeline : public QObject {
    Q_OBJECT
public:
    explicit QuadPipeline(QObject* parent = nullptr);
    ~QuadPipeline() override;

    // Launch the N producer threads and the single consumer thread.
    void start();

    // COOPERATIVE shutdown: flag stop, then join every thread.
    void shutdown();

    long produced() const { return m_produced.load(); }
    long dropped()  const { return m_dropped.load(); }
    long skipped()  const { return m_skipped.load(); }
    long displayed() const { return m_displayed.load(); }

signals:
    // Emitted from a PRODUCER thread (queued to the GUI):
    void framePushed(Frame frame);     // accepted into its bounded queue
    void frameDropped(Frame frame);    // evicted by drop-oldest backpressure
    // Emitted from the CONSUMER thread (queued to the GUI):
    void frameDisplayed(Frame frame);  // freshest frame taken & shown
    void frameSkipped(Frame frame);    // older frame bypassed as stale
    void stopped();                    // all threads have joined

private:
    void  produceLoop(int pane);   // one independent producer per pane
    void  consumeLoop();           // single consumer over all queues
    Frame makeFrame(int pane);     // synthesise the next frame for a pane
    void  interruptibleSleep(int ms);  // sleep that wakes early on stop

    std::vector<std::unique_ptr<BoundedFrameQueue>> m_queues;  // one per pane
    std::vector<std::thread>                        m_producers;
    std::thread                                     m_consumer;

    std::atomic<bool> m_stop{false};
    std::atomic<int>  m_nextId{0};        // globally-unique frame ids
    std::atomic<long> m_produced{0};
    std::atomic<long> m_dropped{0};
    std::atomic<long> m_skipped{0};
    std::atomic<long> m_displayed{0};

    // Per-pane producer cadence + the consumer tick (watchable, not the
    // skeleton's millisecond timings). Faster panes overrun their queue and
    // exercise drop-oldest backpressure; the slow pane is occasionally empty.
    static constexpr int kPeriods[kPaneCount] = {650, 1000, 320, 1500};
    static constexpr int kTickMs = 1200;
};
