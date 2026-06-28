// FramePipeline.h
// The threading backbone, organised as a real media pipeline:
//
//   READER thread  ->  single-slot hand-off  ->  WORKER thread  ->  GUI thread
//   (input source)     (mutex + condition var)   (processing)       (display)
//
//   * A dedicated READER thread pulls frames from an input source (here a
//     generator; in a real player it would read/decode from disk, a socket or
//     a capture device) and posts them into a single, mutex-guarded slot.
//   * If the slot is still full when the next frame is read, that frame is
//     DROPPED -- the worker cannot keep up, so freshness beats completeness.
//   * The WORKER thread sleeps on a condition variable until a frame is posted
//     or a stop is requested, processes it, then hands the finished frame to
//     the GUI thread (via a queued signal) for DISPLAY. The GUI only consumes.
//   * Shutdown is COOPERATIVE: flag stop, wake both threads, join them. Neither
//     thread is killed mid-frame.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <QObject>

#include "Shapes.h"

class FramePipeline : public QObject {
    Q_OBJECT
public:
    FramePipeline(int readMs, int workMs, QObject* parent = nullptr);
    ~FramePipeline() override;

    // Launch the reader and worker threads.
    void start();

    // COOPERATIVE shutdown: flag stop, wake both threads, join them.
    void shutdown();

    long dropped() const { return m_dropped; }

signals:
    // Emitted from the READER thread (queued to the GUI):
    void frameAccepted(Frame frame);   // read and posted into the slot
    void frameDropped(Frame frame);    // slot was full -> dropped
    // Emitted from the WORKER thread (queued to the GUI):
    void processingStarted(int id);    // worker took the slot frame
    void frameReady(Frame frame);      // worker finished -> hand to GUI to show
    void stopped();                    // both threads have joined

private:
    void  readLoop();    // PRODUCER side: input source -> slot
    void  workLoop();    // CONSUMER side: slot -> processing -> GUI
    Frame makeFrame();   // pull the next frame from the input source

    std::thread             m_reader;
    std::thread             m_worker;
    std::mutex              m_mutex;
    std::condition_variable m_cv;
    Frame                   m_pending;            // the single hand-off slot
    bool                    m_hasPending = false; // is the slot occupied?
    bool                    m_stop = false;       // cooperative stop flag
    std::atomic<long>       m_dropped{0};         // frames dropped under load
    int                     m_nextId = 0;         // reader-thread only
    int                     m_readMs;             // input read/decode latency
    int                     m_workMs;             // heavy per-frame processing
};
