// Canvas.h
// The visualisation surface. It owns the threading backbone and the animation
// loop, and paints every stage of the pipeline:
//
//   SOURCE -> [channel 1] -> TRANSFORM -> {4 pool workers} -> [channel 2] -> SINK
//
// The GUI thread (this widget) only consumes the signals the real threads emit;
// it never produces and is never blocked.
#pragma once

#include <array>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include "Token.h"

class PipelineEngine;

class Canvas : public QWidget {
    Q_OBJECT
public:
    explicit Canvas(QWidget* parent = nullptr);

    bool running() const { return m_running; }

signals:
    void statsChanged(int produced, int stalls, int consumed,
                      const QString& status);

public slots:
    void start();
    void stop();

private slots:
    void onItemProduced(WorkItem item);        // source pushed it into channel 1
    void onBackpressure(WorkItem item);        // source blocked on a full channel 1
    void onTransformTook(int id);              // transform popped it from channel 1
    void onWorkerStarted(int id, int worker);  // a pool worker began the task
    void onResultBuffered(int id, int worker); // result pushed into channel 2
    void onItemConsumed(WorkItem item);        // sink consumed it from channel 2
    void onTick();                             // animation frame

private:
    // The furthest stage a downstream signal has announced for an item whose
    // token the GUI has not spawned yet (the source's itemProduced has not been
    // processed). Used to recover from cross-thread signal reordering.
    enum Stage { StTook, StStarted, StBuffered, StConsumed };
    struct PendingStage { Stage stage; int worker = -1; };

    void  arrive(Token& t);                    // handle end-of-segment transition
    bool  advanceToTransform(int id);
    bool  advanceToWorker(int id, int worker);
    bool  advanceToChan2(int id, int worker);
    bool  advanceToSink(int id);
    void  fastForward(Token& t, const PendingStage& ps) const;
    void  layoutChannels();                    // restack buffered tokens each tick
    void  emitStats(const QString& status);
    Token spawnAtSource(const WorkItem& item) const;
    int   channelOccupancy(Token::State inState) const;

    void paintEvent(QPaintEvent*) override;
    void drawPipeline(QPainter& p);
    void drawStation(QPainter& p, const QPointF& c, const QString& text,
                     const QColor& col);
    void drawChannel(QPainter& p, const QPointF& c, const QString& topLabel,
                     int occupied);
    void drawLabel(QPainter& p, const QPointF& center, const QString& text);
    void drawPile(QPainter& p, const QPointF& center,
                  const std::deque<std::pair<int, QColor>>& pile, double alpha);
    void drawToken(QPainter& p, const Token& t);

    static double easeInOut(double x);

    PipelineEngine*    m_engine = nullptr;
    QTimer             m_anim;
    QElapsedTimer      m_clock;
    std::vector<Token> m_tokens;
    std::deque<std::pair<int, QColor>> m_consumedPile;
    std::deque<std::pair<int, QColor>> m_throttledPile;
    // Parks downstream outcomes that reached the GUI before the source's
    // itemProduced for the same item (id -> furthest stage announced).
    std::unordered_map<int, PendingStage> m_pending;

    std::array<bool, kPoolSize> m_workerBusy{};
    bool m_running = false;
    bool m_sourceBlocked = false;   // source currently throttled by chan1
    int  m_produced = 0;
    int  m_consumed = 0;
    int  m_stalls = 0;

    static constexpr int kSourceMs = 240;   // source produce cadence
    static constexpr int kWorkMs   = 900;   // heavy per-item processing on the pool
};
