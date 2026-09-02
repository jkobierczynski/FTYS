#include "gui/MainWindow.h"

#include <QIcon>
#include <QMenuBar>
#include <QSplitter>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QStatusBar>
#include <QLabel>

namespace ls {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("FTYS — Focus Through Your Seeing");
    setWindowIcon(QIcon(":/icons/ftys_logo.png"));
    resize(1280, 800);

    controller_ = new PipelineController(this);
    preview_ = new PreviewWidget(this);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidget(buildControlsPanel());
    scrollArea->setWidgetResizable(true);
    // +20% over the original 340/420 -- the user found the panel cramped
    // once the alignment box-size/point-count/deviation controls were added.
    scrollArea->setMinimumWidth(408);
    scrollArea->setMaximumWidth(504);
    splitter->addWidget(scrollArea);
    splitter->addWidget(preview_);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    progressBar_ = new QProgressBar(this);
    progressBar_->setMaximumWidth(200);
    statusBar()->addPermanentWidget(progressBar_);
    statusBar()->showMessage("Open a SER, AVI, or FITS sequence to begin.");

    auto* openAction = menuBar()->addMenu("&File")->addAction("&Open Sequence...");
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenSequence);

    connect(controller_, &PipelineController::sequenceOpened, this, &MainWindow::onSequenceOpened);
    connect(controller_, &PipelineController::qualityProgress, this, &MainWindow::onQualityProgress);
    connect(controller_, &PipelineController::qualityDone, this, &MainWindow::onQualityDone);
    // Aligning and stacking drive the same progress bar/slot as quality
    // assessment -- same 0-100 percent convention, just a different signal.
    connect(controller_, &PipelineController::alignProgress, this, &MainWindow::onQualityProgress);
    connect(controller_, &PipelineController::stackProgress, this, &MainWindow::onQualityProgress);
    connect(controller_, &PipelineController::selectionChanged, this, &MainWindow::onSelectionChanged);
    connect(controller_, &PipelineController::alignDone, this, &MainWindow::onAlignDone);
    connect(controller_, &PipelineController::stackDone, this, &MainWindow::onStackDone);
    connect(controller_, &PipelineController::sharpenDone, this, &MainWindow::onSharpenDone);
    connect(controller_, &PipelineController::colorDone, this, &MainWindow::onColorDone);
    connect(controller_, &PipelineController::chromaticAberrationDone, this, &MainWindow::onChromaticAberrationDone);
    connect(controller_, &PipelineController::errorOccurred, this, &MainWindow::onError);

    setEnabledStageButtons();
}

