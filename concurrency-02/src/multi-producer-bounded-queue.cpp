// quad_threading_core.cpp
// ---------------------------------------------------------------------------
// Distilled multithreading core of "QtOpenGLQuadPlayers".
//
// The full project is a Qt 6 / C++20 app that plays FOUR independent image
// streams in a 2x2 grid, each decoded on its own worker thread and rendered
// with OpenGL. Stripped of Qt and OpenGL, the concurrency architecture is:
//
//   N independent producers (one decode worker per pane)
//        |  each pushes into its OWN bounded, thread-safe queue
//        v
//   BoundedFrameQueue  (mutex-guarded, capacity N)
//        |  DROP-OLDEST backpressure: when full, the oldest frame is
//        |  discarded so a slow consumer can never grow memory unbounded
//        v
//   1 consumer (the render/main thread) takes the FRESHEST frame per pane
//        and discards any staler ones still queued (low display latency)
//
//   * Each worker runs on its own std::thread.
//   * Shutdown is DETERMINISTIC: signal every worker to stop, then join them
//     all, THEN destroy the shared queues - no producer can outlive its queue.
//
// Here "frames" are just text strings so the threading is visible on the
// console. Build:  g++ -std=c++17 -O2 -pthread quad_threading_core.cpp -o quadcore
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A decoded "frame": a text payload plus its sequence index, per pane.
struct Frame {
    int         pane  = -1;
    int         index = -1;
    std::string payload;
};

// ---------------------------------------------------------------------------
// BoundedFrameQueue: bounded, thread-safe single-producer/single-consumer
// buffer. Mirrors FrameQueue in the real project.
//   - push(): producer side. Drops the OLDEST frame(s) when full (backpressure).
//   - popLatest(): consumer side. Returns the freshest frame, discarding any
//                  older ones still buffered (keeps display latency low).
// ---------------------------------------------------------------------------
class BoundedFrameQueue {
public:
    explicit BoundedFrameQueue(int capacity = 3) : m_capacity(capacity) {}

    // Producer side. Returns how many frames were dropped to make room (0 or 1+).
    int push(Frame frame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        int dropped = 0;
        while (static_cast<int>(m_queue.size()) >= m_capacity) {
            m_queue.pop_front();   // drop the OLDEST -> backpressure
            ++dropped;
        }
        m_queue.push_back(std::move(frame));
        return dropped;
    }

    // Consumer side. Hands back the most recent frame and reports how many
    // older frames were skipped (bypassed) to get to it.
    bool popLatest(Frame& out, int& skipped) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;
        skipped = static_cast<int>(m_queue.size()) - 1;
        out = std::move(m_queue.back());
        m_queue.clear();
        return true;
    }

    int size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_queue.size());
    }

private:
    mutable std::mutex  m_mutex;
    std::deque<Frame>   m_queue;
    int                 m_capacity;
};

// ---------------------------------------------------------------------------
// DecodeWorker: one producer thread for one pane. Generates frames at its own
// pace and pushes them into its dedicated queue. Mirrors DecodeWorker.
// ---------------------------------------------------------------------------
class DecodeWorker {
public:
    DecodeWorker(int pane, BoundedFrameQueue* queue, int periodMs)
        : m_pane(pane), m_queue(queue), m_periodMs(periodMs) {}

    void start() { m_thread = std::thread([this] { run(); }); }

    void stop()  { m_stop.store(true); }                 // cooperative stop flag

    void join()  { if (m_thread.joinable()) m_thread.join(); }

    long produced() const { return m_produced.load(); }
    long dropped()  const { return m_dropped.load(); }

private:
    void run() {
        int index = 0;
        while (!m_stop.load()) {
            Frame f{m_pane, index,
                    "pane" + std::to_string(m_pane) +
                    " frame#" + std::to_string(index)};
            int dropped = m_queue->push(std::move(f));
            m_produced.fetch_add(1);
            m_dropped.fetch_add(dropped);
            ++index;
            // Each pane decodes at a different rate -> independent producers.
            std::this_thread::sleep_for(std::chrono::milliseconds(m_periodMs));
        }
    }

    int                 m_pane;
    BoundedFrameQueue*  m_queue;          // shared, not owned
    int                 m_periodMs;
    std::thread         m_thread;
    std::atomic<bool>   m_stop{false};
    std::atomic<long>   m_produced{0};
    std::atomic<long>   m_dropped{0};
};

int main() {
    std::cout << "=== QtOpenGLQuadPlayers: N-worker bounded-queue core ===\n\n";

    constexpr int kPanes = 4;

    // One bounded queue per pane (capacity 3), and one worker per pane.
    // Held by unique_ptr because each owns a std::mutex / std::thread and so
    // is intentionally non-copyable and non-movable.
    std::vector<std::unique_ptr<BoundedFrameQueue>> queues;
    for (int i = 0; i < kPanes; ++i)
        queues.push_back(std::make_unique<BoundedFrameQueue>(/*capacity=*/3));

    std::vector<std::unique_ptr<DecodeWorker>> workers;
    // Deliberately different periods so the panes run at different fps and the
    // faster ones overrun their queue (exercising drop-oldest backpressure).
    const int periods[kPanes] = {10, 18, 7, 25};
    for (int i = 0; i < kPanes; ++i)
        workers.push_back(std::make_unique<DecodeWorker>(i, queues[i].get(), periods[i]));

    for (auto& w : workers) w->start();

    // CONSUMER (this thread): poll each pane's queue and take the freshest
    // frame, like the render loop pulling the latest decoded frame per pane.
    const int kTicks = 20;                  // ~20 render "frames"
    for (int tick = 0; tick < kTicks; ++tick) {
        std::cout << "-- render tick " << tick << " --\n";
        for (int p = 0; p < kPanes; ++p) {
            Frame f;
            int skipped = 0;
            if (queues[p]->popLatest(f, skipped)) {
                std::cout << "  pane " << p << ": show \"" << f.payload
                          << "\" (skipped " << skipped << " stale)\n";
            } else {
                std::cout << "  pane " << p << ": (no frame ready)\n";
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 fps
    }

    // DETERMINISTIC SHUTDOWN: signal all workers, then join them all, then the
    // queues are destroyed at scope end - no producer outlives its queue.
    std::cout << "\n[main] requesting all workers to stop...\n";
    for (auto& w : workers) w->stop();
    for (auto& w : workers) w->join();
    std::cout << "[main] all workers joined\n\n";

    for (int p = 0; p < kPanes; ++p) {
        std::cout << "pane " << p << ": produced " << workers[p]->produced()
                  << ", dropped " << workers[p]->dropped()
                  << " (backpressure)\n";
    }
    std::cout << "\n[main] clean shutdown complete\n";
    return 0;
}
