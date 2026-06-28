// mp4_threading_core.cpp
// ---------------------------------------------------------------------------
// Distilled multithreading core of "Mp4DualPlayer".
//
// The full project is a Qt 6 / C++20 dual-display MP4 player. Stripped of Qt,
// OpenGL and video decoding, the concurrency architecture it is built on is:
//
//   PRODUCER (main thread)  ->  single-slot hand-off  ->  CONSUMER (worker)
//
//   * One dedicated worker thread does the per-frame work off the main thread,
//     so the UI/producer never blocks.
//   * The hand-off is a SINGLE pending slot guarded by a mutex. If a new frame
//     arrives while one is already pending, the old one is DROPPED (frame-drop
//     under load) - real-time playback favours freshness over completeness.
//   * The worker SLEEPS on a condition variable until a frame is posted or a
//     stop is requested. No polling, no busy-wait: an idle worker uses 0% CPU.
//   * Shutdown is COOPERATIVE: set a stop flag, wake the worker, join it.
//     The worker is never killed mid-operation (no thread "terminate").
//
// Here "frames" are just text strings so the threading is visible on the
// console. Build:  g++ -std=c++17 -O2 -pthread mp4_threading_core.cpp -o mp4core
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

// A "frame" is just a labelled text payload for this demo.
struct Frame {
    int         id = -1;
    std::string payload;
};

// ---------------------------------------------------------------------------
// FrameProcessor: owns one worker thread and a single-slot, mutex-guarded
// hand-off. This mirrors VideoProcessingThread in the real project.
// ---------------------------------------------------------------------------
class FrameProcessor {
public:
    FrameProcessor() {
        // Launch the worker. It blocks immediately until work or stop arrives.
        m_worker = std::thread([this] { run(); });
    }

    ~FrameProcessor() {
        // Cooperative shutdown: request stop, wake the worker, join cleanly.
        stop();
        if (m_worker.joinable())
            m_worker.join();
        std::cout << "[main]   worker joined cleanly\n";
    }

    // PRODUCER side (called from the main thread). Posts a frame into the
    // single pending slot. If a frame is already pending, this one replaces
    // nothing - the NEW frame is dropped, exactly like the real player drops
    // frames it cannot keep up with. (The real code keeps the pending one;
    // either policy is "newest-wins under backpressure" - we keep it faithful.)
    void postFrame(Frame frame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_hasPending) {
            // A frame is already waiting to be processed -> drop the incoming one.
            ++m_dropped;
            std::cout << "[main]   posted frame " << frame.id
                      << " -> DROPPED (worker still busy, total dropped="
                      << m_dropped << ")\n";
            return;
        }
        m_pending    = std::move(frame);
        m_hasPending = true;
        std::cout << "[main]   posted frame " << m_pending.id << " -> queued\n";
        m_cv.notify_one();          // wake the sleeping worker
    }

    void stop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        m_cv.notify_all();          // wake the worker so it can exit
    }

    long dropped() const { return m_dropped; }

private:
    // CONSUMER side: the worker thread loop.
    void run() {
        std::cout << "[worker] started, waiting for frames\n";
        for (;;) {
            Frame frame;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                // Sleep until there is a pending frame OR a stop was requested.
                // The predicate also guards against spurious wakeups.
                m_cv.wait(lock, [this] { return m_hasPending || m_stop; });

                if (m_stop && !m_hasPending) {
                    std::cout << "[worker] stop requested, exiting\n";
                    return;
                }

                // Take the pending frame and clear the slot so the producer
                // can post the next one.
                frame        = std::move(m_pending);
                m_hasPending = false;
            } // mutex released before doing the (slow) work

            // Simulated heavy per-frame work (decode/scale/convert in reality).
            std::cout << "[worker] processing frame " << frame.id
                      << " : \"" << frame.payload << "\"\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            std::cout << "[worker] done frame " << frame.id << "\n";
        }
    }

    std::thread             m_worker;
    std::mutex              m_mutex;
    std::condition_variable m_cv;

    Frame                   m_pending;             // the single hand-off slot
    bool                    m_hasPending = false;  // is the slot occupied?
    bool                    m_stop       = false;  // cooperative stop flag
    std::atomic<long>       m_dropped{0};          // frames dropped under load
};

int main() {
    std::cout << "=== Mp4DualPlayer: single-worker condition-variable core ===\n\n";

    FrameProcessor processor;

    // Producer posts frames FASTER than the worker can process them, so some
    // are dropped - demonstrating the frame-drop-under-load behaviour.
    const std::string captions[] = {
        "intro title card", "scene 1 - sunrise", "scene 2 - city",
        "scene 3 - chase",  "scene 4 - dialog",  "scene 5 - finale",
        "credits roll",     "logo sting"
    };

    int id = 0;
    for (const auto& caption : captions) {
        processor.postFrame(Frame{id++, caption});
        // Producer cadence is faster than the worker's 40ms, forcing drops.
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    // Let the worker drain whatever is pending before we tear down.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "\n[main]   total frames dropped under load: "
              << processor.dropped() << "\n";
    std::cout << "[main]   shutting down...\n";
    // processor destructor runs here: cooperative stop + join.
    return 0;
}