QWidget* MainWindow::buildControlsPanel() {
    auto* panel = new QWidget;
    auto* layout = new QVBoxLayout(panel);

    // --- Sequence -----------------------------------------------------
    auto* seqGroup = new QGroupBox("Sequence");
    auto* seqLayout = new QVBoxLayout(seqGroup);
    sequenceInfoLabel_ = new QLabel("No sequence loaded.");
    sequenceInfoLabel_->setWordWrap(true);
    seqLayout->addWidget(sequenceInfoLabel_);
    layout->addWidget(seqGroup);

    // --- Frame selection ------------------------------------------------
    auto* selGroup = new QGroupBox("Frame Selection");
    auto* selLayout = new QVBoxLayout(selGroup);
    assessButton_ = new QPushButton("Assess Quality");
    connect(assessButton_, &QPushButton::clicked, this, &MainWindow::onAssessQuality);
    selLayout->addWidget(assessButton_);

    auto* percentRow = new QWidget;
    auto* percentRowLayout = new QFormLayout(percentRow);
    percentSlider_ = new QSlider(Qt::Horizontal);
    percentSlider_->setRange(1, 100);
    percentSlider_->setValue(50);
    connect(percentSlider_, &QSlider::valueChanged, this, &MainWindow::onPercentChanged);
    percentValueLabel_ = new QLabel("50%");
    auto* percentRowWidget = new QWidget;
    auto* percentRowHLayout = new QVBoxLayout(percentRowWidget);
    percentRowHLayout->addWidget(percentSlider_);
    percentRowHLayout->addWidget(percentValueLabel_);
    percentRowLayout->addRow("Keep best:", percentRowWidget);
    selLayout->addWidget(percentRow);

    selectionLabel_ = new QLabel("");
    selLayout->addWidget(selectionLabel_);

    inspectQualityButton_ = new QPushButton("View Quality Graph...");
    inspectQualityButton_->setEnabled(false);
    inspectQualityButton_->setToolTip(
        "Shows every scored frame's sharpness as a sorted bar graph (sharpest "
        "to softest), colored by whether it's kept at the current \"keep best "
        "%\", with a slider (or click/drag directly on the graph) to scrub "
        "through and preview any frame alongside its score.");
    connect(inspectQualityButton_, &QPushButton::clicked, this, &MainWindow::onInspectQuality);
    selLayout->addWidget(inspectQualityButton_);

    layout->addWidget(selGroup);

    // --- Alignment -------------------------------------------------------
    auto* alignGroup = new QGroupBox("Alignment");
    auto* alignLayout = new QVBoxLayout(alignGroup);
    auto* patchSizeRow = new QWidget;
    auto* patchSizeRowLayout = new QFormLayout(patchSizeRow);
    alignPatchSizeSpin_ = new QSpinBox;
    alignPatchSizeSpin_->setRange(24, 256);
    alignPatchSizeSpin_->setSingleStep(8);
    alignPatchSizeSpin_->setValue(32);
    alignPatchSizeSpin_->setToolTip(
        "Size of each tracking patch (\"box\"), in pixels. Now that each point's "
        "target patch is first recentered on the frame's own detected disk "
        "position (so it only has to find the leftover local motion, not the "
        "whole disk's bulk translation), smaller patches tracked tighter and "
        "more consistently on real test footage -- 32 is the validated default. "
        "A softer or heavily compressed capture with very little local contrast "
        "may still do better with a larger patch, at the cost of fewer "
        "independent points fitting across the disk.");
    patchSizeRowLayout->addRow("Box size (px):", alignPatchSizeSpin_);
    alignLayout->addWidget(patchSizeRow);

    auto* maxPointsRow = new QWidget;
    auto* maxPointsRowLayout = new QFormLayout(maxPointsRow);
    alignMaxPointsSpin_ = new QSpinBox;
    alignMaxPointsSpin_->setRange(PipelineController::kAlignMaxPointsMin, PipelineController::kAlignMaxPointsMax);
    alignMaxPointsSpin_->setSingleStep(1);
    alignMaxPointsSpin_->setValue(12);
    alignMaxPointsSpin_->setToolTip(
        QString("How many tracking boxes to automatically place on the disk's best-"
                "contrast features (belt edges, festoons, the limb). More points can "
                "resolve local seeing distortion in finer detail on a larger or "
                "higher-contrast disk; a small or soft one may not have enough "
                "well-spaced high-contrast spots to fill a high count, and will fall "
                "back to fewer than requested. Capped at %1, same limit as the manual "
                "box editor in Inspect Alignment Points...")
            .arg(PipelineController::kAlignMaxPointsMax));
    maxPointsRowLayout->addRow("Number of boxes:", alignMaxPointsSpin_);
    alignLayout->addWidget(maxPointsRow);

    auto* maxDeviationRow = new QWidget;
    auto* maxDeviationRowLayout = new QFormLayout(maxDeviationRow);
    alignMaxDeviationSpin_ = new QDoubleSpinBox;
    alignMaxDeviationSpin_->setRange(2.0, 50.0);
    alignMaxDeviationSpin_->setSingleStep(1.0);
    alignMaxDeviationSpin_->setValue(12.0);
    alignMaxDeviationSpin_->setToolTip(
        "Outlier rejection (\"sigma clip\") for individual tracking boxes: on "
        "each frame, a box whose measured shift deviates from that frame's "
        "consensus shift by more than this many pixels is treated as a bad "
        "lock and replaced by the consensus, instead of injecting a spurious "
        "warp into its neighborhood. Lower this to reject more aggressively; "
        "raise it if genuine local seeing distortion on this capture is "
        "larger than that and is being clipped away.");
    maxDeviationRowLayout->addRow("Max deviation (px):", alignMaxDeviationSpin_);
    alignLayout->addWidget(maxDeviationRow);

    alignButton_ = new QPushButton("Align Selected Frames");
    connect(alignButton_, &QPushButton::clicked, this, &MainWindow::onAlign);
    alignLayout->addWidget(alignButton_);
    alignLabel_ = new QLabel("");
    alignLayout->addWidget(alignLabel_);
    inspectAlignmentButton_ = new QPushButton("Inspect Alignment Points...");
    inspectAlignmentButton_->setEnabled(false);
    connect(inspectAlignmentButton_, &QPushButton::clicked, this, &MainWindow::onInspectAlignment);
    alignLayout->addWidget(inspectAlignmentButton_);
    layout->addWidget(alignGroup);

    // --- Stacking ----------------------------------------------------------
    auto* stackGroup = new QGroupBox("Stacking");
    auto* stackLayout = new QVBoxLayout(stackGroup);
    stackModeCombo_ = new QComboBox;
    stackModeCombo_->addItems({"Mean", "Sigma-Clip", "Drizzle"});
    stackLayout->addWidget(stackModeCombo_);

    stackParamsStack_ = new QStackedWidget;
    stackParamsStack_->addWidget(new QWidget); // Mean: no params

    auto* sigmaPage = new QWidget;
    auto* sigmaForm = new QFormLayout(sigmaPage);
    sigmaLowSpin_ = new QDoubleSpinBox; sigmaLowSpin_->setRange(0.5, 10.0); sigmaLowSpin_->setValue(3.0);
    sigmaHighSpin_ = new QDoubleSpinBox; sigmaHighSpin_->setRange(0.5, 10.0); sigmaHighSpin_->setValue(3.0);
    sigmaForm->addRow("Sigma low:", sigmaLowSpin_);
    sigmaForm->addRow("Sigma high:", sigmaHighSpin_);
    stackParamsStack_->addWidget(sigmaPage);

    auto* drizzlePage = new QWidget;
    auto* drizzleForm = new QFormLayout(drizzlePage);
    drizzleScaleSpin_ = new QDoubleSpinBox; drizzleScaleSpin_->setRange(1.0, 4.0); drizzleScaleSpin_->setSingleStep(0.1); drizzleScaleSpin_->setValue(2.0);
    drizzleDropSpin_ = new QDoubleSpinBox; drizzleDropSpin_->setRange(0.1, 1.0); drizzleDropSpin_->setSingleStep(0.05); drizzleDropSpin_->setValue(0.65);
    drizzleForm->addRow("Scale:", drizzleScaleSpin_);
    drizzleForm->addRow("Drop fraction:", drizzleDropSpin_);
    stackParamsStack_->addWidget(drizzlePage);

    connect(stackModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), stackParamsStack_,
            &QStackedWidget::setCurrentIndex);
    stackLayout->addWidget(stackParamsStack_);

    stackButton_ = new QPushButton("Stack");
    connect(stackButton_, &QPushButton::clicked, this, &MainWindow::onStack);
    stackLayout->addWidget(stackButton_);
    layout->addWidget(stackGroup);

    // --- Sharpening -----------------------------------------------------
    auto* sharpGroup = new QGroupBox("Sharpening");
    auto* sharpLayout = new QVBoxLayout(sharpGroup);
    sharpenModeCombo_ = new QComboBox;
    sharpenModeCombo_->addItems({"None", "Wavelet", "Richardson-Lucy"});
    sharpenModeCombo_->setCurrentIndex(1);
    sharpLayout->addWidget(sharpenModeCombo_);

    sharpenParamsStack_ = new QStackedWidget;
    sharpenParamsStack_->addWidget(new QWidget); // None

    auto* waveletPage = new QWidget;
    auto* waveletForm = new QFormLayout(waveletPage);
    const double defaultGains[4] = {1.3, 1.2, 1.0, 1.0};
    for (int i = 0; i < 4; ++i) {
        waveletGainSpins_[i] = new QDoubleSpinBox;
        // Upper bound is generous on purpose: planetary wavelet sharpening
        // is routinely pushed well past "gain=3" for the finer scales, and
        // the previous 3.0 cap was cutting that off artificially.
        waveletGainSpins_[i]->setRange(0.0, 20.0);
        waveletGainSpins_[i]->setSingleStep(0.1);
        waveletGainSpins_[i]->setValue(defaultGains[i]);
        waveletForm->addRow(QString("Scale %1 gain:").arg(i + 1), waveletGainSpins_[i]);
    }
    sharpenParamsStack_->addWidget(waveletPage);

    auto* rlPage = new QWidget;
    auto* rlForm = new QFormLayout(rlPage);
    rlIterationsSpin_ = new QSpinBox; rlIterationsSpin_->setRange(1, 100); rlIterationsSpin_->setValue(15);
    rlSigmaSpin_ = new QDoubleSpinBox; rlSigmaSpin_->setRange(0.3, 10.0); rlSigmaSpin_->setSingleStep(0.1); rlSigmaSpin_->setValue(1.6);
    rlForm->addRow("Iterations:", rlIterationsSpin_);
    rlForm->addRow("PSF sigma (px):", rlSigmaSpin_);
    sharpenParamsStack_->addWidget(rlPage);

    connect(sharpenModeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), sharpenParamsStack_,
            &QStackedWidget::setCurrentIndex);
    sharpenParamsStack_->setCurrentIndex(1);
    sharpLayout->addWidget(sharpenParamsStack_);

    sharpenButton_ = new QPushButton("Apply Sharpening");
    connect(sharpenButton_, &QPushButton::clicked, this, &MainWindow::onSharpen);
    sharpLayout->addWidget(sharpenButton_);
    layout->addWidget(sharpGroup);

    // --- Color -----------------------------------------------------------
    auto* colorGroup = new QGroupBox("Histogram / Color");
    auto* colorLayout = new QVBoxLayout(colorGroup);
    adjustColorButton_ = new QPushButton("Adjust Color...");
    adjustColorButton_->setEnabled(false);
    adjustColorButton_->setToolTip(
        "Opens a full-size histogram/curve editor -- add or remove curve points, and adjust levels, brightness, "
        "color balance, hue, and saturation, with the preview updating live as you go.");
    connect(adjustColorButton_, &QPushButton::clicked, this, &MainWindow::onAdjustColor);
    colorLayout->addWidget(adjustColorButton_);
    layout->addWidget(colorGroup);

    // --- Chromatic aberration ---------------------------------------------
    auto* caGroup = new QGroupBox("Chromatic Aberration");
    auto* caLayout = new QVBoxLayout(caGroup);
    auto* caLabel = new QLabel(
        "Aligns the red and blue channels onto green to remove color "
        "fringing. Offsets are in pixels, applied after color adjustments.");
    caLabel->setWordWrap(true);
    caLayout->addWidget(caLabel);

    auto* caForm = new QFormLayout;
    caRedDxSpin_ = new QDoubleSpinBox; caRedDxSpin_->setRange(-20.0, 20.0); caRedDxSpin_->setSingleStep(0.1); caRedDxSpin_->setDecimals(2);
    caRedDySpin_ = new QDoubleSpinBox; caRedDySpin_->setRange(-20.0, 20.0); caRedDySpin_->setSingleStep(0.1); caRedDySpin_->setDecimals(2);
    caBlueDxSpin_ = new QDoubleSpinBox; caBlueDxSpin_->setRange(-20.0, 20.0); caBlueDxSpin_->setSingleStep(0.1); caBlueDxSpin_->setDecimals(2);
    caBlueDySpin_ = new QDoubleSpinBox; caBlueDySpin_->setRange(-20.0, 20.0); caBlueDySpin_->setSingleStep(0.1); caBlueDySpin_->setDecimals(2);
    caForm->addRow("Red shift X:", caRedDxSpin_);
    caForm->addRow("Red shift Y:", caRedDySpin_);
    caForm->addRow("Blue shift X:", caBlueDxSpin_);
    caForm->addRow("Blue shift Y:", caBlueDySpin_);
    caLayout->addLayout(caForm);

    auto* caButtonRow = new QHBoxLayout;
    caDetectButton_ = new QPushButton("Auto-detect");
    caDetectButton_->setToolTip(
        "Cross-correlates the red and blue channels against green (same FFT "
        "phase correlation used for inter-frame alignment) and fills in the "
        "offsets above as a starting point -- fine-tune by eye afterward.");
    connect(caDetectButton_, &QPushButton::clicked, this, &MainWindow::onDetectCA);
    caApplyButton_ = new QPushButton("Apply CA Correction");
    connect(caApplyButton_, &QPushButton::clicked, this, &MainWindow::onApplyCA);
    caButtonRow->addWidget(caDetectButton_);
    caButtonRow->addWidget(caApplyButton_);
    caLayout->addLayout(caButtonRow);
    layout->addWidget(caGroup);

    // --- Export ------------------------------------------------------------
    auto* exportGroup = new QGroupBox("Export");
    auto* exportLayout = new QVBoxLayout(exportGroup);

    auto* formatRow = new QWidget;
    auto* formatRowLayout = new QFormLayout(formatRow);
    exportFormatCombo_ = new QComboBox;
    exportFormatCombo_->addItems({"PNG", "TIFF", "FITS"});
    formatRowLayout->addRow("Format:", exportFormatCombo_);
    exportLayout->addWidget(formatRow);

    auto* bitDepthRow = new QWidget;
    auto* bitDepthRowLayout = new QFormLayout(bitDepthRow);
    exportBitDepthCombo_ = new QComboBox;
    exportBitDepthCombo_->addItems({"8-bit", "16-bit"});
    exportBitDepthCombo_->setEnabled(false); // PNG is the initial format, and is always 8-bit
    exportBitDepthCombo_->setToolTip(
        "PNG is always 8-bit. TIFF and FITS can be written at full 16-bit "
        "precision instead of the internal float pipeline being rounded "
        "down to 8-bit on export.");
    bitDepthRowLayout->addRow("Bit depth:", exportBitDepthCombo_);
    exportLayout->addWidget(bitDepthRow);

    connect(exportFormatCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onExportFormatChanged);

    exportButton_ = new QPushButton("Export...");
    connect(exportButton_, &QPushButton::clicked, this, &MainWindow::onExport);
    exportLayout->addWidget(exportButton_);
    layout->addWidget(exportGroup);

    layout->addStretch(1);
    return panel;
}

