#pragma once
/// @file
/// @brief GRAPHER's Qt backend: a widget that feeds a grapher::Canvas Qt's input and paints its DrawList. It owns no
///        graph state; the host wires the canvas's callbacks.

#include <grapher/Canvas.h>

#include <QImage>
#include <QPointF>
#include <QWidget>

#include <functional>
#include <string>

class QPainter;

namespace grapher::qt {

/// @brief Resolves an Image draw command's key to a picture; a null QImage draws nothing.
using ImageSource = std::function<QImage(const std::string& key)>;

/// @brief Paint a draw list with QPainter (the widget's paint, and image export).
void Paint(QPainter& painter, const DrawList& list, const ImageSource& images = {});

class CanvasWidget : public QWidget {
    Q_OBJECT
public:
    explicit CanvasWidget(Canvas& canvas, QWidget* parent = nullptr);

    [[nodiscard]] Canvas& canvas() { return canvas_; }

    /// @brief Where node pictures come from (Node::image keys).
    void setImageSource(ImageSource images) { images_ = std::move(images); }
    /// @brief The host navigates the view (its own middle-button and wheel handling, through Canvas::PanBy and
    ///        ZoomAbout): the widget then leaves the middle button and the wheel alone, and the right button only
    ///        asks for a context menu.
    void setHostNavigation(bool on);
    /// @brief Whether the widget handles Delete and Esc itself (the default). A host whose keys are commands turns this
    ///        off; key presses then go to the parent.
    void setOwnKeys(bool on) { ownKeys_ = on; }

signals:
    /// @brief Any press or wheel, before the canvas sees it (a host closes an inline editor on it).
    void interacted();
    void nodeDoubleClicked(const QString& id);
    void edgeDoubleClicked(const QString& id);
    void emptyDoubleClicked(QPointF world);
    /// @brief A right click that did not pan. `kind`/`id` are what was under the pointer (see grapher::HitKind).
    void contextMenuRequested(QPoint globalPos, QPointF world, int kind, const QString& id);
    /// @brief The same right click, with the port or row under the pointer too.
    void contextMenuOn(QPoint globalPos, QPointF world, int kind, const QString& id, const QString& port, const QString& row);
    /// @brief A row with an action other than a Choice was clicked (a Choice opens its menu and sets its field itself).
    void rowClicked(const QString& node, const QString& row, QPoint globalPos);
    void rowDoubleClicked(const QString& node, const QString& row);

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
    bool event(QEvent*) override;

private:
    Canvas&     canvas_;
    QPointF     rightDown_;
    ImageSource images_;
    bool        hostNavigation_ = false;
    bool        ownKeys_        = true;
};

} // namespace grapher::qt
