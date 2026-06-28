// Canvas.cpp
#include "Canvas.h"

#include <cmath>

#include <QCoreApplication>
#include <QFont>
#include <QPainter>
#include <QPen>

#include "PipelineEngine.h"

Canvas::Canvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(1200, 700);
    setAutoFillBackground(true);

    m_anim.setInterval(16);                 // ~60 fps
    connect(&m_anim, &QTimer::timeout, this, &Canvas::onTick);

    m_clock.start();
}

// ---- lifecycle -------------------------------------------------------------
void Canvas::start() {
    if (m_running) return;
    m_running = true;
    m_produced = m_consumed = m_stalls = 0;
    m_sourceBlocked = false;
    m_tokens.clear();
    m_consumedPile.clear();
    m_throttledPile.clear();
    m_pending.clear();
    m_workerBusy.fill(false);

    m_engine = new PipelineEngine(kSourceMs, kWorkMs, this);
    // Queued connections: every signal crosses from a worker thread (source,
    // transform, a pool worker, or the sink) onto the GUI thread's event loop.
    connect(m_engine, &PipelineEngine::itemProduced,
            this, &Canvas::onItemProduced, Qt::QueuedConnection);
    connect(m_engine, &PipelineEngine::backpressure,
            this, &Canvas::onBackpressure, Qt::QueuedConnection);
    connect(m_engine, &PipelineEngine::transformTook,
            this, &Canvas::onTransformTook, Qt::QueuedConnection);
    connect(m_engine, &PipelineEngine::workerStarted,
            this, &Canvas::onWorkerStarted, Qt::QueuedConnection);
    connect(m_engine, &PipelineEngine::resultBuffered,
            this, &Canvas::onResultBuffered, Qt::QueuedConnection);
    connect(m_engine, &PipelineEngine::itemConsumed,
            this, &Canvas::onItemConsumed, Qt::QueuedConnection);
    m_engine->start();

    m_anim.start();
    emitStats("running");
}

void Canvas::stop() {
    if (!m_running) return;
    m_running = false;
    emitStats("stopping - closing channels, joining threads...");
    // Cooperative shutdown + join of source, transform, sink and the pool.
    // No thread is killed mid-work; this blocks at most one work delay.
    if (m_engine) {
        m_engine->shutdown();
        QCoreApplication::processEvents();   // drain late queued signals
        m_engine->deleteLater();
        m_engine = nullptr;
    }
    m_sourceBlocked = false;
    m_workerBusy.fill(false);
    emitStats("stopped - threads joined cleanly");
    update();
}

// ---- model events (all arrive on the GUI thread via queued connections) ----
Token Canvas::spawnAtSource(const WorkItem& item) const {
    Token tok;
    tok.id = item.id;
    tok.shape = item.shape;
    tok.color = item.color;
    tok.pos = tok.from = kSourcePt;
    return tok;
}

void Canvas::onItemProduced(WorkItem item) {
    ++m_produced;
    m_sourceBlocked = false;          // the push that was blocked has gone through
    Token tok = spawnAtSource(item);

    // A downstream signal for this item may already have arrived ahead of us
    // (cross-thread signals have no mutual ordering guarantee). If so, fast
    // forward the freshly spawned token to the furthest stage already announced.
    auto it = m_pending.find(item.id);
    if (it != m_pending.end()) {
        fastForward(tok, it->second);
        m_pending.erase(it);
        m_tokens.push_back(tok);
        emitStats("running");
        return;
    }

    tok.state = Token::ToChan1;
    tok.to = kChan1Pt;
    tok.speed = 1.5;
    m_tokens.push_back(tok);
    emitStats("running");
}

void Canvas::onBackpressure(WorkItem item) {
    ++m_stalls;
    m_sourceBlocked = true;
    // The item is NOT dropped - it will still flow through once a slot frees.
    // We mark the throttle with a dim ghost falling into the THROTTLED pile.
    Token ghost = spawnAtSource(item);
    ghost.ghost = true;
    ghost.pos = ghost.from = kChan1Pt;
    ghost.state = Token::ToThrottled;
    ghost.to = kThrottledPt;
    ghost.speed = 1.4;
    m_tokens.push_back(ghost);
    emitStats("running (source throttled)");
}

void Canvas::onTransformTook(int id) {
    if (!advanceToTransform(id))
        m_pending[id] = {StTook, -1};
}