void MainWindow::setEnabledStageButtons() {
    assessButton_->setEnabled(false);
    inspectQualityButton_->setEnabled(false);
    alignButton_->setEnabled(false);
    inspectAlignmentButton_->setEnabled(false);
    stackButton_->setEnabled(false);
    sharpenButton_->setEnabled(false);
    adjustColorButton_->setEnabled(false);
    caDetectButton_->setEnabled(false);
    caApplyButton_->setEnabled(false);
    exportButton_->setEnabled(false);
}

void MainWindow::onOpenSequence() {
    QString path = QFileDialog::getOpenFileName(this, "Open Capture Sequence", QString(),
                                                 "Capture files (*.ser *.avi *.fits *.fit *.fts);;All files (*)");
    if (path.isEmpty()) return;
    controller_->openSequence(path);
}

void MainWindow::onSequenceOpened(int width, int height, int frameCount, QString formatDescription) {
    sequenceInfoLabel_->setText(QString("%1 x %2, %3 frames\n%4").arg(width).arg(height).arg(frameCount).arg(formatDescription));
    setEnabledStageButtons();
    colorApplied_ = false; // new sequence -- any prior color-stretch state is stale
    caApplied_ = false;
    assessButton_->setEnabled(true);
    QImage first = controller_->previewFrame(0);
    if (!first.isNull()) {
        preview_->setImage(first);
        preview_->fitToWindow();
    }
    statusBar()->showMessage(QString("Loaded sequence: %1 frames").arg(frameCount), 5000);
}

