// Canvas.cpp
#include "Canvas.h"

#include <cmath>

#include <QCoreApplication>
#include <QFont>
#include <QPainter>
#include <QPen>

#include "QuadPipeline.h"

Canvas::Canvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(1200, 760);
    setAutoFillBackground(true);

    m_anim.setInterval(16);                 // ~60 fps
    connect(&m_anim, &QTimer::timeout, this, &Canvas::onTick);

    m_clock.start();
}

// ---- lifecycle -------------------------------------------------------------
void Canvas::start() {
    if (m_running) return;
    m_running = true;
    m_produced = m_dropped = m_skipped = m_displayed = 0;
    m_tokens.clear();
    for (auto& q : m_queueIds) q.clear();
    m_pendingTerminal.clear();
    m_displayPile.clear();
    m_dropPile.clear();
    m_skipPile.clear();
    m_paneBusyUntil.fill(0);
    m_consumerBusyUntil = 0;

    m_pipe = new QuadPipeline(this);
    // Queued connections: signals cross from the worker threads to the GUI.
    connect(m_pipe, &QuadPipeline::framePushed,
            this, &Canvas::onFramePushed, Qt::QueuedConnection);
    connect(m_pipe, &QuadPipeline::frameDropped,
            this, &Canvas::onFrameDropped, Qt::QueuedConnection);
    connect(m_pipe, &QuadPipeline::frameDisplayed,
            this, &Canvas::onFrameDisplayed, Qt::QueuedConnection);
    connect(m_pipe, &QuadPipeline::frameSkipped,
            this, &Canvas::onFrameSkipped, Qt::QueuedConnection);
    m_pipe->start();

    m_anim.start();
    emitStats("running");
}

void Canvas::stop() {
    if (!m_running) return;
    m_running = false;
    emitStats("stopping - joining threads...");
    // Cooperative shutdown + join of every producer and the consumer. No thread
    // is killed mid-frame; this blocks at most one short producer/consumer step.
    if (m_pipe) {
        m_pipe->shutdown();
        QCoreApplication::processEvents();   // drain late queued signals
        m_pipe->deleteLater();
        m_pipe = nullptr;
    }
    emitStats("stopped - threads joined cleanly");
    update();
}

// ---- helpers ---------------------------------------------------------------
Token* Canvas::findToken(int id) {
    for (auto& t : m_tokens)
        if (t.id == id) return &t;
    return nullptr;
}

int Canvas::queuedCount(int pane) const {
    return int(m_queueIds[pane].size());
}

// Re-target every token still in pane's queue to its current slot position, so
// the column slides forward when the oldest is dropped or consumed.
void Canvas::reflowQueue(int pane) {
    const auto& ids = m_queueIds[pane];
    for (int i = 0; i < int(ids.size()); ++i) {
        Token* tok = findToken(ids[i]);
        if (!tok) continue;
        const QPointF dest = queueSlotPt(pane, i);
        tok->slot = i;
        if (tok->state == Token::InQueue || tok->state == Token::ToQueue) {
            if (tok->to != dest) {
                tok->from = tok->pos;
                tok->to = dest;
                tok->t = 0.0;
                tok->state = Token::ToQueue;
                tok->speed = 2.4;            // a quick slide, not a long glide
            }
        }
    }
}

// ---- model events (all arrive on the GUI thread via queued connections) ----
Token Canvas::spawnAtProducer(const Frame& frame) const {
    Token tok;
    tok.id = frame.id;
    tok.pane = frame.pane;
    tok.shape = frame.shape;
    tok.color = frame.color;
    tok.pos = tok.from = producerPt(frame.pane);
    return tok;
}

