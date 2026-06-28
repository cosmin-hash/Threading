// Canvas.cpp
#include "Canvas.h"

#include <cmath>

#include <QCoreApplication>
#include <QFont>
#include <QPainter>
#include <QPen>

#include "FramePipeline.h"

Canvas::Canvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(1180, 660);
    setAutoFillBackground(true);

    m_anim.setInterval(16);                 // ~60 fps
    connect(&m_anim, &QTimer::timeout, this, &Canvas::onTick);

    m_clock.start();
}

// ---- lifecycle -------------------------------------------------------------
void Canvas::start() {
    if (m_running) return;
    m_running = true;
    m_read = m_displayed = 0;
    m_tokens.clear();
    m_displayPile.clear();
    m_dropPile.clear();
    m_pending.clear();
    m_workerBusy = false;

    m_pipe = new FramePipeline(kReadMs, kWorkMs, this);
    // Queued connections: signals cross from the reader/worker threads to here.
    connect(m_pipe, &FramePipeline::frameAccepted,
            this, &Canvas::onFrameAccepted, Qt::QueuedConnection);
    connect(m_pipe, &FramePipeline::frameDropped,
            this, &Canvas::onFrameDropped, Qt::QueuedConnection);
    connect(m_pipe, &FramePipeline::processingStarted,
            this, &Canvas::onProcessingStarted, Qt::QueuedConnection);
    connect(m_pipe, &FramePipeline::frameReady,
            this, &Canvas::onFrameReady, Qt::QueuedConnection);
    m_pipe->start();

    m_anim.start();
    emitStats("running");
}

void Canvas::stop() {
    if (!m_running) return;
    m_running = false;
    emitStats("stopping - joining threads...");
    // Cooperative shutdown + join of both reader and worker threads. Neither is
    // killed mid-frame; this blocks at most one read/processing delay.
    if (m_pipe) {
        m_pipe->shutdown();
        QCoreApplication::processEvents();   // drain late queued signals
        m_pipe->deleteLater();
        m_pipe = nullptr;
    }
    emitStats("stopped - threads joined cleanly");
    update();
}

// ---- model events (all arrive on the GUI thread via queued connections) ----
Token Canvas::spawnAtReader(const Frame& frame) const {
    Token tok;
    tok.id = frame.id;
    tok.shape = frame.shape;
    tok.color = frame.color;
    tok.pos = tok.from = kReaderPt;
    return tok;
}

void Canvas::onFrameAccepted(Frame frame) {
    ++m_read;
    Token tok = spawnAtReader(frame);

    // A worker-thread signal for this frame may already have arrived ahead of us
    // (cross-thread signals have no mutual ordering guarantee). If so, send the
    // freshly spawned token straight to the furthest stage already announced.
    auto it = m_pending.find(frame.id);
    if (it != m_pending.end()) {
        const PendingStage stage = it->second;
        m_pending.erase(it);
        if (stage == StReady) {
            tok.state = Token::ToGui;   // already processed -> head to display
            tok.to = kGuiPt;
            tok.speed = 1.5;
        } else {                        // StStarted -> head to the worker
            tok.state = Token::ToWorker;
            tok.to = kWorkerPt;
            tok.speed = 1.4;
        }
        m_tokens.push_back(tok);
        emitStats("running");
        return;
    }

    tok.state = Token::ToSlot;
    tok.to = kSlotPt;
    tok.speed = 1.6;
    m_tokens.push_back(tok);
    emitStats("running");
}

void Canvas::onFrameDropped(Frame frame) {
    ++m_read;
    Token tok = spawnAtReader(frame);
    // Drift toward the slot, deflect off the full slot, fall into dropped pile.
    tok.state = Token::Bounce;
    tok.to = kBouncePt;
    tok.speed = 1.6;
    m_tokens.push_back(tok);
    emitStats("running");
}

void Canvas::onProcessingStarted(int id) {
    m_workerBusy = true;
    // If the token does not exist yet, the reader's frameAccepted has not been
    // processed; remember that this frame has at least started processing.
    if (!advanceToWorker(id))
        m_pending[id] = StStarted;
}