void MainWindow::onAssessQuality() {
    assessButton_->setEnabled(false);
    progressBar_->setValue(0);
    statusBar()->showMessage("Assessing frame quality...");
    controller_->computeQuality();
}

void MainWindow::onQualityProgress(int percent) {
    progressBar_->setValue(percent);
}

void MainWindow::onQualityDone(int totalFrames) {
    qualityReady_ = true;
    assessButton_->setEnabled(true);
    inspectQualityButton_->setEnabled(true);
    statusBar()->showMessage(QString("Quality assessed for %1 frames").arg(totalFrames), 5000);
    controller_->selectPercent(percentSlider_->value());
    alignButton_->setEnabled(true);
    // A freshly (re-)computed quality pass invalidates any graph an already-
    // open inspector is showing -- refresh it in place rather than leaving
    // it stale, same convention as onAlignDone() does for the alignment
    // inspector.
    if (qualityInspector_) qualityInspector_->refresh();
}

void MainWindow::onPercentChanged(int value) {
    percentValueLabel_->setText(QString("%1%").arg(value));
    if (qualityReady_) controller_->selectPercent(value);
}

void MainWindow::onSelectionChanged(int keptCount, int totalScored, double estimatedMegabytes) {
    QString memNote = estimatedMegabytes > 4000.0
                           ? QString(" -- ~%1 MB, consider a lower percentage").arg(estimatedMegabytes, 0, 'f', 0)
                           : QString(" (~%1 MB)").arg(estimatedMegabytes, 0, 'f', 0);
    selectionLabel_->setText(QString("Keeping %1 of %2 frames%3").arg(keptCount).arg(totalScored).arg(memNote));
    // "Keep best %" changed the kept/not-kept boundary -- recolor an
    // already-open graph immediately rather than only on the next full
    // quality pass, so dragging the percent slider visibly moves the
    // cutoff line in real time. refreshSelection() (not refresh()) so this
    // doesn't yank the user's current scrub position back to rank 0 every
    // time the slider ticks.
    if (qualityInspector_) qualityInspector_->refreshSelection();
}