// Animate an already-spawned token off its queue toward its outcome. Returns
// false if the token isn't known yet (its framePushed hasn't arrived).
bool Canvas::routeTerminal(int id, int pane, Outcome outcome) {
    Token* tok = findToken(id);
    if (!tok) return false;

    auto& ids = m_queueIds[pane];
    for (auto it = ids.begin(); it != ids.end(); ++it) {
        if (*it == id) { ids.erase(it); break; }
    }

    tok->from = tok->pos;
    tok->t = 0.0;
    switch (outcome) {
    case OutDropped:
        tok->to = kDropPilePt;  tok->state = Token::ToDrop;      tok->speed = 1.5; break;
    case OutSkipped:
        tok->to = kSkipPilePt;  tok->state = Token::ToSkip;      tok->speed = 1.4; break;
    case OutDisplayed:
        m_consumerBusyUntil = m_clock.elapsed() + 420;
        tok->to = kConsumerPt;  tok->state = Token::ToConsumer;  tok->speed = 1.3; break;
    }
    reflowQueue(pane);
    return true;
}

void Canvas::onFramePushed(Frame frame) {
    ++m_produced;
    Token tok = spawnAtProducer(frame);

    // If a terminal outcome for this frame already arrived (consumer signal
    // overtook this push), spawn the token and route it straight to its
    // outcome instead of parking it in the queue.
    auto pit = m_pendingTerminal.find(frame.id);
    if (pit != m_pendingTerminal.end()) {
        const Outcome outcome = pit->second;
        m_pendingTerminal.erase(pit);
        m_tokens.push_back(tok);
        routeTerminal(frame.id, frame.pane, outcome);
        m_paneBusyUntil[frame.pane] = m_clock.elapsed() + 220;
        emitStats("running");
        return;
    }

    const int slot = queuedCount(frame.pane);
    m_queueIds[frame.pane].push_back(frame.id);
    tok.slot = slot;
    tok.state = Token::ToQueue;
    tok.to = queueSlotPt(frame.pane, slot);
    tok.speed = 1.6;
    m_tokens.push_back(tok);
    m_paneBusyUntil[frame.pane] = m_clock.elapsed() + 220;
    emitStats("running");
}

void Canvas::onFrameDropped(Frame frame) {
    if (!routeTerminal(frame.id, frame.pane, OutDropped))
        m_pendingTerminal[frame.id] = OutDropped;
    emitStats("running");
}

void Canvas::onFrameSkipped(Frame frame) {
    if (!routeTerminal(frame.id, frame.pane, OutSkipped))
        m_pendingTerminal[frame.id] = OutSkipped;
    emitStats("running");
}

void Canvas::onFrameDisplayed(Frame frame) {
    if (!routeTerminal(frame.id, frame.pane, OutDisplayed))
        m_pendingTerminal[frame.id] = OutDisplayed;
    emitStats("running");
}

// ---- animation -------------------------------------------------------------
void Canvas::onTick() {
    const double dt = 1.0 / 60.0;
    for (auto& t : m_tokens) {
        t.pulse += dt;
        switch (t.state) {
        case Token::ToQueue:
        case Token::ToConsumer:
        case Token::ToDisplayed:
        case Token::ToDrop:
        case Token::ToSkip: {
            t.t = qMin(1.0, t.t + dt * t.speed);
            const double e = easeInOut(t.t);
            t.pos = t.from + (t.to - t.from) * e;
            if (t.t >= 1.0)
                arrive(t);
            break;
        }
        case Token::AtConsumer: {
            // Dwell at the consumer (~0.4s) while it "renders" the frame.
            t.t = qMin(1.0, t.t + dt * 2.5);
            if (t.t >= 1.0) {
                t.from = t.pos;
                t.to = kDisplayPilePt;
                t.t = 0.0;
                t.state = Token::ToDisplayed;
                t.speed = 1.6;
            }
            break;
        }
        case Token::InQueue:
        case Token::Displayed:
        case Token::DroppedLanded:
        case Token::SkippedLanded:
            break;
        }
    }
    // Reap tokens that have landed in a pile (the pile keeps its own copy).
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->state == Token::Displayed || it->state == Token::DroppedLanded ||
            it->state == Token::SkippedLanded)
            it = m_tokens.erase(it);
        else
            ++it;
    }
    update();
}

