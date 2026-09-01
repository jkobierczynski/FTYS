#include "gui/AlignmentInspectorDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QFont>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace ls {

AlignmentInspectorDialog::AlignmentInspectorDialog(PipelineController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller) {
    setWindowTitle("Alignment Point Inspector");
    resize(950, 800);

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

    editModeCheck_ = new QCheckBox("Manual box editing (click empty space to add, drag a box to move, right-click a box to delete)");
    retrackButton_ = new QPushButton("Re-track with these boxes");
    retrackButton_->setEnabled(false);
    resetAutoButton_ = new QPushButton("Reset to automatic placement");
    editHintLabel_ = new QLabel;
    editHintLabel_->setWordWrap(true);
    editHintLabel_->setStyleSheet("color: #cc6600;");

    auto* editRow = new QHBoxLayout;
    editRow->addWidget(retrackButton_);
    editRow->addWidget(resetAutoButton_);
    editRow->addStretch(1);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(preview_, 1);
    layout->addLayout(scrubRow);
    layout->addWidget(infoLabel_);
    layout->addWidget(alignedCheck_);
    layout->addWidget(editModeCheck_);
    layout->addLayout(editRow);
    layout->addWidget(editHintLabel_);

    connect(slider_, &QSlider::valueChanged, posSpin_, &QSpinBox::setValue);
    connect(posSpin_, QOverload<int>::of(&QSpinBox::valueChanged), slider_, &QSlider::setValue);
    connect(posSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &AlignmentInspectorDialog::updateView);
    connect(alignedCheck_, &QCheckBox::toggled, this, &AlignmentInspectorDialog::updateView);
    connect(editModeCheck_, &QCheckBox::toggled, this, &AlignmentInspectorDialog::onEditModeToggled);
    connect(retrackButton_, &QPushButton::clicked, this, &AlignmentInspectorDialog::onRetrackClicked);
    connect(resetAutoButton_, &QPushButton::clicked, this, &AlignmentInspectorDialog::onResetAutomaticClicked);
    connect(preview_, &PreviewWidget::imagePressed, this, &AlignmentInspectorDialog::onImagePressed);
    connect(preview_, &PreviewWidget::imageMoved, this, &AlignmentInspectorDialog::onImageMoved);
    connect(preview_, &PreviewWidget::imageReleased, this, &AlignmentInspectorDialog::onImageReleased);

    setEditingControlsEnabled(false);
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

    // A fresh alignment result (whether from automatic placement or a
    // manual re-track) is the new source of truth -- drop any unstaged
    // manual edit rather than let it silently disagree with what's now
    // actually tracked.
    editedPoints_ = controller_->alignmentPoints();
    editsPending_ = false;
    dragIndex_ = -1;
    retrackButton_->setEnabled(false);
    editHintLabel_->clear();

    updateView();
}