void MainWindow::onInspectQuality() {
    if (!qualityInspector_) qualityInspector_ = new QualityInspectorDialog(controller_, this);
    qualityInspector_->refresh();
    qualityInspector_->show();
    qualityInspector_->raise();
    qualityInspector_->activateWindow();
}

void MainWindow::onAlign() {
    alignButton_->setEnabled(false);
    controller_->setAlignPatchSize(alignPatchSizeSpin_->value());
    controller_->setAlignMaxPoints(alignMaxPointsSpin_->value());
    controller_->setAlignMaxDeviation(alignMaxDeviationSpin_->value());
    progressBar_->setValue(0);
    statusBar()->showMessage("Aligning frames...");
    controller_->alignSelected();
}

void MainWindow::onAlignDone(double averageConfidence) {
    alignButton_->setEnabled(true);
    stackButton_->setEnabled(true);
    inspectAlignmentButton_->setEnabled(true);
    alignLabel_->setText(QString("Average correlation confidence: %1").arg(averageConfidence, 0, 'f', 3));
    statusBar()->showMessage("Alignment complete", 5000);
    // If the inspector is already open (e.g. the user re-aligned after
    // changing the kept percentage), refresh it in place rather than
    // leaving it showing stale points/frames.
    if (alignmentInspector_) alignmentInspector_->refresh();
}

