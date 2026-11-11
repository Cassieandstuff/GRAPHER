#include <grapher/qt/CanvasWidget.h>

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QToolTip>
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

void Paint(QPainter& p, const DrawList& list, const ImageSource& images)
{
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    static const QFont kMono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);

    for (const DrawCmd& cmd : list.cmds) {
        std::visit(Overloaded{
            [&](const FillRect& r) {
                QPen pen = r.strokeWidth > 0.f ? QPen(Q(r.stroke), r.strokeWidth) : QPen(Qt::NoPen);
                if (r.dashed) pen.setStyle(Qt::DashLine);
                p.setPen(pen);
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
                f.setItalic(t.italic);
                p.setFont(f);
                p.setPen(Q(t.color));
                const QString text  = QString::fromUtf8(t.text.data(), static_cast<qsizetype>(t.text.size()));
                const QRectF  box   = Q(t.box);
                const int     align = t.align == Align::Center ? Qt::AlignHCenter : (t.align == Align::Right ? Qt::AlignRight : Qt::AlignLeft);
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
            [&](const Image& img) {
                if (!images || img.box.w < 1.f || img.box.h < 1.f) return;
                const QImage picture = images(img.key);
                if (picture.isNull()) return;
                p.save();
                if (img.radius > 0.f) {
                    QPainterPath clip;
                    clip.addRoundedRect(Q(img.box), img.radius, img.radius);
                    p.setClipPath(clip);
                }
                p.drawImage(Q(img.box), picture);
                p.restore();
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
    Paint(p, canvas_.Draw(), images_);
}

void CanvasWidget::setHostNavigation(bool on)
{
    hostNavigation_        = on;
    canvas_.panWithButtons = !on;
}

bool CanvasWidget::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        const auto*       help = static_cast<QHelpEvent*>(e);
        const std::string tip  = canvas_.TooltipAt(V(help->pos()));
        if (tip.empty()) QToolTip::hideText();
        else QToolTip::showText(help->globalPos(), QString::fromStdString(tip), this);
        return true;
    }
    return QWidget::event(e);
}

void CanvasWidget::resizeEvent(QResizeEvent*)
{
    canvas_.SetViewport({static_cast<float>(width()), static_cast<float>(height())});
}

void CanvasWidget::mousePressEvent(QMouseEvent* e)
{
    if (hostNavigation_ && e->button() == Qt::MiddleButton) return;
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
    case HitKind::Connect:
    case HitKind::Port: setCursor(Qt::CrossCursor); break;
    case HitKind::Edge: setCursor(Qt::PointingHandCursor); break;
    default: unsetCursor(); break;
    }
}

void CanvasWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (hostNavigation_ && e->button() == Qt::MiddleButton) return;
    canvas_.PointerUp(V(e->position()), ButtonOf(e->button()));
    update();
    if (const auto click = canvas_.TakeRowClick()) {
        const QString node = QString::fromStdString(click->node), row = QString::fromStdString(click->row);
        const Node*   n    = canvas_.GetGraph().FindNode(click->node);
        const Row*    r    = n ? n->FindRow(click->row) : nullptr;
        if (r && r->action == RowAction::Choice && !r->choices.empty()) {
            // A choice offers its items where it was clicked, the current one checked.
            QMenu menu(this);
            for (const std::string& item : r->choices) {
                QAction* a = menu.addAction(QString::fromStdString(item));
                a->setCheckable(true);
                a->setChecked(item == r->value);
                a->setData(QString::fromStdString(item));
            }
            const QRect line = [&] {
                const auto rr = canvas_.RowScreenRect(click->node, click->row);
                return rr ? QRectF(rr->x, rr->y, rr->w, rr->h).toRect() : QRect(e->position().toPoint(), QSize(1, 1));
            }();
            if (QAction* picked = menu.exec(mapToGlobal(line.bottomLeft())))
                if (canvas_.PickChoice(click->node, click->row, picked->data().toString().toStdString())) update();
        } else {
            emit rowClicked(node, row, e->globalPosition().toPoint());
        }
    }
    if (e->button() == Qt::RightButton && QLineF(rightDown_, e->position()).length() < 4.0) {
        const Hit  hit   = canvas_.HitTest(V(e->position()));
        const Vec2 world = canvas_.camera.ToWorld(V(e->position()));
        emit contextMenuRequested(e->globalPosition().toPoint(), Q(world), static_cast<int>(hit.kind),
                                  QString::fromStdString(hit.id));
        emit contextMenuOn(e->globalPosition().toPoint(), Q(world), static_cast<int>(hit.kind), QString::fromStdString(hit.id),
                           QString::fromStdString(hit.port), QString::fromStdString(hit.row));
    }
}

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    const Hit hit = canvas_.HitTest(V(e->position()));
    // A double-click on a member group's header opens or closes it.
    if (hit.kind == HitKind::Node)
        if (const Node* n = canvas_.GetGraph().FindNode(hit.id); n && IsMemberGroup(*n, canvas_.GetSchema())) {
            const Rect r = canvas_.ScreenRectOf(hit.id);
            if (e->position().y() <= r.y + canvas_.style.groupHeader * canvas_.camera.zoom) {
                canvas_.SetCollapsed(hit.id, !canvas_.IsCollapsed(hit.id));
                update();
                return;
            }
        }
    switch (hit.kind) {
    case HitKind::Row:
        emit rowDoubleClicked(QString::fromStdString(hit.id), QString::fromStdString(hit.row));
        emit nodeDoubleClicked(QString::fromStdString(hit.id));
        break;
    case HitKind::Node:
    case HitKind::Resize: emit nodeDoubleClicked(QString::fromStdString(hit.id)); break;
    case HitKind::Edge: emit edgeDoubleClicked(QString::fromStdString(hit.id)); break;
    case HitKind::None: emit emptyDoubleClicked(Q(canvas_.camera.ToWorld(V(e->position())))); break;
    default: break;
    }
}

void CanvasWidget::wheelEvent(QWheelEvent* e)
{
    if (hostNavigation_) {
        e->ignore();
        return;
    }
    emit interacted();
    if (canvas_.Wheel(V(e->position()), static_cast<float>(e->angleDelta().y()) / 120.f)) update();
}

void CanvasWidget::keyPressEvent(QKeyEvent* e)
{
    if (!ownKeys_) {
        QWidget::keyPressEvent(e);
        return;
    }
    if (e->key() == Qt::Key_Escape && canvas_.Busy()) {
        canvas_.CancelGesture();
        update();
        return;
    }
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
