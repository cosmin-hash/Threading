// Canvas.h
// The visualisation surface. It owns the threading pipeline and the animation
// loop, and paints the four stages: reader -> single slot -> worker -> GUI
// display. The GUI thread (this widget) only consumes finished frames; the
// producing is done by the pipeline's reader thread.
#pragma once

#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QColor>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include "Token.h"

class FramePipeline;

class Canvas : public QWidget {
    Q_OBJECT
public:
    explicit Canvas(QWidget* parent = nullptr);

    bool running() const { return m_running; }

signals:
    void statsChanged(int read, int dropped, int displayed,
                      const QString& status);

public slots:
    void start();
    void stop();

private slots:
    void onFrameAccepted(Frame frame);   // reader posted into the slot
    void onFrameDropped(Frame frame);    // slot full -> dropped
    void onProcessingStarted(int id);    // worker took the slot frame
    void onFrameReady(Frame frame);      // worker finished -> GUI displays it
    void onTick();                       // animation frame

private:
    // The furthest stage a worker-thread signal has announced for a frame whose
    // token the GUI has not created yet (the reader's frameAccepted has not been
    // processed). Used to recover from cross-thread signal reordering.
    enum PendingStage { StStarted, StReady };

    void   arrive(Token& t);             // handle end-of-segment transition
    bool   advanceToWorker(int id);      // apply "processing started" to a token
    bool   advanceToGui(int id);         // apply "frame ready" to a token
    void   emitStats(const QString& status);
    bool   slotOccupied() const;
    Token  spawnAtReader(const Frame& frame) const;

    void paintEvent(QPaintEvent*) override;
    void drawPipeline(QPainter& p);
    void drawStation(QPainter& p, const QPointF& c, const QString& text,
                     const QColor& col);
    void drawLabel(QPainter& p, const QPointF& center, const QString& text);
    void drawPile(QPainter& p, const QPointF& center,
                  const std::deque<std::pair<int, QColor>>& pile, double alpha);
    void drawToken(QPainter& p, const Token& t);

    static double easeInOut(double x);

    FramePipeline*     m_pipe = nullptr;
    QTimer             m_anim;
    QElapsedTimer      m_clock;
    std::vector<Token> m_tokens;
    std::deque<std::pair<int, QColor>> m_displayPile;
    std::deque<std::pair<int, QColor>> m_dropPile;
    // Parks worker-thread outcomes that reached the GUI before the reader's
    // frameAccepted for the same frame (id -> furthest stage announced).
    std::unordered_map<int, PendingStage> m_pending;

    bool m_running = false;
    bool m_workerBusy = false;
    int  m_read = 0;
    int  m_displayed = 0;
    int  m_lastDropped = 0;

    static constexpr int kReadMs = 650;    // input read/decode cadence
    static constexpr int kWorkMs = 1300;   // heavy per-frame processing
};
