#include "gui/PreviewWidget.h"

#include <QWheelEvent>
#include <QMouseEvent>
#include <QPixmap>

namespace ls {

PreviewWidget::PreviewWidget(QWidget* parent) : QGraphicsView(parent) {
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    pixmapItem_ = scene_->addPixmap(QPixmap());
    setDragMode(QGraphicsView::ScrollHandDrag);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setBackgroundBrush(QBrush(QColor(30, 30, 30)));
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
}

void PreviewWidget::setImage(const QImage& image) {
    pixmapItem_->setPixmap(QPixmap::fromImage(image));
    scene_->setSceneRect(pixmapItem_->boundingRect());
}

void PreviewWidget::fitToWindow() {
    if (pixmapItem_->pixmap().isNull()) return;
    fitInView(pixmapItem_, Qt::KeepAspectRatio);
    zoom_ = transform().m11();
}

void PreviewWidget::wheelEvent(QWheelEvent* event) {
    double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    zoom_ *= factor;
    scale(factor, factor);
}

void PreviewWidget::setEditMode(bool on) {
    editMode_ = on;
    // NoDrag while editing: ScrollHandDrag would otherwise consume the
    // left button for panning before a press ever reaches mousePressEvent,
    // and would fight with dragging a box around.
    setDragMode(on ? QGraphicsView::NoDrag : QGraphicsView::ScrollHandDrag);
}

void PreviewWidget::mousePressEvent(QMouseEvent* event) {
    if (editMode_) {
        emit imagePressed(mapToScene(event->pos()), event->button());
        event->accept();
        return;
    }
    QGraphicsView::mousePressEvent(event);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* event) {
    if (editMode_) {
        if (event->buttons() & Qt::LeftButton) emit imageMoved(mapToScene(event->pos()));
        event->accept();
        return;
    }
    QGraphicsView::mouseMoveEvent(event);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (editMode_) {
        emit imageReleased(mapToScene(event->pos()));
        event->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
}

} // namespace ls
