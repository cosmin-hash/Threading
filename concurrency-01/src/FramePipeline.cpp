// FramePipeline.cpp
#include "FramePipeline.h"

#include <chrono>

#include <QRandomGenerator>

FramePipeline::FramePipeline(int readMs, int workMs, QObject* parent)
    : QObject(parent), m_readMs(readMs), m_workMs(workMs) {
    qRegisterMetaType<Frame>("Frame");   // needed for queued cross-thread signals
}

FramePipeline::~FramePipeline() { shutdown(); }

void FramePipeline::start() {
    if (m_reader.joinable() || m_worker.joinable()) return;
    m_stop = false;
    m_hasPending = false;
    m_dropped = 0;
    m_nextId = 0;
    m_worker = std::thread([this] { workLoop(); });
    m_reader = std::thread([this] { readLoop(); });
}

void FramePipeline::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        m_cv.notify_all();   // wake the worker so it can exit
    }
    if (m_reader.joinable()) m_reader.join();
    if (m_worker.joinable()) m_worker.join();
    emit stopped();
}

// Pull the next frame from the input source. Here it is synthesised, but this
// is exactly where a real player would read & decode the next frame from disk,
// a network stream or a capture device.
Frame FramePipeline::makeFrame() {
    Frame f;
    f.id = ++m_nextId;
    f.shape = QRandomGenerator::global()->bounded(int(ShapeCount));
    const int h = QRandomGenerator::global()->bounded(360);
    f.color = QColor::fromHsv(h, 200 + QRandomGenerator::global()->bounded(56),
                              230 + QRandomGenerator::global()->bounded(26));
    return f;
}

// PRODUCER thread: read frames from the input source and hand each into the
// single slot. A frame read while the slot is still full is dropped.
void FramePipeline::readLoop() {
    for (;;) {
        // Simulated input read/decode latency.
        std::this_thread::sleep_for(std::chrono::milliseconds(m_readMs));

        Frame f = makeFrame();
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop) return;
            if (m_hasPending) {            // slot occupied -> drop this frame
                ++m_dropped;
            } else {
                m_pending = f;
                m_hasPending = true;
                m_cv.notify_one();         // wake the sleeping worker
                accepted = true;
            }
        }
        if (accepted) emit frameAccepted(f);
        else          emit frameDropped(f);
    }
}

// CONSUMER thread: sleep on the CV until a frame is posted or stop is
// requested, process it, then hand the finished frame to the GUI for display.
void FramePipeline::workLoop() {
    for (;;) {
        Frame frame;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_hasPending || m_stop; });
            if (m_stop && !m_hasPending)
                return;
            frame = m_pending;
            m_hasPending = false;
        } // mutex released before the slow work

        emit processingStarted(frame.id);
        std::this_thread::sleep_for(std::chrono::milliseconds(m_workMs));
        emit frameReady(frame);            // hand off to the GUI thread
    }
}