void Canvas::onWorkerStarted(int id, int worker) {
    if (worker >= 0 && worker < kPoolSize) m_workerBusy[worker] = true;
    if (!advanceToWorker(id, worker))
        m_pending[id] = {StStarted, worker};
}

void Canvas::onResultBuffered(int id, int worker) {
    if (worker >= 0 && worker < kPoolSize) m_workerBusy[worker] = false;
    if (!advanceToChan2(id, worker))
        m_pending[id] = {StBuffered, worker};
}

void Canvas::onItemConsumed(WorkItem item) {
    if (!advanceToSink(item.id))
        m_pending[item.id] = {StConsumed, -1};
}

// ---- token re-routing helpers (each returns false if the token isn't known) -
bool Canvas::advanceToTransform(int id) {
    for (auto& t : m_tokens)
        if (t.id == id && !t.ghost &&
            (t.state == Token::ToChan1 || t.state == Token::InChan1)) {
            t.from = t.pos; t.to = kTransformPt; t.t = 0.0;
            t.state = Token::ToTransform; t.speed = 1.5;
            return true;
        }
    return false;
}

bool Canvas::advanceToWorker(int id, int worker) {
    for (auto& t : m_tokens)
        if (t.id == id && !t.ghost &&
            (t.state == Token::ToChan1 || t.state == Token::InChan1 ||
             t.state == Token::ToTransform || t.state == Token::AtTransform)) {
            t.worker = worker;
            t.from = t.pos; t.to = kPoolPt(worker); t.t = 0.0;
            t.state = Token::ToWorker; t.speed = 1.4;
            return true;
        }
    return false;
}

bool Canvas::advanceToChan2(int id, int worker) {
    for (auto& t : m_tokens)
        if (t.id == id && !t.ghost &&
            (t.state != Token::ToChan2 && t.state != Token::InChan2 &&
             t.state != Token::ToSink  && t.state != Token::ToConsumed &&
             t.state != Token::Consumed)) {
            if (t.worker < 0) t.worker = worker;
            t.from = t.pos; t.to = kChan2Pt; t.t = 0.0;
            t.state = Token::ToChan2; t.speed = 1.4;
            return true;
        }
    return false;
}

bool Canvas::advanceToSink(int id) {
    for (auto& t : m_tokens)
        if (t.id == id && !t.ghost &&
            (t.state != Token::ToSink && t.state != Token::ToConsumed &&
             t.state != Token::Consumed)) {
            t.from = t.pos; t.to = kSinkPt; t.t = 0.0;
            t.state = Token::ToSink; t.speed = 1.5;
            return true;
        }
    return false;
}

// Route a freshly spawned token straight to the furthest stage already reached.
void Canvas::fastForward(Token& t, const PendingStage& ps) const {
    t.from = t.pos; t.t = 0.0;
    switch (ps.stage) {
    case StTook:
        t.state = Token::ToTransform; t.to = kTransformPt; t.speed = 1.5;
        break;
    case StStarted:
        t.worker = ps.worker;
        t.state = Token::ToWorker; t.to = kPoolPt(ps.worker); t.speed = 1.4;
        break;
    case StBuffered:
        t.worker = ps.worker;
        t.state = Token::ToChan2; t.to = kChan2Pt; t.speed = 1.4;
        break;
    case StConsumed:
        t.state = Token::ToSink; t.to = kSinkPt; t.speed = 1.5;
        break;
    }
}

// ---- animation -------------------------------------------------------------
void Canvas::onTick() {
    const double dt = 1.0 / 60.0;
    for (auto& t : m_tokens) {
        t.pulse += dt;
        switch (t.state) {
        case Token::ToChan1:
        case Token::ToTransform:
        case Token::ToWorker:
        case Token::ToChan2:
        case Token::ToSink:
        case Token::ToConsumed:
        case Token::ToThrottled: {
            t.t = qMin(1.0, t.t + dt * t.speed);
            t.pos = t.from + (t.to - t.from) * easeInOut(t.t);
            if (t.t >= 1.0) arrive(t);
            break;
        }
        default:    // InChan1, AtTransform, Processing, InChan2, landed states
            break;
        }
    }
    layoutChannels();

    // Reap tokens that have landed in a pile (the pile keeps its own copy).
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->state == Token::Consumed || it->state == Token::Throttled)
            it = m_tokens.erase(it);
        else
            ++it;
    }
    update();
}

