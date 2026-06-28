// Canvas.h
// The visualisation surface. It owns the threading pipeline and the animation
// loop, and paints the stages: N producer threads -> per-pane bounded queues ->
// one consumer. The GUI thread (this widget) only consumes finished frames; the
// producing/queueing is done by the pipeline's worker threads.
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

class QuadPipeline;

class Canvas : public QWidget {
    Q_OBJECT
public:
    explicit Canvas(QWidget* parent = nullptr);

    bool running() const { return m_running; }

signals:
    void statsChanged(int produced, int dropped, int skipped, int displayed,
                      const QString& status);

public slots:
    void start();
    void stop();

private slots:
    void onFramePushed(Frame frame);     // producer pushed into its queue
    void onFrameDropped(Frame frame);    // drop-oldest backpressure
    void onFrameDisplayed(Frame frame);  // consumer showed the freshest frame
    void onFrameSkipped(Frame frame);    // consumer bypassed a stale frame
    void onTick();                       // animation frame

private:
    // Terminal outcome of a frame, used to route its token off the queue.
    enum Outcome { OutDropped, OutSkipped, OutDisplayed };

    void   arrive(Token& t);             // handle end-of-segment transition
    void   reflowQueue(int pane);        // re-target queued tokens to slots
    bool   routeTerminal(int id, int pane, Outcome outcome);  // animate off queue
    Token* findToken(int id);
    void   emitStats(const QString& status);
    int    queuedCount(int pane) const;
    Token  spawnAtProducer(const Frame& frame) const;

    void paintEvent(QPaintEvent*) override;
    void drawPipeline(QPainter& p);
    void drawStation(QPainter& p, const QPointF& c, const QString& text,
                     const QColor& col, double radius = 50.0);
    void drawWorkingRing(QPainter& p, const QPointF& c, double radius);
    void drawLabel(QPainter& p, const QPointF& center, const QString& text);
    void drawPile(QPainter& p, const QPointF& center,
                  const std::deque<std::pair<int, QColor>>& pile, double alpha);
    void drawToken(QPainter& p, const Token& t);

    static double easeInOut(double x);

    QuadPipeline*      m_pipe = nullptr;
    QTimer             m_anim;
    QElapsedTimer      m_clock;
    std::vector<Token> m_tokens;

    // Visual mirror of each pane's queue (front = oldest); holds token ids.
    std::array<std::deque<int>, kPaneCount> m_queueIds;

    // A terminal signal (displayed/skipped/dropped) can arrive from the consumer
    // thread BEFORE the producer thread's framePushed reaches us (the two cross
    // separate queued connections, so order is only guaranteed per sender). Park
    // such outcomes here and apply them the moment the matching push arrives, so
    // a token is never stranded in the queue mirror.
    std::unordered_map<int, Outcome> m_pendingTerminal;

    std::deque<std::pair<int, QColor>> m_displayPile;
    std::deque<std::pair<int, QColor>> m_dropPile;
    std::deque<std::pair<int, QColor>> m_skipPile;

    // "actively working" highlight windows (clock ms until which to show ring).
    std::array<qint64, kPaneCount> m_paneBusyUntil{};
    qint64 m_consumerBusyUntil = 0;

    bool m_running = false;
    int  m_produced = 0;
    int  m_dropped = 0;
    int  m_skipped = 0;
    int  m_displayed = 0;
};
