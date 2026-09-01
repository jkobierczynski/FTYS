#pragma once

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QImage>

namespace ls {

// Zoomable, pannable image display built on QGraphicsView. Wheel zooms
// (anchored under the cursor), left-drag pans (via ScrollHandDrag), and
// setImage() swaps in a new frame -- used for every pipeline stage's
// preview (raw frame, stack, sharpened, color-adjusted).
class PreviewWidget : public QGraphicsView {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void fitToWindow();

protected:
    void wheelEvent(QWheelEvent* event) override;

private:
    QGraphicsScene* scene_;
    QGraphicsPixmapItem* pixmapItem_;
    double zoom_ = 1.0;
};

} // namespace ls
