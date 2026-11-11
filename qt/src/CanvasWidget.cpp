#include <grapher/qt/CanvasWidget.h>

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QApplication>
#include <QFrame>
#include <QHelpEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QToolTip>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
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

constexpr int kOverlayMargin = 8;

/// @brief The minimap: paints Canvas::DrawMinimap, and a press or drag on it moves the canvas's view there.
class MinimapView final : public QWidget {
public:
    MinimapView(Canvas& canvas, QWidget* parent, std::function<void()> moved)
        : QWidget(parent), canvas_(canvas), moved_(std::move(moved))
    {
        setObjectName(QStringLiteral("grapherMinimap"));
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        Paint(p, canvas_.DrawMinimap(V(QPointF(width(), height()))));
    }
    void mousePressEvent(QMouseEvent* e) override { Go(e->position()); }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & (Qt::LeftButton | Qt::MiddleButton)) Go(e->position());
    }
    void wheelEvent(QWheelEvent* e) override { e->accept(); }

private:
    void Go(QPointF at)
    {
        canvas_.CenterOnWorld(canvas_.MinimapToWorld(V(at), V(QPointF(width(), height()))));
        moved_();
    }

    Canvas&               canvas_;
    std::function<void()> moved_;
};

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
                if (l.dotted) pen.setStyle(Qt::DotLine);
                else if (l.dashed) pen.setStyle(Qt::DashLine);
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
    if (minimap_ && minimap_->isVisible()) minimap_->update();
}

void CanvasWidget::setMinimapShown(bool on)
{
    if (!minimap_) minimap_ = new MinimapView(canvas_, this, [this] { update(); });
    minimap_->setVisible(on);
    placeOverlays();
}

bool CanvasWidget::minimapShown() const { return minimap_ && minimap_->isVisible(); }

void CanvasWidget::addOverlay(QWidget* widget, Qt::Corner corner)
{
    widget->setParent(this);
    widget->show();
    overlays_.emplace_back(widget, corner);
    placeOverlays();
}

void CanvasWidget::placeOverlays()
{
    if (minimap_) {
        const QSize s(std::min(240, width() / 4), std::min(180, height() / 4));
        minimap_->setGeometry(width() - s.width() - kOverlayMargin, height() - s.height() - kOverlayMargin, s.width(), s.height());
    }
    // Each corner's widgets stack away from it; the bottom right starts above the minimap.
    int stack[4] = {kOverlayMargin, kOverlayMargin, kOverlayMargin, kOverlayMargin};
    if (minimapShown()) stack[Qt::BottomRightCorner] += minimap_->height() + kOverlayMargin;
    std::erase_if(overlays_, [](const auto& o) { return o.first.isNull(); });
    for (const auto& [w, corner] : overlays_) {
        const QSize s    = w->sizeHint().expandedTo(w->minimumSize());
        const bool  left = corner == Qt::TopLeftCorner || corner == Qt::BottomLeftCorner;
        const bool  top  = corner == Qt::TopLeftCorner || corner == Qt::TopRightCorner;
        int&        at   = stack[corner];
        const int   x    = left ? kOverlayMargin : width() - s.width() - kOverlayMargin;
        const int   y    = top ? at : height() - at - s.height();
        w->setGeometry(x, y, s.width(), s.height());
        at += s.height() + kOverlayMargin / 2;
    }
}

void CanvasWidget::openSearch(const QString& query)
{
    if (!searchBox_) {
        searchBox_ = new QFrame(this);
        searchBox_->setObjectName(QStringLiteral("grapherSearch"));
        searchBox_->setFrameShape(QFrame::StyledPanel);
        searchBox_->setAutoFillBackground(true);
        auto* layout = new QVBoxLayout(searchBox_);
        layout->setContentsMargins(6, 6, 6, 6);
        query_ = new QLineEdit(searchBox_);
        query_->setObjectName(QStringLiteral("grapherSearchQuery"));
        query_->setPlaceholderText(tr("Find"));
        query_->installEventFilter(this);
        count_ = new QLabel(searchBox_);
        hits_  = new QListWidget(searchBox_);
        hits_->setObjectName(QStringLiteral("grapherSearchHits"));
        hits_->setMaximumHeight(220);
        hits_->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(query_);
        layout->addWidget(count_);
        layout->addWidget(hits_);
        connect(query_, &QLineEdit::textChanged, this, [this] { runSearch(); });
        connect(hits_, &QListWidget::currentRowChanged, this, [this](int row) { goToHit(row); });
        connect(hits_, &QListWidget::itemActivated, this, [this] { closeSearch(true); });
    }
    beforeSearch_   = canvas_.camera;
    selectedBefore_ = canvas_.SelectedNodes();
    ringsBefore_    = canvas_.highlights.rings;
    searchBox_->setGeometry(kOverlayMargin, kOverlayMargin, std::min(360, width() - 2 * kOverlayMargin), 290);
    searchBox_->show();
    searchBox_->raise();
    query_->setFocus();
    if (!query.isEmpty() && query != query_->text()) query_->setText(query);   // runs the search
    else runSearch();
    query_->selectAll();
}

