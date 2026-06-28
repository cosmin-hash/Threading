// Canvas.cpp
#include "Canvas.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QFont>
#include <QPainter>
#include <QPen>

#include "WorkStealingPool.h"

Canvas::Canvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(1180, 640);
    setAutoFillBackground(true);

    m_anim.setInterval(16);                 // ~60 fps
    connect(&m_anim, &QTimer::timeout, this, &Canvas::onTick);

    m_clock.start();
}

// ---- lifecycle -------------------------------------------------------------
void Canvas::start() {
    if (m_running) return;
    m_running = true;
    m_submitted = m_ownDone = m_stolenDone = 0;
    m_tokens.clear();
    for (auto& d : m_dequeIds) d.clear();
    m_ownPile.clear();
    m_stolenPile.clear();
    m_pending.clear();
    m_busy.fill(false);

    m_pool = new WorkStealingPool(kWorkers, kSubmitMs, kShortMs, kLongMs, this);
    // Queued connections: signals cross from the submitter/worker threads to here.
    connect(m_pool, &WorkStealingPool::taskSubmitted,
            this, &Canvas::onTaskSubmitted, Qt::QueuedConnection);
    connect(m_pool, &WorkStealingPool::taskStarted,
            this, &Canvas::onTaskStarted, Qt::QueuedConnection);
    connect(m_pool, &WorkStealingPool::taskFinished,
            this, &Canvas::onTaskFinished, Qt::QueuedConnection);
    m_pool->start();

    m_anim.start();
    emitStats("running");
}

void Canvas::stop() {
    if (!m_running) return;
    m_running = false;
    emitStats("stopping - draining backlog, joining threads...");
    // Cooperative shutdown + join of the submitter and every worker. No task in
    // flight is killed; the workers drain whatever is still pending first.
    if (m_pool) {
        m_pool->shutdown();
        QCoreApplication::processEvents();   // drain late queued signals
        m_pool->deleteLater();
        m_pool = nullptr;
    }
    m_busy.fill(false);
    emitStats("stopped - threads joined cleanly");
    update();
}

// ---- model events (all arrive on the GUI thread via queued connections) ----
Token Canvas::spawnAtSubmit(const Task& task) const {
    Token tok;
    tok.id = task.id;
    tok.shape = task.shape;
    tok.color = task.color;
    tok.expensive = task.expensive;
    tok.home = task.home;
    tok.pos = tok.from = kSubmitPt;
    return tok;
}

void Canvas::onTaskSubmitted(Task task) {
    ++m_submitted;
    Token tok = spawnAtSubmit(task);

    // A worker-thread signal for this task may already have arrived ahead of us
    // (cross-thread signals have no mutual ordering guarantee). If so, send the
    // freshly spawned token straight to the furthest stage already announced
    // instead of into the deque.
    auto it = m_pending.find(task.id);
    if (it != m_pending.end()) {
        const Parked pk = it->second;
        m_pending.erase(it);
        tok.ranBy = pk.byWorker;
        tok.stolen = pk.stolen;
        tok.from = kSubmitPt;
        if (pk.stage == Parked::Finished) {     // already finished -> head to pile
            tok.state = pk.stolen ? Token::ToStolenPile : Token::ToOwnPile;
            tok.to = pk.stolen ? kStolenPilePt : kOwnPilePt;
            tok.speed = 1.4;
        } else {                                // already started -> head to worker
            tok.state = Token::ToWorker;
            tok.to = kWorkerPt(pk.byWorker);
            tok.speed = 1.4;
        }
        m_tokens.push_back(tok);
        emitStats("running");
        return;
    }

    // Normal path: it joins the back of its home worker's deque and homes in.
    tok.state = Token::Queued;
    m_dequeIds[task.home].push_back(task.id);
    m_tokens.push_back(tok);
    emitStats("running");
}