void MainWindow::onInspectAlignment() {
    if (!alignmentInspector_) alignmentInspector_ = new AlignmentInspectorDialog(controller_, this);
    alignmentInspector_->refresh();
    alignmentInspector_->show();
    alignmentInspector_->raise();
    alignmentInspector_->activateWindow();
}

void MainWindow::onStack() {
    stackButton_->setEnabled(false);
    progressBar_->setValue(0);
    statusBar()->showMessage("Stacking...");
    int mode = stackModeCombo_->currentIndex();
    if (mode == 2) {
        DrizzleParams dp;
        dp.scale = drizzleScaleSpin_->value();
        dp.dropFraction = drizzleDropSpin_->value();
        controller_->stackDrizzle(dp);
    } else {
        StackParams sp;
        sp.mode = (mode == 1) ? StackMode::SigmaClip : StackMode::Mean;
        sp.sigmaLow = sigmaLowSpin_->value();
        sp.sigmaHigh = sigmaHighSpin_->value();
        controller_->stackMean(sp);
    }
}

void MainWindow::onStackDone(QImage preview) {
    stackButton_->setEnabled(true);
    sharpenButton_->setEnabled(true);
    preview_->setImage(preview);
    statusBar()->showMessage("Stacking complete", 5000);
}

void MainWindow::onSharpen() {
    sharpenButton_->setEnabled(false);
    statusBar()->showMessage("Sharpening...");
    int mode = sharpenModeCombo_->currentIndex();
    SharpenMode sm = mode == 1 ? SharpenMode::Wavelet : mode == 2 ? SharpenMode::RichardsonLucy : SharpenMode::None;
    WaveletParams wp;
    wp.layerGains = {waveletGainSpins_[0]->value(), waveletGainSpins_[1]->value(), waveletGainSpins_[2]->value(),
                      waveletGainSpins_[3]->value()};
    RLParams rp;
    rp.iterations = rlIterationsSpin_->value();
    rp.psfSigma = rlSigmaSpin_->value();
    controller_->applySharpen(sm, wp, rp);
}