void CanvasWidget::runSearch()
{
    const std::string  q      = query_->text().toStdString();
    const SearchResult result = search_ ? search_(q) : Search(canvas_.GetGraph(), canvas_.GetSchema(), q);
    hitIds_.clear();
    const QSignalBlocker quiet(hits_);
    hits_->clear();
    for (const SearchHit& h : result.hits) {
        const Node*       n     = canvas_.GetGraph().FindNode(h.node);
        const NodeType*   t     = n ? canvas_.GetSchema().FindNode(n->type) : nullptr;
        const std::string title = n && t && !t->titleField.empty() && !n->Field(t->titleField).empty() ? n->Field(t->titleField) : h.node;
        hits_->addItem(QString::fromStdString(title + "  \u00b7  " + h.where + ": " + h.text));
        hitIds_.push_back(h.node);
    }
    count_->setText(result.total > result.hits.size() ? tr("%1 of %2").arg(result.hits.size()).arg(result.total)
                                                      : tr("%n hit(s)", nullptr, static_cast<int>(result.hits.size())));
    canvas_.highlights.rings = ringsBefore_;
    if (!hitIds_.empty()) {
        hits_->setCurrentRow(0);
        goToHit(0);
    }
    update();
}

void CanvasWidget::goToHit(int row)
{
    if (row < 0 || row >= static_cast<int>(hitIds_.size())) return;
    const std::string& id = hitIds_[static_cast<std::size_t>(row)];
    canvas_.Reveal(id);
    canvas_.SelectNodes({id});
    canvas_.CenterOn(id);
    // The other hits are ringed.
    canvas_.highlights.rings = ringsBefore_;
    for (const std::string& other : hitIds_)
        if (other != id) canvas_.highlights.rings[other] = canvas_.style.searchRing;
    update();
}

void CanvasWidget::closeSearch(bool keep)
{
    if (!searchBox_ || !searchBox_->isVisible()) return;
    const int row = hits_->currentRow();
    canvas_.highlights.rings = ringsBefore_;
    searchBox_->hide();
    setFocus();
    if (keep && row >= 0 && row < static_cast<int>(hitIds_.size())) {
        emit searchAccepted(QString::fromStdString(hitIds_[static_cast<std::size_t>(row)]));
    } else if (!keep) {
        canvas_.camera = beforeSearch_;
        canvas_.SelectNodes(selectedBefore_);
        emit searchCancelled();
    }
    update();
}

bool CanvasWidget::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == query_ && e->type() == QEvent::KeyPress) {
        const auto* k = static_cast<QKeyEvent*>(e);
        switch (k->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
            if (hits_->count() > 0)
                hits_->setCurrentRow(std::clamp(hits_->currentRow() + (k->key() == Qt::Key_Up ? -1 : 1), 0, hits_->count() - 1));
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: closeSearch(true); return true;
        case Qt::Key_Escape: closeSearch(false); return true;
        default: break;
        }
    }
    if (watched == query_ && e->type() == QEvent::FocusOut)
        QTimer::singleShot(0, this, [this] {
            // Leaving the box keeps the hit.
            if (searchBox_ && searchBox_->isVisible() && !searchBox_->isAncestorOf(QApplication::focusWidget())) closeSearch(true);
        });
    return QWidget::eventFilter(watched, e);
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
    placeOverlays();
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
    if (canvas_.PointerMove(V(e->position()), ModsOf(e->modifiers()))) update();
    if (canvas_.Busy()) return;
    switch (canvas_.HitTest(V(e->position())).kind) {
    case HitKind::Resize: setCursor(Qt::SizeFDiagCursor); break;
    case HitKind::Connect:
    case HitKind::Port: setCursor(Qt::CrossCursor); break;
    case HitKind::Handle: setCursor(Qt::OpenHandCursor); break;
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
