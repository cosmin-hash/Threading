// Canvas.h
// The visualisation surface. It owns the work-stealing pool and the animation
// loop, and paints the scheduler: a round-robin submitter, one deque lane and
// station per worker, and two outcome piles (completed-own / stolen). The GUI
// thread (this widget) only consumes events the pool emits; it never schedules.
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

class WorkStealingPool;

class Canvas : public QWidget {
    Q_OBJECT
public:
    explicit Canvas(QWidget* parent = nullptr);

    bool running() const { return m_running; }

signals:
    void statsChanged(int submitted, int ranOwn, int stolen,
                      const QString& status);

public slots:
    void start();
    void stop();

private slots:
    void onTaskSubmitted(Task task);                       // submitter pushed it
    void onTaskStarted(Task task, int byWorker, bool stolen);  // worker began it
    void onTaskFinished(Task task, int byWorker, bool stolen); // worker finished it
    void onTick();                                         // animation frame

private:
    // The furthest stage a worker-thread signal has announced for a task whose
    // token the GUI has not created yet (the submitter's taskSubmitted has not
    // been processed). Used to recover from cross-thread signal reordering.
    struct Parked {
        enum Stage { Started, Finished } stage = Started;
        int  byWorker = 0;
        bool stolen = false;
    };

    void  arrive(Token& t);                       // end-of-segment transition
    bool  advanceToWorker(int id, int byWorker, bool stolen);
    bool  advanceToPile(int id, int byWorker, bool stolen);
    void  removeFromDeque(int id, int home);
    Token spawnAtSubmit(const Task& task) const;
    void  emitStats(const QString& status);

    void paintEvent(QPaintEvent*) override;
    void drawScene(QPainter& p);
    void drawStation(QPainter& p, const QPointF& c, double r, const QColor& col);
    void drawWorkingArc(QPainter& p, const QPointF& c, double r);
    void drawLabel(QPainter& p, const QPointF& center, const QString& text,
                   int pt = 9);
    void drawPile(QPainter& p, const QPointF& center,
                  const std::deque<std::pair<int, QColor>>& pile, double alpha);
    void drawToken(QPainter& p, const Token& t);

    static double easeInOut(double x);

    WorkStealingPool*  m_pool = nullptr;
    QTimer             m_anim;
    QElapsedTimer      m_clock;
    std::vector<Token> m_tokens;

    // GUI mirror of each worker's deque: ids in order FRONT(0)..BACK(n-1). Used
    // only to position queued tokens; the real deques live in the pool.
    std::array<std::deque<int>, kWorkers> m_dequeIds;

    std::deque<std::pair<int, QColor>> m_ownPile;     // completed by their owner
    std::deque<std::pair<int, QColor>> m_stolenPile;  // completed by a thief

    // Parks worker-thread outcomes that reached the GUI before the submitter's
    // taskSubmitted for the same task (id -> furthest stage announced + who/how).
    std::unordered_map<int, Parked> m_pending;

    std::array<bool, kWorkers> m_busy{};   // is worker i running a task right now?

    bool m_running = false;
    int  m_submitted = 0;
    int  m_ownDone = 0;
    int  m_stolenDone = 0;

    static constexpr int kSubmitMs = 320;   // cadence between round-robin submits
    static constexpr int kShortMs  = 550;   // cheap task duration
    static constexpr int kLongMs   = 1700;  // heavy task duration (worker 0's load)
};
