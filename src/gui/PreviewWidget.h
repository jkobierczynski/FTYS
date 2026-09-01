#pragma once

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QPointF>

namespace ls {

// Zoomable, pannable image display built on QGraphicsView. Wheel zooms
// (anchored under the cursor), left-drag pans (via ScrollHandDrag), and
// setImage() swaps in a new frame -- used for every pipeline stage's
// preview (raw frame, stack, sharpened, color-adjusted).
//
// Optionally doubles as a click/drag surface for the Alignment Point
// Inspector's manual box editing: setEditMode(true) swaps ScrollHandDrag
// for NoDrag (so a left-press starts a box interaction instead of a pan)
// and switches mouse events from Qt's normal view handling to the
// imagePressed/imageMoved/imageReleased signals below, reported in *scene*
// coordinates -- which, since setImage() sizes the scene rect to exactly
// the pixmap's bounding rect at the origin, are the same coordinates
// AlignmentPoint::x/y already use. The dialog owning this widget does all
// the actual hit-testing/add/move/delete logic; this class only forwards
// where the mouse was.
class PreviewWidget : public QGraphicsView {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void fitToWindow();
    void setEditMode(bool on);
    bool editMode() const { return editMode_; }

signals:
    void imagePressed(QPointF imagePos, Qt::MouseButton button);
    void imageMoved(QPointF imagePos);
    void imageReleased(QPointF imagePos);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QGraphicsScene* scene_;
    QGraphicsPixmapItem* pixmapItem_;
    double zoom_ = 1.0;
    bool editMode_ = false;
};

} // namespace ls