void Canvas::onFrameReady(Frame frame) {
    m_workerBusy = false;
    // Same guard: park the terminal outcome if the token has not spawned yet.
    if (!advanceToGui(frame.id))
        m_pending[frame.id] = StReady;
}

// Move a slot-bound token to the worker. Returns false if no such token exists.
bool Canvas::advanceToWorker(int id) {
    for (auto& t : m_tokens) {
        if (t.id == id && (t.state == Token::ToSlot || t.state == Token::InSlot)) {
            t.from = t.pos;
            t.to = kWorkerPt;
            t.t = 0.0;
            t.state = Token::ToWorker;
            t.speed = 1.4;
            return true;
        }
    }
    return false;
}

// Move an in-flight token to the GUI display. Returns false if none matches.
bool Canvas::advanceToGui(int id) {
    for (auto& t : m_tokens) {
        if (t.id == id &&
            (t.state == Token::Processing || t.state == Token::ToWorker ||
             t.state == Token::InSlot || t.state == Token::ToSlot)) {
            t.from = t.pos;
            t.to = kGuiPt;
            t.t = 0.0;
            t.state = Token::ToGui;
            t.speed = 1.5;
            return true;
        }
    }
    return false;
}

// ---- animation -------------------------------------------------------------
void Canvas::onTick() {
    const double dt = 1.0 / 60.0;
    for (auto& t : m_tokens) {
        t.pulse += dt;
        switch (t.state) {
        case Token::ToSlot:
        case Token::ToWorker:
        case Token::ToGui:
        case Token::Bounce:
        case Token::ToDrop: {
            t.t = qMin(1.0, t.t + dt * t.speed);
            const double e = easeInOut(t.t);
            t.pos = t.from + (t.to - t.from) * e;
            if (t.t >= 1.0)
                arrive(t);
            break;
        }
        case Token::InSlot:
        case Token::Processing:
        case Token::Displayed:
        case Token::Dropped:
            break;
        }
    }
    // Reap tokens that have landed in a pile (the pile keeps its own copy).
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->state == Token::Displayed || it->state == Token::Dropped)
            it = m_tokens.erase(it);
        else
            ++it;
    }
    update();
}

void Canvas::arrive(Token& t) {
    switch (t.state) {
    case Token::ToSlot:
        t.state = Token::InSlot;
        t.pos = kSlotPt;
        break;
    case Token::ToWorker:
        t.state = Token::Processing;
        t.pos = kWorkerPt;
        break;
    case Token::ToGui:
        t.state = Token::Displayed;
        ++m_displayed;
        m_displayPile.push_back({t.shape, t.color});
        if (m_displayPile.size() > 24) m_displayPile.pop_front();
        emitStats("running");
        break;
    case Token::Bounce:
        // Deflected off the full slot; now fall into the dropped pile.
        t.from = t.pos;
        t.to = kDropPt;
        t.t = 0.0;
        t.state = Token::ToDrop;
        t.speed = 1.4;
        break;
    case Token::ToDrop:
        t.state = Token::Dropped;
        m_dropPile.push_back({t.shape, t.color});
        if (m_dropPile.size() > 24) m_dropPile.pop_front();
        break;
    default:
        break;
    }
}

// ---- helpers ---------------------------------------------------------------
double Canvas::easeInOut(double x) {
    return x < 0.5 ? 2 * x * x : 1 - std::pow(-2 * x + 2, 2) / 2;
}

bool Canvas::slotOccupied() const {
    for (const auto& t : m_tokens)
        if (t.state == Token::InSlot) return true;
    return false;
}

void Canvas::emitStats(const QString& status) {
    const int drops = m_pipe ? int(m_pipe->dropped()) : m_lastDropped;
    emit statsChanged(m_read, drops, m_displayed, status);
    if (m_pipe) m_lastDropped = drops;
}

// ---- painting --------------------------------------------------------------
void Canvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(24, 26, 34));

    drawPipeline(p);
    drawPile(p, kDisplayPt, m_displayPile, 1.0);
    drawPile(p, kDropPt, m_dropPile, 0.65);
    // Moving tokens last so they sit above the stations.
    for (const auto& t : m_tokens)
        drawToken(p, t);
}

