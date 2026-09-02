#include "gui/ColorAdjustmentDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QList>

namespace ls {

ColorAdjustmentDialog::ColorAdjustmentDialog(PipelineController* controller, QWidget* parent)
    : QDialog(parent), controller_(controller) {
    setWindowTitle("Adjust Color");
    resize(900, 860);

    curvesWidget_ = new CurvesWidget(this);
    curvesWidget_->setMinimumHeight(260);
    preview_ = new PreviewWidget(this);

    auto* curveLegend = new QLabel(
        "Double-click empty space on the curve to add a point, double-click a point to remove it, drag a point to "
        "move it. The curve is applied on top of the black/white/gamma levels below, not instead of them.");
    curveLegend->setWordWrap(true);
    curveLegend->setStyleSheet("color: #999;");

    // --- Levels --------------------------------------------------------
    auto* levelsGroup = new QGroupBox("Levels");
    auto* levelsForm = new QFormLayout(levelsGroup);
    blackPointSpin_ = new QDoubleSpinBox;
    blackPointSpin_->setRange(0.0, 0.99);
    blackPointSpin_->setSingleStep(0.01);
    blackPointSpin_->setValue(0.0);
    whitePointSpin_ = new QDoubleSpinBox;
    whitePointSpin_->setRange(0.01, 1.0);
    whitePointSpin_->setSingleStep(0.01);
    whitePointSpin_->setValue(1.0);
    gammaSpin_ = new QDoubleSpinBox;
    gammaSpin_->setRange(0.1, 5.0);
    gammaSpin_->setSingleStep(0.05);
    gammaSpin_->setValue(1.0);
    levelsForm->addRow("Black point:", blackPointSpin_);
    levelsForm->addRow("White point:", whitePointSpin_);
    levelsForm->addRow("Gamma:", gammaSpin_);

    // --- Brightness & color balance --------------------------------------
    auto* balanceGroup = new QGroupBox("Brightness && Color Balance");
    auto* balanceForm = new QFormLayout(balanceGroup);
    brightnessSpin_ = new QDoubleSpinBox;
    brightnessSpin_->setRange(-0.5, 0.5);
    brightnessSpin_->setSingleStep(0.01);
    brightnessSpin_->setValue(0.0);
    brightnessSpin_->setToolTip("Additive offset applied to every channel -- a simple overall lightening/darkening "
                                 "on top of whatever the levels stretch above already did.");
    redGainSpin_ = new QDoubleSpinBox;
    redGainSpin_->setRange(0.0, 3.0);
    redGainSpin_->setSingleStep(0.02);
    redGainSpin_->setValue(1.0);
    greenGainSpin_ = new QDoubleSpinBox;
    greenGainSpin_->setRange(0.0, 3.0);
    greenGainSpin_->setSingleStep(0.02);
    greenGainSpin_->setValue(1.0);
    blueGainSpin_ = new QDoubleSpinBox;
    blueGainSpin_->setRange(0.0, 3.0);
    blueGainSpin_->setSingleStep(0.02);
    blueGainSpin_->setValue(1.0);
    auto* balanceTip = new QLabel("Independent per-channel gain -- push blue up to cool the image, red up to warm "
                                   "it. No effect on mono captures.");
    balanceTip->setWordWrap(true);
    balanceTip->setStyleSheet("color: #999;");
    balanceForm->addRow("Brightness:", brightnessSpin_);
    balanceForm->addRow("Red gain:", redGainSpin_);
    balanceForm->addRow("Green gain:", greenGainSpin_);
    balanceForm->addRow("Blue gain:", blueGainSpin_);
    balanceForm->addRow(balanceTip);

    // --- Hue & saturation -------------------------------------------------
    auto* hueSatGroup = new QGroupBox("Hue && Saturation");
    auto* hueSatForm = new QFormLayout(hueSatGroup);
    hueSpin_ = new QDoubleSpinBox;
    hueSpin_->setRange(-180.0, 180.0);
    hueSpin_->setSingleStep(1.0);
    hueSpin_->setValue(0.0);
    hueSpin_->setSuffix(QString::fromUtf8(" \xC2\xB0"));
    hueSpin_->setToolTip("Rotates every pixel's hue around the color wheel; saturation and brightness are left "
                          "alone. No effect on mono captures.");
    saturationSpin_ = new QDoubleSpinBox;
    saturationSpin_->setRange(0.0, 3.0);
    saturationSpin_->setSingleStep(0.05);
    saturationSpin_->setValue(1.0);
    hueSatForm->addRow("Hue:", hueSpin_);
    hueSatForm->addRow("Saturation:", saturationSpin_);

    resetButton_ = new QPushButton("Reset to Defaults");

    auto* controlsRow = new QHBoxLayout;
    controlsRow->addWidget(levelsGroup);
    controlsRow->addWidget(balanceGroup);
    controlsRow->addWidget(hueSatGroup);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(curvesWidget_);
    layout->addWidget(curveLegend);
    layout->addLayout(controlsRow);
    layout->addWidget(resetButton_, 0, Qt::AlignRight);
    layout->addWidget(preview_, 1);

    debounceTimer_ = new QTimer(this);
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(150);

    const QList<QDoubleSpinBox*> spins = {blackPointSpin_, whitePointSpin_, gammaSpin_,   brightnessSpin_,
                                           redGainSpin_,    greenGainSpin_,  blueGainSpin_, hueSpin_,
                                           saturationSpin_};
    for (QDoubleSpinBox* spin : spins) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                &ColorAdjustmentDialog::onControlChanged);
    }
    connect(curvesWidget_, &CurvesWidget::curveChanged, this, &ColorAdjustmentDialog::onCurveChanged);
    connect(debounceTimer_, &QTimer::timeout, this, &ColorAdjustmentDialog::onDebounceTimeout);
    connect(resetButton_, &QPushButton::clicked, this, &ColorAdjustmentDialog::onReset);
    // Context object `this` matters here: colorDone is emitted from a
    // QtConcurrent worker thread, and without a context object living on
    // the main thread this connection would run directly on that worker
    // thread instead of being queued -- see the alignment/quality
    // inspector dialogs' own history of this exact bug.
    connect(controller_, &PipelineController::colorDone, this, &ColorAdjustmentDialog::onColorResultReady);
}

