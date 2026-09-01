#include "gui/AlignmentInspectorDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QFont>
#include <QTimer>
#include <algorithm>

namespace ls {

AlignmentInspectorDialog::AlignmentInspectorDialog(PipelineController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller) {
    setWindowTitle("Alignment Point Inspector");
    resize(950, 750);

    preview_ = new PreviewWidget(this);
    slider_ = new QSlider(Qt::Horizontal);
    posSpin_ = new QSpinBox;
    infoLabel_ = new QLabel;
    infoLabel_->setWordWrap(true);
    alignedCheck_ = new QCheckBox("Show aligned (per-pixel warped, boxes fixed) rather than raw (boxes follow the tracked feature)");
    alignedCheck_->setChecked(true);

    auto* scrubRow = new QHBoxLayout;
    scrubRow->addWidget(new QLabel("Frame:"));
    scrubRow->addWidget(slider_, 1);
    scrubRow->addWidget(posSpin_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(preview_, 1);
    layout->addLayout(scrubRow);
    layout->addWidget(infoLabel_);
    layout->addWidget(alignedCheck_);

    connect(slider_, &QSlider::valueChanged, posSpin_, &QSpinBox::setValue);
    connect(posSpin_, QOverload<int>::of(&QSpinBox::valueChanged), slider_, &QSlider::setValue);
    connect(posSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &AlignmentInspectorDialog::updateView);
    connect(alignedCheck_, &QCheckBox::toggled, this, &AlignmentInspectorDialog::updateView);
}

void AlignmentInspectorDialog::refresh() {
    int n = controller_->alignedFrameCount();
    int maxPos = std::max(0, n - 1);
    slider_->blockSignals(true);
    posSpin_->blockSignals(true);
    slider_->setRange(0, maxPos);
    posSpin_->setRange(0, maxPos);
    slider_->setValue(0);
    posSpin_->setValue(0);
    slider_->blockSignals(false);
    posSpin_->blockSignals(false);
    fitDone_ = false;
    updateView();
}

void AlignmentInspectorDialog::updateView() {
    int pos = posSpin_->value();
    bool aligned = alignedCheck_->isChecked();

    QImage base = aligned ? controller_->alignedFramePreview(pos) : controller_->rawSelectedFramePreview(pos);
    if (base.isNull()) return;
    // Force a color format regardless of the source (mono captures decode
    // to grayscale) so the yellow boxes are always visible.
    QImage canvas = base.convertToFormat(QImage::Format_RGB32);

    const std::vector<AlignmentPoint>& points = controller_->alignmentPoints();
    int patchSize = controller_->alignmentPatchSize();
    std::vector<Transform2D> shifts = controller_->pointShiftsForFrame(pos);

    QPainter painter(&canvas);
    QPen pen(QColor(255, 220, 0));
    pen.setWidth(2);
    painter.setPen(pen);
    QFont font = painter.font();
    font.setPointSize(11);
    font.setBold(true);
    painter.setFont(font);

    for (size_t i = 0; i < points.size(); ++i) {
        double px = points[i].x;
        double py = points[i].y;
        if (!aligned && i < shifts.size()) {
            // Raw frame: the tracked feature actually sits at
            // (px + dx, py + dy) in this frame's own un-warped pixels --
            // same (out(x,y) = src(x+dx,y+dy)) convention as applyShift.
            // In "aligned" mode the per-pixel warp has already undone
            // this, so the box stays at the point's plain canonical
            // position.
            px += shifts[i].dx;
            py += shifts[i].dy;
        }
        QRectF box(px - patchSize / 2.0, py - patchSize / 2.0, patchSize, patchSize);
        painter.drawRect(box);
        painter.drawText(QPointF(box.left() + 2, box.top() - 4), QString::number(i + 1));
    }
    painter.end();

    preview_->setImage(canvas);
    if (!fitDone_ && isVisible()) {
        // Safe to fit immediately: the dialog is already realized (this is
        // a re-align while it's still open), so the viewport already has
        // its real, laid-out size. The very-first-open case is handled by
        // showEvent() instead -- see its comment for why fitting there
        // can't just happen inline here.
        preview_->fitToWindow();
        fitDone_ = true;
    }

    int origIdx = controller_->originalFrameIndex(pos);
    double avgConf = 0.0;
    for (const auto& t : shifts) avgConf += t.confidence;
    if (!shifts.empty()) avgConf /= shifts.size();
    infoLabel_->setText(QString("Position %1 / %2 (original frame index %3) -- %4 alignment points, "
                                 "avg confidence for this frame: %5")
                             .arg(pos + 1)
                             .arg(controller_->alignedFrameCount())
                             .arg(origIdx)
                             .arg(points.size())
                             .arg(avgConf, 0, 'f', 3));
}

void AlignmentInspectorDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!fitDone_) {
        // MainWindow::onInspectAlignment() calls refresh() (which draws the
        // first frame) *before* show(), so on first open updateView() above
        // sees isVisible() == false and skips fitting -- at that point the
        // dialog's layout hasn't been activated yet and the preview
        // viewport still reports a stale/default size, which is exactly
        // why fitInView used to compute a scale that was zoomed way out.
        // Even here, inside showEvent, Qt hasn't necessarily finished
        // applying the pending layout to the viewport yet (layout activation
        // is posted, not synchronous), so defer one more event-loop turn --
        // by the time this runs, the viewport's real size is in place and
        // fitToWindow() computes the correct zoom.
        QTimer::singleShot(0, this, [this]() {
            preview_->fitToWindow();
            fitDone_ = true;
        });
    }
}

} // namespace ls