void AlignmentInspectorDialog::updateView() {
    int pos = posSpin_->value();
    bool aligned = alignedCheck_->isChecked();
    bool editing = editModeCheck_->isChecked();

    QImage base = aligned ? controller_->alignedFramePreview(pos) : controller_->rawSelectedFramePreview(pos);
    if (base.isNull()) return;
    // Force a color format regardless of the source (mono captures decode
    // to grayscale) so the boxes are always visible.
    QImage canvas = base.convertToFormat(QImage::Format_RGB32);

    // While editing, draw the *working* (possibly not-yet-retracked) point
    // list rather than the controller's committed one, and skip per-frame
    // shifts entirely -- an edited/added box has no real tracked shift for
    // this frame yet, and editing always happens in "aligned" mode anyway
    // (see onEditModeToggled), where an untracked box just sits at its
    // plain canonical position, same as a real one would if its shift were
    // zero.
    const std::vector<AlignmentPoint>& points = editing ? editedPoints_ : controller_->alignmentPoints();
    int patchSize = controller_->alignmentPatchSize();
    if (patchSize <= 0) patchSize = 32; // no alignment has ever run yet (e.g. editing before the first Align)
    std::vector<Transform2D> shifts = editing ? std::vector<Transform2D>{} : controller_->pointShiftsForFrame(pos);

    QPainter painter(&canvas);
    QFont font = painter.font();
    font.setPointSize(11);
    font.setBold(true);
    painter.setFont(font);

    for (size_t i = 0; i < points.size(); ++i) {
        double px = points[i].x;
        double py = points[i].y;
        bool clipped = false;
        if (!editing && i < shifts.size()) {
            clipped = shifts[i].clipped;
            if (!aligned) {
                // Raw frame: the tracked feature actually sits at
                // (px + dx, py + dy) in this frame's own un-warped pixels --
                // same (out(x,y) = src(x+dx,y+dy)) convention as applyShift.
                // In "aligned" mode the per-pixel warp has already undone
                // this, so the box stays at the point's plain canonical
                // position.
                px += shifts[i].dx;
                py += shifts[i].dy;
            }
        }
        QColor color;
        if (editing) {
            color = (static_cast<int>(i) == dragIndex_) ? QColor(255, 255, 255) : QColor(0, 200, 255);
        } else {
            color = clipped ? QColor(255, 60, 60) : QColor(255, 220, 0);
        }
        QPen pen(color);
        pen.setWidth((editing && static_cast<int>(i) == dragIndex_) ? 3 : 2);
        painter.setPen(pen);
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

    if (editing) {
        infoLabel_->setText(QString("Editing %1 / %2 boxes%3 -- click empty space to add, drag a box to move, "
                                     "right-click a box to delete, then \"Re-track with these boxes\".")
                                 .arg(points.size())
                                 .arg(PipelineController::kAlignMaxPointsMax)
                                 .arg(editsPending_ ? " (not yet re-tracked)" : ""));
        return;
    }

    int origIdx = controller_->originalFrameIndex(pos);
    double avgConf = 0.0;
    int clippedCount = 0;
    for (const auto& t : shifts) { avgConf += t.confidence; if (t.clipped) ++clippedCount; }
    if (!shifts.empty()) avgConf /= shifts.size();
    infoLabel_->setText(QString("Position %1 / %2 (original frame index %3) -- %4 alignment points, "
                                 "avg confidence for this frame: %5, %6 sigma-clipped (red)")
                             .arg(pos + 1)
                             .arg(controller_->alignedFrameCount())
                             .arg(origIdx)
                             .arg(points.size())
                             .arg(avgConf, 0, 'f', 3)
                             .arg(clippedCount));
}

void AlignmentInspectorDialog::setEditingControlsEnabled(bool editing) {
    retrackButton_->setEnabled(editing && editsPending_);
    resetAutoButton_->setEnabled(editing);
    // Scrubbing and the raw/aligned toggle are disabled while editing:
    // boxes are being placed in the reference frame's canonical coordinate
    // space, and "aligned" is the only view where that space lines up
    // consistently across every frame -- see the class comment.
    slider_->setEnabled(!editing);
    posSpin_->setEnabled(!editing);
    if (editing) alignedCheck_->setChecked(true);
    alignedCheck_->setEnabled(!editing);
}

void AlignmentInspectorDialog::onEditModeToggled(bool on) {
    if (on) {
        // Start from whatever's currently tracked, not a stale copy from
        // the last refresh() -- lets the user turn editing on, off, and
        // back on again without losing an in-progress (not yet retracked)
        // edit made earlier in the same dialog session... except refresh()
        // itself (called after every real track) already resyncs
        // editedPoints_, so this only matters if editedPoints_ was never
        // touched -- harmless either way.
        dragIndex_ = -1;
        editHintLabel_->clear();
    }
    preview_->setEditMode(on);
    setEditingControlsEnabled(on);
    updateView();
}

int AlignmentInspectorDialog::hitTestPoint(QPointF pos) const {
    int patchSize = controller_->alignmentPatchSize();
    if (patchSize <= 0) patchSize = 32;
    double half = patchSize / 2.0;
    for (int i = static_cast<int>(editedPoints_.size()) - 1; i >= 0; --i) {
        if (std::abs(pos.x() - editedPoints_[static_cast<size_t>(i)].x) <= half &&
            std::abs(pos.y() - editedPoints_[static_cast<size_t>(i)].y) <= half) {
            return i;
        }
    }
    return -1;
}

void AlignmentInspectorDialog::onImagePressed(QPointF pos, Qt::MouseButton button) {
    if (!editModeCheck_->isChecked()) return;
    int hit = hitTestPoint(pos);

    if (button == Qt::RightButton) {
        if (hit >= 0) {
            editedPoints_.erase(editedPoints_.begin() + hit);
            editsPending_ = true;
            dragIndex_ = -1;
            retrackButton_->setEnabled(true);
            editHintLabel_->clear();
            updateView();
        }
        return;
    }
    if (button != Qt::LeftButton) return;

    if (hit >= 0) {
        dragIndex_ = hit; // move handled in onImageMoved as the drag continues
    } else {
        if (static_cast<int>(editedPoints_.size()) >= PipelineController::kAlignMaxPointsMax) {
            editHintLabel_->setText(QString("Already at the maximum of %1 boxes -- delete one before adding another.")
                                         .arg(PipelineController::kAlignMaxPointsMax));
            return;
        }
        editedPoints_.push_back(AlignmentPoint{pos.x(), pos.y()});
        // Let a click-and-drag in one motion also move the box that was
        // just added, rather than requiring a separate press afterward.
        dragIndex_ = static_cast<int>(editedPoints_.size()) - 1;
    }
    editsPending_ = true;
    retrackButton_->setEnabled(true);
    editHintLabel_->clear();
    updateView();
}

void AlignmentInspectorDialog::onImageMoved(QPointF pos) {
    if (!editModeCheck_->isChecked() || dragIndex_ < 0 || dragIndex_ >= static_cast<int>(editedPoints_.size())) return;
    QRectF bounds = preview_->sceneRect();
    editedPoints_[static_cast<size_t>(dragIndex_)].x = std::clamp(pos.x(), bounds.left(), bounds.right());
    editedPoints_[static_cast<size_t>(dragIndex_)].y = std::clamp(pos.y(), bounds.top(), bounds.bottom());
    editsPending_ = true;
    updateView();
}

void AlignmentInspectorDialog::onImageReleased(QPointF /*pos*/) {
    dragIndex_ = -1;
    updateView(); // drop the drag-in-progress highlight
}

void AlignmentInspectorDialog::onRetrackClicked() {
    if (editedPoints_.empty()) {
        editHintLabel_->setText("At least one alignment box is required.");
        return;
    }
    retrackButton_->setEnabled(false);
    editHintLabel_->clear();
    controller_->realignWithPoints(editedPoints_);
    // No local state change here beyond disabling the button: the
    // controller's alignDone signal (connected to MainWindow::onAlignDone,
    // which calls refresh() on an open inspector) is what brings
    // editedPoints_/editsPending_ back in sync once tracking actually
    // completes -- same path as after an automatic Align Selected Frames.
}

void AlignmentInspectorDialog::onResetAutomaticClicked() {
    // Reuses whatever patch size / max points / max deviation the
    // controller currently holds -- i.e. whatever the main window's
    // controls were last set to for "Align Selected Frames" -- rather than
    // reading MainWindow's spin boxes directly, so this dialog doesn't
    // need to know about them.
    editHintLabel_->clear();
    controller_->alignSelected();
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
