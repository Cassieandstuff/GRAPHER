#pragma once
/// @file
/// @brief GRAPHER's Qt backend: a widget that feeds a grapher::Canvas Qt's input and paints its DrawList. It owns no
///        graph state; the host wires the canvas's callbacks.

#include <grapher/Canvas.h>

#include <QPointF>
#include <QWidget>

class QPainter;

namespace grapher::qt {

/// @brief Paint a draw list with QPainter (the widget's paint, and image export).
void Paint(QPainter& painter, const DrawList& list);

class CanvasWidget : public QWidget {
    Q_OBJECT
public:
    explicit CanvasWidget(Canvas& canvas, QWidget* parent = nullptr);

    [[nodiscard]] Canvas& canvas() { return canvas_; }

signals:
    /// @brief Any press or wheel, before the canvas sees it (a host closes an inline editor on it).
    void interacted();
    void nodeDoubleClicked(const QString& id);
    void edgeDoubleClicked(const QString& id);
    void emptyDoubleClicked(QPointF world);
    /// @brief A right click that did not pan. `kind`/`id` are what was under the pointer (see grapher::HitKind).
    void contextMenuRequested(QPoint globalPos, QPointF world, int kind, const QString& id);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    Canvas& canvas_;
    QPointF rightDown_;
};

} // namespace grapher::qt
