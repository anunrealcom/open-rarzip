#include "iconfactory.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <cmath>

namespace yasuo {
namespace {

void drawIcon(QPainter& p, IconKind kind, const QColor& color) {
    QPen pen(color);
    pen.setWidthF(1.7);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (kind) {
    case IconKind::Minimize:
        p.drawLine(QPointF(6, 16.5), QPointF(18, 16.5));
        break;
    case IconKind::Maximize:
        p.drawRoundedRect(QRectF(6, 6, 12, 12), 2, 2);
        break;
    case IconKind::Restore:
        p.drawRoundedRect(QRectF(6, 8, 10, 10), 2, 2);
        p.drawRoundedRect(QRectF(9, 6, 9, 9), 2, 2);
        break;
    case IconKind::Close:
        p.drawLine(QPointF(6.5, 6.5), QPointF(17.5, 17.5));
        p.drawLine(QPointF(17.5, 6.5), QPointF(6.5, 17.5));
        break;

    case IconKind::AddFile:
        p.drawRoundedRect(QRectF(5, 3, 14, 18), 2.5, 2.5);
        p.drawLine(QPointF(8.5, 12), QPointF(15.5, 12));
        p.drawLine(QPointF(12, 8.5), QPointF(12, 15.5));
        break;
    case IconKind::AddFolder: {
        QPainterPath path;
        path.moveTo(3, 8); path.lineTo(9, 8); path.lineTo(11, 10);
        path.lineTo(21, 10); path.lineTo(21, 19); path.lineTo(3, 19);
        path.closeSubpath();
        p.drawPath(path);
        p.drawLine(QPointF(12, 12), QPointF(12, 17));
        p.drawLine(QPointF(9.5, 14.5), QPointF(14.5, 14.5));
        break;
    }
    case IconKind::Remove:
        p.drawEllipse(QRectF(4, 4, 16, 16));
        p.drawLine(QPointF(8, 12), QPointF(16, 12));
        break;
    case IconKind::Clear:
        p.drawLine(QPointF(5, 7), QPointF(19, 7));
        p.drawLine(QPointF(9, 7), QPointF(9, 5));
        p.drawLine(QPointF(9, 5), QPointF(15, 5));
        p.drawLine(QPointF(15, 5), QPointF(15, 7));
        {
            QPainterPath can;
            can.moveTo(7, 7); can.lineTo(8, 20); can.lineTo(16, 20); can.lineTo(17, 7);
            p.drawPath(can);
            p.drawLine(QPointF(10.6, 10.5), QPointF(10.9, 17));
            p.drawLine(QPointF(13.4, 10.5), QPointF(13.1, 17));
        }
        break;
    case IconKind::Browse:
        p.drawEllipse(QRectF(4, 4, 11, 11));
        p.drawLine(QPointF(15.5, 15.5), QPointF(20.5, 20.5));
        break;
    case IconKind::Start: {
        QPolygonF tri;
        tri << QPointF(8, 5.5) << QPointF(19, 12) << QPointF(8, 18.5);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawPolygon(tri);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        break;
    }
    case IconKind::Cancel:
        p.drawRoundedRect(QRectF(6.5, 6.5, 11, 11), 2, 2);
        break;
    case IconKind::Check:
        p.drawLine(QPointF(5, 12.5), QPointF(10, 17));
        p.drawLine(QPointF(10, 17), QPointF(19, 7));
        break;

    case IconKind::NavCompress:
        p.drawRoundedRect(QRectF(4, 7, 16, 13), 2.5, 2.5);
        p.drawLine(QPointF(4, 10.5), QPointF(20, 10.5));
        p.drawLine(QPointF(10, 13.5), QPointF(14, 13.5));
        break;
    case IconKind::NavArchive: {
        QPainterPath path;
        path.moveTo(3, 8); path.lineTo(9, 8); path.lineTo(11, 10);
        path.lineTo(21, 10); path.lineTo(21, 19); path.lineTo(3, 19);
        path.closeSubpath();
        p.drawPath(path);
        break;
    }
    case IconKind::NavTask:
        p.drawEllipse(QRectF(4, 4, 16, 16));
        p.drawLine(QPointF(12, 12), QPointF(12, 7.5));
        p.drawLine(QPointF(12, 12), QPointF(15.5, 13.5));
        break;
    case IconKind::NavSettings:
        p.drawLine(QPointF(4, 8), QPointF(20, 8));
        p.drawLine(QPointF(4, 12), QPointF(20, 12));
        p.drawLine(QPointF(4, 16), QPointF(20, 16));
        p.setBrush(color);
        p.drawEllipse(QPointF(9, 8), 2.0, 2.0);
        p.drawEllipse(QPointF(15, 12), 2.0, 2.0);
        p.drawEllipse(QPointF(11, 16), 2.0, 2.0);
        p.setBrush(Qt::NoBrush);
        break;

    case IconKind::Sun:
        p.drawEllipse(QRectF(8.5, 8.5, 7, 7));
        for (int i = 0; i < 8; ++i) {
            const double a = i * 3.14159265358979 / 4.0;
            p.drawLine(QPointF(12 + 6.0 * std::cos(a), 12 + 6.0 * std::sin(a)),
                       QPointF(12 + 8.3 * std::cos(a), 12 + 8.3 * std::sin(a)));
        }
        break;
    case IconKind::Moon: {
        QPainterPath outer;
        outer.addEllipse(QRectF(5, 4, 15, 15));
        QPainterPath inner;
        inner.addEllipse(QRectF(10, 1.5, 15, 15));
        p.drawPath(outer.subtracted(inner));
        break;
    }
    }
}

} // namespace

QIcon Icons::make(IconKind kind, const QColor& color, int size) {
    const qreal dpr = 2.0;
    const int px = qMax(1, qRound(size * dpr));
    QPixmap pm(px, px);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal s = px / 24.0;
    p.scale(s, s);
    drawIcon(p, kind, color);
    p.end();
    pm.setDevicePixelRatio(dpr);
    return QIcon(pm);
}

} // namespace yasuo