void Canvas::onTaskStarted(Task task, int byWorker, bool stolen) {
    m_busy[byWorker] = true;
    // If the token does not exist yet, the submitter's taskSubmitted has not been
    // processed; remember that this task has at least started (and on whom).
    if (!advanceToWorker(task.id, byWorker, stolen)) {
        Parked pk;
        pk.stage = Parked::Started;
        pk.byWorker = byWorker;
        pk.stolen = stolen;
        m_pending[task.id] = pk;
    }
}

void Canvas::onTaskFinished(Task task, int byWorker, bool stolen) {
    m_busy[byWorker] = false;
    // Same guard: park the terminal outcome if the token has not spawned yet.
    if (!advanceToPile(task.id, byWorker, stolen)) {
        Parked pk;
        pk.stage = Parked::Finished;
        pk.byWorker = byWorker;
        pk.stolen = stolen;
        m_pending[task.id] = pk;
    }
}

// Pull a queued token out of its deque and fly it to the worker that took it.
bool Canvas::advanceToWorker(int id, int byWorker, bool stolen) {
    for (auto& t : m_tokens) {
        if (t.id == id && t.state == Token::Queued) {
            removeFromDeque(id, t.home);
            t.from = t.pos;
            t.to = kWorkerPt(byWorker);
            t.t = 0.0;
            t.state = Token::ToWorker;
            t.speed = 1.4;
            t.ranBy = byWorker;
            t.stolen = stolen;
            return true;
        }
    }
    return false;
}

// Fly an in-flight token to its outcome pile. Returns false if none matches.
bool Canvas::advanceToPile(int id, int byWorker, bool stolen) {
    for (auto& t : m_tokens) {
        if (t.id == id &&
            (t.state == Token::Queued || t.state == Token::ToWorker ||
             t.state == Token::Working)) {
            if (t.state == Token::Queued)
                removeFromDeque(id, t.home);
            t.from = t.pos;
            t.to = stolen ? kStolenPilePt : kOwnPilePt;
            t.t = 0.0;
            t.state = stolen ? Token::ToStolenPile : Token::ToOwnPile;
            t.speed = 1.4;
            t.ranBy = byWorker;
            t.stolen = stolen;
            return true;
        }
    }
    return false;
}

void Canvas::removeFromDeque(int id, int home) {
    auto& d = m_dequeIds[home];
    d.erase(std::remove(d.begin(), d.end(), id), d.end());
}

// ---- animation -------------------------------------------------------------
void Canvas::onTick() {
    const double dt = 1.0 / 60.0;
    for (auto& t : m_tokens) {
        t.pulse += dt;
        switch (t.state) {
        case Token::Queued: {
            // Home (exponential smoothing) toward the current slot; the target
            // slides as tasks ahead are popped or stolen, so the queue compacts.
            auto& d = m_dequeIds[t.home];
            const int n = int(d.size());
            const auto it = std::find(d.begin(), d.end(), t.id);
            if (it != d.end()) {
                const int idx = int(it - d.begin());
                const QPointF target = kDequeSlot(t.home, idx, n);
                t.pos += (target - t.pos) * 0.18;
            }
            break;
        }
        case Token::ToWorker:
        case Token::ToOwnPile:
        case Token::ToStolenPile: {
            t.t = qMin(1.0, t.t + dt * t.speed);
            const double e = easeInOut(t.t);
            t.pos = t.from + (t.to - t.from) * e;
            if (t.t >= 1.0)
                arrive(t);
            break;
        }
        case Token::Working:
        case Token::OwnDone:
        case Token::StolenDone:
            break;
        }
    }
    // Reap tokens that have landed in a pile (the pile keeps its own copy).
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->state == Token::OwnDone || it->state == Token::StolenDone)
            it = m_tokens.erase(it);
        else
            ++it;
    }
    update();
}

void Canvas::arrive(Token& t) {
    switch (t.state) {
    case Token::ToWorker:
        t.state = Token::Working;
        t.pos = kWorkerPt(t.ranBy);
        break;
    case Token::ToOwnPile:
        t.state = Token::OwnDone;
        ++m_ownDone;
        m_ownPile.push_back({t.shape, t.color});
        if (m_ownPile.size() > 24) m_ownPile.pop_front();
        emitStats("running");
        break;
    case Token::ToStolenPile:
        t.state = Token::StolenDone;
        ++m_stolenDone;
        m_stolenPile.push_back({t.shape, t.color});
        if (m_stolenPile.size() > 24) m_stolenPile.pop_front();
        emitStats("running");
        break;
    default:
        break;
    }
}

