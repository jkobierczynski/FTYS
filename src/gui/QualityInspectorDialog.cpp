#include "gui/QualityInspectorDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <algorithm>

namespace ls {

QualityInspectorDialog::QualityInspectorDialog(PipelineController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller) {
    setWindowTitle("Quality Inspector");
    resize(900, 700);

    graph_ = new QualityGraphWidget(this);
    preview_ = new PreviewWidget(this);
    slider_ = new QSlider(Qt::Horizontal);
    rankSpin_ = new QSpinBox;
    logScaleCheck_ = new QCheckBox("Logarithmic scale");
    logScaleCheck_->setToolTip(
        "Draw bar heights on a log10 scale instead of linear. Quality scores often span a couple of orders of "
        "magnitude between the sharpest and softest frames, which on a linear scale squashes most of the "
        "distribution -- including the \"keep best %\" cutoff region -- down near the bottom.");
    infoLabel_ = new QLabel;
    infoLabel_->setWordWrap(true);

    auto* legendLabel = new QLabel(
        "Green = kept at the current \"keep best %\", gray = not kept, dashed line = the cutoff between them, "
        "yellow line = the frame shown below.");
    legendLabel->setWordWrap(true);
    legendLabel->setStyleSheet("color: #999;");

    auto* legendRow = new QHBoxLayout;
    legendRow->addWidget(legendLabel, 1);
    legendRow->addWidget(logScaleCheck_);

    auto* scrubRow = new QHBoxLayout;
    scrubRow->addWidget(new QLabel("Rank (sharpest first):"));
    scrubRow->addWidget(slider_, 1);
    scrubRow->addWidget(rankSpin_);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(graph_);
    layout->addLayout(legendRow);
    layout->addWidget(preview_, 1);
    layout->addLayout(scrubRow);
    layout->addWidget(infoLabel_);

    connect(slider_, &QSlider::valueChanged, rankSpin_, &QSpinBox::setValue);
    connect(rankSpin_, QOverload<int>::of(&QSpinBox::valueChanged), slider_, &QSlider::setValue);
    connect(rankSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &QualityInspectorDialog::updateView);
    connect(graph_, &QualityGraphWidget::rankClicked, this, &QualityInspectorDialog::onGraphRankClicked);
    connect(logScaleCheck_, &QCheckBox::toggled, this, &QualityInspectorDialog::onLogScaleToggled);
}

void QualityInspectorDialog::refresh() {
    // Same descending-by-score sort selectTopPercent() uses internally
    // (see FrameSelector.cpp), so rank `i < keptCount` here always agrees
    // with which original frames selectPercent() actually kept.
    sorted_ = controller_->qualityScores();
    std::stable_sort(sorted_.begin(), sorted_.end(),
                      [](const FrameQuality& a, const FrameQuality& b) { return a.score > b.score; });

    int kept = controller_->selectedCount();
    graph_->setData(sorted_, kept);

    int maxRank = std::max(0, static_cast<int>(sorted_.size()) - 1);
    slider_->blockSignals(true);
    rankSpin_->blockSignals(true);
    slider_->setRange(0, maxRank);
    rankSpin_->setRange(0, maxRank);
    slider_->setValue(0);
    rankSpin_->setValue(0);
    slider_->blockSignals(false);
    rankSpin_->blockSignals(false);
    fitDone_ = false;

    updateView();
}

void QualityInspectorDialog::refreshSelection() {
    if (sorted_.empty()) return; // nothing scored yet -- refresh() handles that case
    graph_->setData(sorted_, controller_->selectedCount());
    updateView(); // redraws the cursor and updates the kept/not-kept text at the current rank
}

void QualityInspectorDialog::onGraphRankClicked(int rank) {
    rankSpin_->setValue(rank); // triggers updateView() via the existing connection
}

void QualityInspectorDialog::onLogScaleToggled(bool checked) {
    graph_->setLogScale(checked);
}

void QualityInspectorDialog::updateView() {
    int rank = rankSpin_->value();
    graph_->setCursorRank(rank);

    if (sorted_.empty() || rank < 0 || rank >= static_cast<int>(sorted_.size())) {
        infoLabel_->setText("Run \"Assess Quality\" first.");
        return;
    }

    const FrameQuality& fq = sorted_[static_cast<size_t>(rank)];
    QImage img = controller_->previewFrame(static_cast<int>(fq.index));
    if (!img.isNull()) preview_->setImage(img);

    if (!fitDone_ && isVisible()) {
        // Same fitInView-before-layout-is-ready fix as
        // AlignmentInspectorDialog: safe to fit inline here only once the
        // dialog is already realized (a refresh() while it's still open);
        // the very-first-open case is handled by showEvent() instead.
        preview_->fitToWindow();
        fitDone_ = true;
    }

    int kept = controller_->selectedCount();
    bool isKept = rank < kept;
    infoLabel_->setText(QString("Rank %1 / %2 (sharpest to softest) -- original frame index %3 -- "
                                 "quality score %4 -- %5")
                             .arg(rank + 1)
                             .arg(sorted_.size())
                             .arg(fq.index)
                             .arg(fq.score, 0, 'g', 4)
                             .arg(isKept ? "kept at current \"keep best %\"" : "not kept at current \"keep best %\""));
}

void QualityInspectorDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!fitDone_) {
        // MainWindow calls refresh() (which draws the first frame) before
        // show(), so updateView() above sees isVisible() == false on first
        // open and skips fitting -- defer one event-loop turn so the
        // viewport has its real, laid-out size by the time fitToWindow()
        // runs. See AlignmentInspectorDialog::showEvent for the full
        // history of why this needs deferring at all.
        QTimer::singleShot(0, this, [this]() {
            preview_->fitToWindow();
            fitDone_ = true;
        });
    }
}

} // namespace ls