void Canvas::arrive(Token& t) {
    switch (t.state) {
    case Token::ToChan1:
        t.state = Token::InChan1; t.pos = kChan1Pt;
        break;
    case Token::ToTransform:
        t.state = Token::AtTransform; t.pos = kTransformPt;
        break;
    case Token::ToWorker:
        t.state = Token::Processing; t.pos = kPoolPt(t.worker);
        break;
    case Token::ToChan2:
        t.state = Token::InChan2; t.pos = kChan2Pt;
        break;
    case Token::ToSink:
        // The sink already consumed it; carry on into the consumed pile.
        t.from = kSinkPt; t.to = kConsumedPt; t.t = 0.0;
        t.state = Token::ToConsumed; t.speed = 1.5;
        break;
    case Token::ToConsumed:
        t.state = Token::Consumed;
        ++m_consumed;
        m_consumedPile.push_back({t.shape, t.color});
        if (m_consumedPile.size() > 24) m_consumedPile.pop_front();
        emitStats("running");
        break;
    case Token::ToThrottled:
        t.state = Token::Throttled;
        m_throttledPile.push_back({t.shape, t.color});
        if (m_throttledPile.size() > 24) m_throttledPile.pop_front();
        break;
    default:
        break;
    }
}

// Restack the buffered tokens into their channel's slots (FIFO order) so the
// channels visibly fill and drain.
void Canvas::layoutChannels() {
    int i1 = 0, i2 = 0;
    for (auto& t : m_tokens) {
        if (t.state == Token::InChan1)
            t.pos = kChanSlotPt(kChan1Pt, qMin(i1++, kChannelCap - 1));
        else if (t.state == Token::InChan2)
            t.pos = kChanSlotPt(kChan2Pt, qMin(i2++, kChannelCap - 1));
    }
}

int Canvas::channelOccupancy(Token::State inState) const {
    int n = 0;
    for (const auto& t : m_tokens) if (t.state == inState) ++n;
    return n;
}

// ---- helpers ---------------------------------------------------------------
double Canvas::easeInOut(double x) {
    return x < 0.5 ? 2 * x * x : 1 - std::pow(-2 * x + 2, 2) / 2;
}

void Canvas::emitStats(const QString& status) {
    emit statsChanged(m_produced, m_stalls, m_consumed, status);
}

// ---- painting --------------------------------------------------------------
void Canvas::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(24, 26, 34));

    drawPipeline(p);
    drawPile(p, kConsumedPt, m_consumedPile, 1.0);
    drawPile(p, kThrottledPt, m_throttledPile, 0.65);
    // Moving tokens last so they sit above the stations.
    for (const auto& t : m_tokens)
        drawToken(p, t);
}

