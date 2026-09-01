#include "gui/PreviewWidget.h"

#include <QWheelEvent>
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

} // namespace ls