void Canvas::arrive(Token& t) {
    switch (t.state) {
    case Token::ToQueue:
        t.state = Token::InQueue;
        t.pos = t.to;
        break;
    case Token::ToConsumer:
        t.state = Token::AtConsumer;
        t.t = 0.0;
        t.pos = kConsumerPt;
        break;
    case Token::ToDisplayed:
        t.state = Token::Displayed;
        ++m_displayed;
        m_displayPile.push_back({t.shape, t.color});
        if (m_displayPile.size() > 24) m_displayPile.pop_front();
        emitStats("running");
        break;
    case Token::ToDrop:
        t.state = Token::DroppedLanded;
        m_dropPile.push_back({t.shape, t.color});
        if (m_dropPile.size() > 24) m_dropPile.pop_front();
        break;
    case Token::ToSkip:
        t.state = Token::SkippedLanded;
        m_skipPile.push_back({t.shape, t.color});
        if (m_skipPile.size() > 24) m_skipPile.pop_front();
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
    if (m_pipe) {
        m_dropped   = int(m_pipe->dropped());
        m_skipped   = int(m_pipe->skipped());
    }
    emit statsChanged(m_produced, m_dropped, m_skipped, m_displayed, status);
}

// ---- painting --------------------------------------------------------------
void Canvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(24, 26, 34));

    drawPipeline(p);
    drawPile(p, kDisplayPilePt, m_displayPile, 1.0);
    drawPile(p, kDropPilePt, m_dropPile, 0.65);
    drawPile(p, kSkipPilePt, m_skipPile, 0.65);
    // Moving tokens last so they sit above the stations.
    for (const auto& t : m_tokens)
        drawToken(p, t);
}