void MainWindow::onSharpenDone(QImage preview) {
    sharpenButton_->setEnabled(true);
    adjustColorButton_->setEnabled(true);
    statusBar()->showMessage("Sharpening complete", 5000);
    if (colorApplied_ && colorAdjustDialog_) {
        // Histogram/color stretch was already applied before this sharpen
        // (re)run -- refresh() updates the dialog's histogram against the
        // new sharpened result and reapplies whatever settings are
        // currently dialed in there (its completion, via colorDone, is
        // what actually updates the preview below), rather than leaving
        // the preview/export showing the unstretched sharpen output, which
        // is what re-running Wavelet or Richardson-Lucy used to do before
        // this cascade existed.
        colorAdjustDialog_->refresh();
    } else {
        preview_->setImage(preview);
    }
}

void MainWindow::onAdjustColor() {
    if (!colorAdjustDialog_) colorAdjustDialog_ = new ColorAdjustmentDialog(controller_, this);
    colorAdjustDialog_->refresh();
    colorAdjustDialog_->show();
    colorAdjustDialog_->raise();
    colorAdjustDialog_->activateWindow();
}

void MainWindow::onColorDone(QImage preview) {
    caDetectButton_->setEnabled(true);
    caApplyButton_->setEnabled(true);
    exportButton_->setEnabled(true);
    colorApplied_ = true;
    statusBar()->showMessage("Color adjustments applied", 5000);
    if (caApplied_) {
        // Chromatic aberration correction had already been applied before
        // this color (re)run -- reapply it now with the same offsets,
        // mirroring onSharpenDone()'s cascade into color, so a color tweak
        // doesn't silently leave the preview/export on a CA correction
        // computed against the previous color result.
        onApplyCA();
    } else {
        preview_->setImage(preview);
    }
}

