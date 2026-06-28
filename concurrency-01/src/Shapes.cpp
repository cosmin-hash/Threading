// Shapes.cpp
#include "Shapes.h"

#include <QPainter>
#include <QPolygonF>
#include <QtMath>
#include <cmath>

void paintShape(QPainter& p, int shape, const QPointF& c, double r,
                QColor color, double alpha) {
    alpha = qBound(0.0, alpha, 1.0);
    color.setAlphaF(alpha);
    QColor edge = color.darker(160);
    edge.setAlphaF(alpha);
    p.setBrush(color);
    p.setPen(QPen(edge, 2));

    switch (shape) {
    case Circle:
        p.drawEllipse(c, r, r);
        break;
    case Square:
        p.drawRoundedRect(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), 4, 4);
        break;
    case Rectangle:
        p.drawRoundedRect(QRectF(c.x() - r * 1.3, c.y() - r * 0.75,
                                 2.6 * r, 1.5 * r), 4, 4);
        break;
    case Triangle: {
        QPolygonF tri;
        for (int i = 0; i < 3; ++i) {
            double a = -M_PI / 2 + i * 2 * M_PI / 3;
            tri << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
        }
        p.drawPolygon(tri);
        break;
    }
    case Hexagon: {
        QPolygonF hex;
        for (int i = 0; i < 6; ++i) {
            double a = i * M_PI / 3;
            hex << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
        }
        p.drawPolygon(hex);
        break;
    }
    case Star: {
        QPolygonF star;
        for (int i = 0; i < 10; ++i) {
            double rad = (i % 2 == 0) ? r : r * 0.45;
            double a = -M_PI / 2 + i * M_PI / 5;
            star << QPointF(c.x() + rad * std::cos(a), c.y() + rad * std::sin(a));
        }
        p.drawPolygon(star);
        break;
    }
    default:
        p.drawEllipse(c, r, r);
    }
}
