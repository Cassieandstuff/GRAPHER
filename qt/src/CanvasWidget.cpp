#include <grapher/qt/CanvasWidget.h>

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <cmath>

namespace grapher::qt {

namespace {

QColor Q(Color c) { return QColor(c.r, c.g, c.b, c.a); }
QPointF Q(Vec2 v) { return {v.x, v.y}; }
QRectF Q(const Rect& r) { return {r.x, r.y, r.w, r.h}; }
Vec2 V(QPointF p) { return {static_cast<float>(p.x()), static_cast<float>(p.y())}; }

Button ButtonOf(Qt::MouseButton b)
{
    switch (b) {
    case Qt::MiddleButton: return Button::Middle;
    case Qt::RightButton: return Button::Right;
    default: return Button::Left;
    }
}

Modifiers ModsOf(Qt::KeyboardModifiers m)
{
    return {m.testFlag(Qt::ShiftModifier), m.testFlag(Qt::ControlModifier), m.testFlag(Qt::AltModifier)};
}

template <class... F> struct Overloaded : F... { using F::operator()...; };

} // namespace

void Paint(QPainter& p, const DrawList& list)
{
    static const QFont kMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    for (const DrawCmd& cmd : list.cmds) {
        std::visit(Overloaded{
            [&](const FillRect& r) {
                p.setPen(r.strokeWidth > 0.f ? QPen(Q(r.stroke), r.strokeWidth) : QPen(Qt::NoPen));
                p.setBrush(Q(r.fill));
                if (r.radius > 0.f) p.drawRoundedRect(Q(r.rect), r.radius, r.radius);
                else p.drawRect(Q(r.rect));
            },
            [&](const Polyline& l) {
                if (l.points.size() < 2) return;
                QPen pen(Q(l.color), l.width);
                pen.setCapStyle(Qt::RoundCap);
                if (l.dashed) pen.setStyle(Qt::DashLine);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                QPainterPath path(Q(l.points.front()));
                for (std::size_t i = 1; i < l.points.size(); ++i) path.lineTo(Q(l.points[i]));
                p.drawPath(path);
            },
            [&](const Polygon& g) {
                if (g.points.size() < 3) return;
                QPolygonF poly;
                for (Vec2 v : g.points) poly << Q(v);
                p.setPen(Qt::NoPen);
                p.setBrush(Q(g.fill));
                p.drawPolygon(poly);
            },
            [&](const Circle& c) {
                p.setPen(c.strokeWidth > 0.f ? QPen(Q(c.stroke), c.strokeWidth) : QPen(Qt::NoPen));
                p.setBrush(Q(c.fill));
                p.drawEllipse(Q(c.center), c.radius, c.radius);
            },
            [&](const Text& t) {
                if (t.size < 3.f || t.box.w < 2.f || t.box.h < 2.f) return;
                QFont f = t.mono ? kMono : QApplication::font();
                f.setPixelSize(std::max(1, static_cast<int>(std::lround(t.size))));
                f.setBold(t.bold);
                p.setFont(f);
                p.setPen(Q(t.color));
                const QString text  = QString::fromUtf8(t.text.data(), static_cast<qsizetype>(t.text.size()));
                const QRectF  box   = Q(t.box);
                const int     align = (t.align == Align::Center ? Qt::AlignHCenter : Qt::AlignLeft);
                if (t.wrap) {
                    p.save();
                    p.setClipRect(box);
                    p.drawText(box, align | Qt::AlignTop | Qt::TextWordWrap, text);
                    p.restore();
                } else {
                    const QString line = QFontMetricsF(f).elidedText(text.section(QLatin1Char('\n'), 0, 0), Qt::ElideRight, box.width());
                    p.drawText(box, align | Qt::AlignVCenter, line);
                }
            },
        }, cmd);
    }
}

CanvasWidget::CanvasWidget(Canvas& canvas, QWidget* parent) : QWidget(parent), canvas_(canvas)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void CanvasWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    Paint(p, canvas_.Draw());
}

void CanvasWidget::resizeEvent(QResizeEvent*)
{
    canvas_.SetViewport({static_cast<float>(width()), static_cast<float>(height())});
}

void CanvasWidget::mousePressEvent(QMouseEvent* e)
{
    emit interacted();
    setFocus(Qt::MouseFocusReason);
    if (e->button() == Qt::RightButton) rightDown_ = e->position();
    if (canvas_.PointerDown(V(e->position()), ButtonOf(e->button()), ModsOf(e->modifiers()))) update();
}

void CanvasWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (canvas_.PointerMove(V(e->position()))) update();
    if (canvas_.Busy()) return;
    switch (canvas_.HitTest(V(e->position())).kind) {
    case HitKind::Resize: setCursor(Qt::SizeFDiagCursor); break;
    case HitKind::Connect: setCursor(Qt::CrossCursor); break;
    case HitKind::Edge: setCursor(Qt::PointingHandCursor); break;
    default: unsetCursor(); break;
    }
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* e)
{
    canvas_.PointerUp(V(e->position()), ButtonOf(e->button()));
    update();
    if (e->button() == Qt::RightButton && QLineF(rightDown_, e->position()).length() < 4.0) {
        const Hit  hit   = canvas_.HitTest(V(e->position()));
        const Vec2 world = canvas_.camera.ToWorld(V(e->position()));
        emit contextMenuRequested(e->globalPosition().toPoint(), Q(world), static_cast<int>(hit.kind),
                                  QString::fromStdString(hit.id));
    }
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    const Hit hit = canvas_.HitTest(V(e->position()));
    switch (hit.kind) {
    case HitKind::Node:
    case HitKind::Resize: emit nodeDoubleClicked(QString::fromStdString(hit.id)); break;
    case HitKind::Edge: emit edgeDoubleClicked(QString::fromStdString(hit.id)); break;
    case HitKind::None: emit emptyDoubleClicked(Q(canvas_.camera.ToWorld(V(e->position())))); break;
    default: break;
    }
}

void CanvasWidget::wheelEvent(QWheelEvent* e)
{
    emit interacted();
    if (canvas_.Wheel(V(e->position()), static_cast<float>(e->angleDelta().y()) / 120.f)) update();
}

void CanvasWidget::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Delete) {
        if (canvas_.DeleteSelection()) update();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        canvas_.ClearSelection();
        update();
        return;
    }
    QWidget::keyPressEvent(e);
}

void CanvasWidget::leaveEvent(QEvent*)
{
    if (!canvas_.Busy() && canvas_.PointerMove({-1e6f, -1e6f})) update();
}

} // namespace grapher::qt