void Canvas::drawPipeline(QPainter& p) {
    const qint64 now = m_clock.elapsed();

    // Per-pane flow lines: producer -> queue, and queue -> consumer.
    p.setPen(QPen(QColor(60, 66, 86), 3, Qt::DashLine));
    for (int pane = 0; pane < kPaneCount; ++pane) {
        p.drawLine(producerPt(pane), queueSlotPt(pane, 0));
        p.drawLine(queueSlotPt(pane, kCapacity - 1), kConsumerPt);
    }
    // Consumer -> displayed pile.
    p.drawLine(kConsumerPt, kDisplayPilePt);

    // Producer stations + per-pane bounded queues.
    for (int pane = 0; pane < kPaneCount; ++pane) {
        const bool busy = now < m_paneBusyUntil[pane];
        drawStation(p, producerPt(pane),
                    QString("PRODUCER %1\n(decode thread)").arg(pane),
                    busy ? QColor(70, 160, 210) : QColor(70, 130, 200), 40);
        if (busy && m_running)
            drawWorkingRing(p, producerPt(pane), 48);

        // Bounded-queue holding area: a rounded-rect that turns amber when any
        // slot is occupied, with kCapacity slot outlines inside it.
        const QPointF s0 = queueSlotPt(pane, 0);
        const QPointF sN = queueSlotPt(pane, kCapacity - 1);
        QRectF box(s0.x() - 28, s0.y() - 28,
                   (sN.x() - s0.x()) + 56, 56);
        const bool occupied = queuedCount(pane) > 0;
        p.setPen(QPen(occupied ? QColor(230, 170, 60) : QColor(90, 100, 120), 3));
        p.setBrush(QColor(34, 38, 50));
        p.drawRoundedRect(box, 12, 12);
        p.setPen(QPen(QColor(70, 78, 100), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        for (int s = 0; s < kCapacity; ++s) {
            const QPointF c = queueSlotPt(pane, s);
            p.drawEllipse(c, kRadius + 2, kRadius + 2);
        }
    }

    // Queue heading.
    p.setPen(QColor(170, 176, 196));
    drawLabel(p, QPointF(queueSlotPt(0, 1).x(), paneY(0) - 48),
              QString("BOUNDED QUEUES  (capacity %1, drop-oldest)").arg(kCapacity));

    // Single consumer station with a rotating "rendering" ring when active.
    const bool consuming = now < m_consumerBusyUntil;
    drawStation(p, kConsumerPt, "CONSUMER\n(render thread)",
                consuming ? QColor(220, 90, 90) : QColor(150, 120, 210));
    if (consuming) {
        drawWorkingRing(p, kConsumerPt, 58);
    } else if (m_running) {
        p.setPen(QColor(170, 176, 196));
        drawLabel(p, QPointF(kConsumerPt.x(), kConsumerPt.y() + 92),
                  "takes the freshest per pane");
    }

    // Pile labels.
    p.setPen(QColor(150, 130, 210));
    drawLabel(p, QPointF(kDisplayPilePt.x(), kDisplayPilePt.y() - 78), "DISPLAYED");
    p.setPen(QColor(220, 110, 110));
    drawLabel(p, QPointF(kDropPilePt.x(), kDropPilePt.y() - 78),
              "DROPPED\n(backpressure)");
    p.setPen(QColor(210, 170, 90));
    drawLabel(p, QPointF(kSkipPilePt.x(), kSkipPilePt.y() - 78),
              "SKIPPED\n(stale)");

    // Title + subtitle.
    p.setPen(QColor(210, 215, 230));
    QFont title = p.font();
    title.setPointSize(13);
    title.setBold(true);
    p.setFont(title);
    p.drawText(QRectF(20, 8, width() - 40, 26), Qt::AlignLeft,
               "N producer threads  ->  per-pane bounded queues (drop-oldest)  ->  "
               "one consumer takes the freshest");
    QFont sub = p.font();
    sub.setPointSize(9);
    sub.setBold(false);
    p.setFont(sub);
    p.setPen(QColor(150, 156, 174));
    p.drawText(QRectF(20, 32, width() - 40, 22), Qt::AlignLeft,
               "Each pane's producer decodes at its own rate into its own bounded queue; a "
               "full queue drops the OLDEST frame. The single consumer takes the freshest "
               "frame per pane and skips the rest, keeping display latency low.");
}

void Canvas::drawStation(QPainter& p, const QPointF& c, const QString& text,
                         const QColor& col, double radius) {
    p.setPen(QPen(col, 3));
    p.setBrush(QColor(34, 38, 50));
    p.drawEllipse(c, radius, radius);
    p.setPen(QColor(170, 176, 196));
    drawLabel(p, QPointF(c.x(), c.y() + radius + 6), text);
}

void Canvas::drawWorkingRing(QPainter& p, const QPointF& c, double radius) {
    p.save();
    p.translate(c);
    const double ang = std::fmod(m_clock.elapsed() * 0.36, 360.0);
    p.rotate(ang);
    QPen ring(QColor(240, 200, 90), 4);
    ring.setCapStyle(Qt::RoundCap);
    p.setPen(ring);
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(-radius, -radius, 2 * radius, 2 * radius), 0, 270 * 16);
    p.restore();
}

void Canvas::drawLabel(QPainter& p, const QPointF& center, const QString& text) {
    QFont f = p.font();
    f.setPointSize(9);
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
    double r = kRadius;
    if (t.state == Token::AtConsumer)
        r = kRadius + 4 * std::sin(t.pulse * 6.0);   // pulse while "rendering"
    paintShape(p, t.shape, t.pos, r, t.color, 1.0);

    // id badge
    p.setPen(QColor(20, 20, 28));
    QFont f = p.font();
    f.setPointSize(8);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRectF(t.pos.x() - r, t.pos.y() - r, 2 * r, 2 * r),
               Qt::AlignCenter, QString::number(t.id));
}