void Canvas::drawPipeline(QPainter& p) {
    // Flow lines (dashed): source -> chan1 -> transform =fan out=> pool
    //                      =fan in=> chan2 -> sink -> consumed pile.
    p.setPen(QPen(QColor(60, 66, 86), 3, Qt::DashLine));
    p.drawLine(kSourcePt, kChan1Pt);
    p.drawLine(kChan1Pt, kTransformPt);
    for (int i = 0; i < kPoolSize; ++i) {
        p.drawLine(kTransformPt, kPoolPt(i));
        p.drawLine(kPoolPt(i), kChan2Pt);
    }
    p.drawLine(kChan2Pt, kSinkPt);
    p.drawLine(kSinkPt, kConsumedPt);
    // The throttle "spill" line down to the dimmed pile.
    p.setPen(QPen(QColor(120, 90, 60), 2, Qt::DotLine));
    p.drawLine(kChan1Pt, kThrottledPt);

    // SOURCE station (turns red while throttled by backpressure).
    drawStation(p, kSourcePt, "SOURCE THREAD\n(producer)",
                m_sourceBlocked ? QColor(220, 90, 90) : QColor(70, 130, 200));
    if (m_sourceBlocked) {
        p.setPen(QColor(220, 150, 120));
        drawLabel(p, QPointF(kSourcePt.x(), kSourcePt.y() + 92),
                  "BLOCKED on full channel\n(backpressure)");
    }

    // CHANNEL 1 (bounded, capacity kChannelCap).
    drawChannel(p, kChan1Pt, "BOUNDED CHANNEL 1\n(cap 4, blocks when full)",
                channelOccupancy(Token::InChan1));

    // TRANSFORM dispatcher.
    drawStation(p, kTransformPt, "TRANSFORM\n(dispatcher)", QColor(80, 170, 160));

    // POOL worker stations, each with a rotating arc when busy.
    for (int i = 0; i < kPoolSize; ++i) {
        const QPointF c = kPoolPt(i);
        const bool busy = m_workerBusy[i];
        drawStation(p, c, QString("POOL #%1").arg(i),
                    busy ? QColor(220, 150, 70) : QColor(90, 170, 110));
        if (busy) {
            p.save();
            p.translate(c);
            const double ang = std::fmod(m_clock.elapsed() * 0.36 + i * 40, 360.0);
            p.rotate(ang);
            QPen ring(QColor(240, 200, 90), 4);
            ring.setCapStyle(Qt::RoundCap);
            p.setPen(ring);
            p.drawArc(QRectF(-40, -40, 80, 80), 0, 270 * 16);
            p.restore();
        }
    }

    // CHANNEL 2 (bounded, capacity kChannelCap).
    drawChannel(p, kChan2Pt, "BOUNDED CHANNEL 2\n(cap 4, blocks when full)",
                channelOccupancy(Token::InChan2));

    // SINK station.
    drawStation(p, kSinkPt, "SINK THREAD\n(consumer)", QColor(150, 120, 210));

    // Pile labels.
    p.setPen(QColor(150, 130, 210));
    drawLabel(p, QPointF(kConsumedPt.x(), kConsumedPt.y() - 78), "CONSUMED");
    p.setPen(QColor(210, 150, 110));
    drawLabel(p, QPointF(kThrottledPt.x(), kThrottledPt.y() - 78),
              "THROTTLED (backpressure stalls)");

    // Title + subtitle.
    p.setPen(QColor(210, 215, 230));
    QFont title = p.font();
    title.setPointSize(13);
    title.setBold(true);
    p.setFont(title);
    p.drawText(QRectF(20, 12, width() - 40, 30), Qt::AlignLeft,
               "Source  ->  bounded channel  ->  transform  ->  thread pool  ->  bounded channel  ->  sink");
    QFont sub = p.font();
    sub.setPointSize(9);
    sub.setBold(false);
    p.setFont(sub);
    p.setPen(QColor(150, 156, 174));
    p.drawText(QRectF(20, 38, width() - 40, 40), Qt::AlignLeft | Qt::TextWordWrap,
               "A 3-stage pipeline composed from bounded channels and a thread pool. A full channel BLOCKS its "
               "producer (true backpressure that throttles, never drops);\n"
               "the transform fans each item out across the pool and fans the results back in. The GUI thread only paints.");
}

void Canvas::drawStation(QPainter& p, const QPointF& c, const QString& text,
                         const QColor& col) {
    p.setPen(QPen(col, 3));
    p.setBrush(QColor(34, 38, 50));
    p.drawEllipse(c, 35, 35);
    p.setPen(QColor(170, 176, 196));
    drawLabel(p, QPointF(c.x(), c.y() + 44), text);
}

void Canvas::drawChannel(QPainter& p, const QPointF& c, const QString& topLabel,
                         int occupied) {
    const double w = 56.0;
    const double h = (kChannelCap - 1) * kChanSlotGap + 50.0;
    QRectF box(c.x() - w / 2, c.y() - h / 2, w, h);
    const bool full = occupied >= kChannelCap;

    p.setPen(QPen(full ? QColor(230, 170, 60) : QColor(90, 100, 120), 3));
    p.setBrush(QColor(34, 38, 50));
    p.drawRoundedRect(box, 10, 10);

    // Slot markers; a filled slot glows amber.
    for (int i = 0; i < kChannelCap; ++i) {
        const QPointF s = kChanSlotPt(c, i);
        const bool filled = i < occupied;
        p.setPen(QPen(filled ? QColor(230, 170, 60) : QColor(70, 78, 98), 1.5));
        p.setBrush(filled ? QColor(70, 60, 40) : QColor(28, 31, 41));
        p.drawRoundedRect(QRectF(s.x() - 20, s.y() - 12, 40, 24), 5, 5);
    }

    p.setPen(QColor(170, 176, 196));
    drawLabel(p, QPointF(c.x(), c.y() - h / 2 - 34), topLabel);
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
        r = kRadius + 4 * std::sin(t.pulse * 6.0);   // gentle pulse while working
    const double alpha = t.ghost ? 0.5 : 1.0;
    paintShape(p, t.shape, t.pos, r, t.color, alpha);

    // id badge
    p.setPen(QColor(20, 20, 28));
    QFont f = p.font();
    f.setPointSize(8);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRectF(t.pos.x() - r, t.pos.y() - r, 2 * r, 2 * r),
               Qt::AlignCenter, QString::number(t.id));
}