void Canvas::drawPipeline(QPainter& p) {
    // Flow line reader -> slot -> worker -> GUI.
    p.setPen(QPen(QColor(60, 66, 86), 3, Qt::DashLine));
    p.drawLine(kReaderPt, kSlotPt);
    p.drawLine(kSlotPt, kWorkerPt);
    p.drawLine(kWorkerPt, kGuiPt);

    // Reader station (the input source / producer thread).
    drawStation(p, kReaderPt, "READER THREAD\n(input source)", QColor(70, 130, 200));

    // Single hand-off slot.
    QRectF slotBox(kSlotPt.x() - 46, kSlotPt.y() - 46, 92, 92);
    const bool occupied = slotOccupied();
    p.setPen(QPen(occupied ? QColor(230, 170, 60) : QColor(90, 100, 120), 3));
    p.setBrush(QColor(34, 38, 50));
    p.drawRoundedRect(slotBox, 12, 12);
    p.setPen(QColor(170, 176, 196));
    drawLabel(p, QPointF(kSlotPt.x(), kSlotPt.y() - 82),
              "SINGLE-SLOT HAND-OFF\n(mutex + condition var)");

    // Worker station with a rotating "processing" ring when busy.
    drawStation(p, kWorkerPt, "WORKER THREAD\n(processing)",
                m_workerBusy ? QColor(220, 90, 90) : QColor(90, 170, 110));
    if (m_workerBusy) {
        p.save();
        p.translate(kWorkerPt);
        const double ang = std::fmod(m_clock.elapsed() * 0.36, 360.0);
        p.rotate(ang);
        QPen ring(QColor(240, 200, 90), 4);
        ring.setCapStyle(Qt::RoundCap);
        p.setPen(ring);
        p.drawArc(QRectF(-58, -58, 116, 116), 0, 270 * 16);
        p.restore();
    } else if (m_running) {
        p.setPen(QColor(170, 176, 196));
        drawLabel(p, QPointF(kWorkerPt.x(), kWorkerPt.y() + 92),
                  "sleeping on CV (0% CPU)");
    }

    // GUI station (the final consumer that displays finished frames).
    drawStation(p, kGuiPt, "GUI THREAD\n(display / consumer)", QColor(150, 120, 210));

    // Pile labels.
    p.setPen(QColor(150, 130, 210));
    drawLabel(p, QPointF(kDisplayPt.x(), kDisplayPt.y() - 78), "DISPLAYED");
    p.setPen(QColor(220, 110, 110));
    drawLabel(p, QPointF(kDropPt.x(), kDropPt.y() - 78), "DROPPED");

    // Title + subtitle.
    p.setPen(QColor(210, 215, 230));
    QFont title = p.font();
    title.setPointSize(13);
    title.setBold(true);
    p.setFont(title);
    p.drawText(QRectF(20, 12, width() - 40, 30), Qt::AlignLeft,
               "Reader thread  ->  single-slot hand-off  ->  worker thread  ->  GUI display");
    QFont sub = p.font();
    sub.setPointSize(9);
    sub.setBold(false);
    p.setFont(sub);
    p.setPen(QColor(150, 156, 174));
    p.drawText(QRectF(20, 38, width() - 40, 22), Qt::AlignLeft,
               "A reader thread pulls frames from the input source faster than the worker can "
               "process them; while the slot is full, frames are dropped. The GUI thread only "
               "displays finished frames.");
}

void Canvas::drawStation(QPainter& p, const QPointF& c, const QString& text,
                         const QColor& col) {
    p.setPen(QPen(col, 3));
    p.setBrush(QColor(34, 38, 50));
    p.drawEllipse(c, 50, 50);
    p.setPen(QColor(170, 176, 196));
    drawLabel(p, QPointF(c.x(), c.y() + 70), text);
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
    if (t.state == Token::Processing)
        r = kRadius + 4 * std::sin(t.pulse * 6.0);   // gentle pulse
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