void MainWindow::onDetectCA() {
    ChromaticAberrationParams detected = controller_->detectCA();
    caRedDxSpin_->setValue(detected.redDx);
    caRedDySpin_->setValue(detected.redDy);
    caBlueDxSpin_->setValue(detected.blueDx);
    caBlueDySpin_->setValue(detected.blueDy);
    statusBar()->showMessage(
        QString("Detected CA offsets -- red (%1, %2), blue (%3, %4) px")
            .arg(detected.redDx, 0, 'f', 2).arg(detected.redDy, 0, 'f', 2)
            .arg(detected.blueDx, 0, 'f', 2).arg(detected.blueDy, 0, 'f', 2),
        5000);
}

void MainWindow::onApplyCA() {
    caApplyButton_->setEnabled(false);
    statusBar()->showMessage("Correcting chromatic aberration...");
    ChromaticAberrationParams params;
    params.redDx = caRedDxSpin_->value();
    params.redDy = caRedDySpin_->value();
    params.blueDx = caBlueDxSpin_->value();
    params.blueDy = caBlueDySpin_->value();
    controller_->applyChromaticAberration(params);
}

void MainWindow::onChromaticAberrationDone(QImage preview) {
    caApplyButton_->setEnabled(true);
    exportButton_->setEnabled(true);
    caApplied_ = true;
    preview_->setImage(preview);
    statusBar()->showMessage("Chromatic aberration correction applied", 5000);
}

void MainWindow::onExportFormatChanged(int index) {
    // PNG (index 0) is always 8-bit; TIFF/FITS (1, 2) can go either way.
    bool canChooseBitDepth = index != 0;
    exportBitDepthCombo_->setEnabled(canChooseBitDepth);
    if (!canChooseBitDepth) exportBitDepthCombo_->setCurrentIndex(0);
}

void MainWindow::onExport() {
    ExportFormat format = ExportFormat::PNG;
    QString filter = "PNG Image (*.png)";
    QString suffix = "png";
    switch (exportFormatCombo_->currentIndex()) {
        case 1:
            format = ExportFormat::TIFF;
            filter = "TIFF Image (*.tif *.tiff)";
            suffix = "tif";
            break;
        case 2:
            format = ExportFormat::FITS;
            filter = "FITS Image (*.fits *.fit *.fts)";
            suffix = "fits";
            break;
        default:
            break;
    }

    QFileDialog dialog(this, "Export Image");
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(filter);
    dialog.setDefaultSuffix(suffix);
    if (dialog.exec() != QDialog::Accepted) return;
    QString path = dialog.selectedFiles().value(0);
    if (path.isEmpty()) return;

    ExportBitDepth bitDepth =
        (exportBitDepthCombo_->currentIndex() == 1) ? ExportBitDepth::Sixteen : ExportBitDepth::Eight;
    bool ok = controller_->exportImage(path, format, bitDepth);
    statusBar()->showMessage(ok ? "Exported " + path : "Export failed", 5000);
    if (!ok) QMessageBox::warning(this, "Export failed", "Could not write image to " + path);
}

void MainWindow::onError(QString message) {
    setEnabledStageButtons();
    if (controller_->frameCount() > 0) assessButton_->setEnabled(true);
    if (qualityReady_) alignButton_->setEnabled(true);
    QMessageBox::warning(this, "Error", message);
    statusBar()->showMessage("Error: " + message, 8000);
}

} // namespace ls