// ---- helpers ---------------------------------------------------------------
double Canvas::easeInOut(double x) {
    return x < 0.5 ? 2 * x * x : 1 - std::pow(-2 * x + 2, 2) / 2;
}

void Canvas::emitStats(const QString& status) {
    emit statsChanged(m_submitted, m_ownDone, m_stolenDone, status);
}

// ---- painting --------------------------------------------------------------
void Canvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(24, 26, 34));

    drawScene(p);
    drawPile(p, kOwnPilePt, m_ownPile, 1.0);
    drawPile(p, kStolenPilePt, m_stolenPile, 0.65);

    // Highlight an in-flight steal (a token crossing rows to a thief worker).
    for (const auto& t : m_tokens) {
        if (t.state == Token::ToWorker && t.stolen) {
            QPen steal(QColor(240, 170, 70, 150), 2, Qt::DashLine);
            p.setPen(steal);
            p.drawLine(t.from, t.pos);
        }
    }
    // Moving tokens last so they sit above the stations.
    for (const auto& t : m_tokens)
        drawToken(p, t);
}

void Canvas::drawScene(QPainter& p) {
    // ---- title + subtitle --------------------------------------------------
    p.setPen(QColor(210, 215, 230));
    QFont title = p.font();
    title.setPointSize(13);
    title.setBold(true);
    p.setFont(title);
    p.drawText(QRectF(20, 12, width() - 40, 30), Qt::AlignLeft,
               "Work-stealing scheduler  ->  per-worker deques  +  steal-from-a-random-victim");
    QFont sub = p.font();
    sub.setPointSize(9);
    sub.setBold(false);
    p.setFont(sub);
    p.setPen(QColor(150, 156, 174));
    p.drawText(QRectF(20, 38, width() - 40, 22), Qt::AlignLeft,
               "Tasks are submitted round-robin; every task for worker 0 is heavy, so its deque backs up while "
               "the idle workers steal from its FRONT. Owners pop their own deque's BACK (LIFO); thieves steal "
               "the FRONT (FIFO).");

    // ---- flow guides -------------------------------------------------------
    p.setPen(QPen(QColor(54, 60, 80), 2, Qt::DashLine));
    for (int i = 0; i < kWorkers; ++i)
        p.drawLine(kSubmitPt, QPointF(kLaneLeftX + 8, kWorkerY[i]));   // submit -> deque
    // Faint worker -> pile guides.
    p.setPen(QPen(QColor(44, 50, 68), 1, Qt::DashLine));
    for (int i = 0; i < kWorkers; ++i) {
        p.drawLine(kWorkerPt(i), kOwnPilePt);
        p.drawLine(kWorkerPt(i), kStolenPilePt);
    }

    // ---- submit station ----------------------------------------------------
    drawStation(p, kSubmitPt, 48, QColor(70, 130, 200));
    p.setPen(QColor(170, 176, 196));
    drawLabel(p, QPointF(kSubmitPt.x(), kSubmitPt.y() + 60), "SUBMIT\n(round-robin)");

    // ---- per-worker deque lanes + stations ---------------------------------
    for (int i = 0; i < kWorkers; ++i) {
        const double y = kWorkerY[i];

        // Deque lane (a subtle holding strip from the steal end to the worker).
        QRectF lane(kLaneLeftX, y - 26, kDequeBackX - kLaneLeftX + 34, 52);
        p.setPen(QPen(QColor(64, 72, 96), 2));
        p.setBrush(QColor(30, 34, 46));
        p.drawRoundedRect(lane, 10, 10);

        // End hints: FRONT is the steal end (left), BACK the owner end (right).
        QFont tiny = p.font();
        tiny.setPointSize(7);
        p.setFont(tiny);
        p.setPen(QColor(140, 120, 90));
        p.drawText(QRectF(lane.left(), y - 25, 90, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   "front (steal)");
        p.setPen(QColor(120, 140, 120));
        p.drawText(QRectF(lane.right() - 92, y - 25, 88, 14), Qt::AlignRight | Qt::AlignVCenter,
                   "back (own)");
        p.setFont(sub);

        // Worker station; turns red while busy, with a rotating processing arc.
        const bool busy = m_busy[i];
        drawStation(p, kWorkerPt(i), 46, busy ? QColor(220, 90, 90)
                                              : QColor(90, 170, 110));
        if (busy)
            drawWorkingArc(p, kWorkerPt(i), 46);

        p.setPen(QColor(170, 176, 196));
        drawLabel(p, QPointF(kWorkerX, y + 58),
                  QString("WORKER %1").arg(i) + (i == 0 ? "  (heavy load)" : ""));
    }

    // ---- pile labels -------------------------------------------------------
    p.setPen(QColor(120, 190, 140));
    drawLabel(p, QPointF(kOwnPilePt.x(), kOwnPilePt.y() - 70), "COMPLETED (own)", 10);
    p.setPen(QColor(230, 180, 90));
    drawLabel(p, QPointF(kStolenPilePt.x(), kStolenPilePt.y() - 70), "STOLEN & DONE", 10);
}

void Canvas::drawStation(QPainter& p, const QPointF& c, double r,
                         const QColor& col) {
    p.setPen(QPen(col, 3));
    p.setBrush(QColor(34, 38, 50));
    p.drawEllipse(c, r, r);
}

void Canvas::drawWorkingArc(QPainter& p, const QPointF& c, double r) {
    p.save();
    p.translate(c);
    const double ang = std::fmod(m_clock.elapsed() * 0.36, 360.0);
    p.rotate(ang);
    QPen ring(QColor(240, 200, 90), 4);
    ring.setCapStyle(Qt::RoundCap);
    p.setPen(ring);
    const double rr = r + 8;
    p.drawArc(QRectF(-rr, -rr, 2 * rr, 2 * rr), 0, 270 * 16);
    p.restore();
}

void Canvas::drawLabel(QPainter& p, const QPointF& center, const QString& text,
                       int pt) {
    QFont f = p.font();
    f.setPointSize(pt);
    f.setBold(false);
    p.setFont(f);
    // Keep whatever pen colour the caller set for the label tint.
    p.drawText(QRectF(center.x() - 120, center.y() - 6, 240, 44),
               Qt::AlignHCenter | Qt::AlignTop, text);
}

void Canvas::drawPile(QPainter& p, const QPointF& center,
                      const std::deque<std::pair<int, QColor>>& pile,
                      double alpha) {
    const int cols = 6;
    const double s = 18.0, gap = 6.0;
    const double x0 = center.x() - (cols * (s + gap) - gap) / 2 + s / 2;
    const double y0 = center.y() - 8;
    for (int i = 0; i < int(pile.size()); ++i) {
        const int r = i / cols, c = i % cols;
        const QPointF pos(x0 + c * (s + gap), y0 + r * (s + gap));
        paintShape(p, pile[i].first, pos, s / 2, pile[i].second, alpha);
    }
}

void Canvas::drawToken(QPainter& p, const Token& t) {
    double r = t.expensive ? kRadius * 1.18 : kRadius;
    if (t.state == Token::Working)
        r += 4 * std::sin(t.pulse * 6.0);            // gentle pulse
    paintShape(p, t.shape, t.pos, r, t.color, 1.0);

    // Heavy tasks wear a thin amber ring so worker 0's overload reads at a glance.
    if (t.expensive) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(240, 180, 80), 2));
        p.drawEllipse(t.pos, r + 3, r + 3);
    }

    // id badge
    p.setPen(QColor(20, 20, 28));
    QFont f = p.font();
    f.setPointSize(8);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRectF(t.pos.x() - r, t.pos.y() - r, 2 * r, 2 * r),
               Qt::AlignCenter, QString::number(t.id));
}