void ColorAdjustmentDialog::refresh() {
    curvesWidget_->setHistogram(controller_->preColorHistogram());
    debounceTimer_->stop();
    applyNow();
}

void ColorAdjustmentDialog::onControlChanged() {
    debounceTimer_->start(); // restarts if already running -- coalesces rapid spin-box changes
}

void ColorAdjustmentDialog::onCurveChanged(const std::vector<std::pair<float, float>>&) {
    debounceTimer_->start();
}

void ColorAdjustmentDialog::onDebounceTimeout() {
    applyNow();
}

void ColorAdjustmentDialog::onReset() {
    const QList<QDoubleSpinBox*> spins = {blackPointSpin_, whitePointSpin_, gammaSpin_,   brightnessSpin_,
                                           redGainSpin_,    greenGainSpin_,  blueGainSpin_, hueSpin_,
                                           saturationSpin_};
    // Block signals while resetting so each spin box's valueChanged doesn't
    // restart the debounce timer nine times over -- one single applyNow()
    // call at the end instead.
    for (QDoubleSpinBox* spin : spins) spin->blockSignals(true);
    blackPointSpin_->setValue(0.0);
    whitePointSpin_->setValue(1.0);
    gammaSpin_->setValue(1.0);
    brightnessSpin_->setValue(0.0);
    redGainSpin_->setValue(1.0);
    greenGainSpin_->setValue(1.0);
    blueGainSpin_->setValue(1.0);
    hueSpin_->setValue(0.0);
    saturationSpin_->setValue(1.0);
    for (QDoubleSpinBox* spin : spins) spin->blockSignals(false);
    curvesWidget_->resetToIdentity(); // doesn't emit curveChanged itself, unlike a drag/double-click
    debounceTimer_->stop();
    applyNow();
}

void ColorAdjustmentDialog::onColorResultReady(QImage preview) {
    preview_->setImage(preview);
    if (!fitDone_ && isVisible()) {
        // Safe to fit inline here only once the dialog is already
        // realized (a result arriving while it's open); the very-first-
        // open case is handled by showEvent() instead, same convention as
        // QualityInspectorDialog/AlignmentInspectorDialog.
        preview_->fitToWindow();
        fitDone_ = true;
    }
}

void ColorAdjustmentDialog::applyNow() {
    LevelsParams lp;
    lp.blackPoint = static_cast<float>(blackPointSpin_->value());
    lp.whitePoint = static_cast<float>(whitePointSpin_->value());
    lp.gamma = static_cast<float>(gammaSpin_->value());

    BrightnessParams bp;
    bp.brightness = static_cast<float>(brightnessSpin_->value());

    ColorBalanceParams cbp;
    cbp.redGain = static_cast<float>(redGainSpin_->value());
    cbp.greenGain = static_cast<float>(greenGainSpin_->value());
    cbp.blueGain = static_cast<float>(blueGainSpin_->value());

    HueParams hp;
    hp.hueDegrees = static_cast<float>(hueSpin_->value());

    SaturationParams sp;
    sp.saturation = static_cast<float>(saturationSpin_->value());

    controller_->applyColor(lp, curvesWidget_->controlPoints(), sp, cbp, hp, bp);
}

void ColorAdjustmentDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!fitDone_) {
        // MainWindow calls refresh() (which kicks off the first applyNow())
        // before show(), so onColorResultReady may see isVisible() == false
        // and skip fitting if the result races ahead of the dialog actually
        // being shown -- defer one event-loop turn so the viewport has its
        // real, laid-out size by the time fitToWindow() runs. Same fix as
        // the alignment/quality inspector dialogs use.
        QTimer::singleShot(0, this, [this]() {
            preview_->fitToWindow();
            fitDone_ = true;
        });
    }
}

} // namespace ls
