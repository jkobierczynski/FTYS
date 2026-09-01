// Headless verification of the alignment-inspector feature: runs the real
// pipeline through alignSelected(), then reproduces exactly what
// AlignmentInspectorDialog draws (yellow boxes at each point, raw and
// aligned modes) for a couple of frame positions, saved to disk so it can
// actually be looked at -- not part of ctest, just a sanity check that the
// new PipelineController methods return sensible data before trusting the
// GUI dialog wraps them correctly.
#include "gui/PipelineController.h"

#include <QGuiApplication>
#include <QTimer>
#include <QPainter>
#include <QFont>
#include <QDebug>

int main(int argc, char** argv) {
    // QPainter::setFont() touches the font database, which needs at least
    // a QGuiApplication (not just QCoreApplication) even when running
    // headless -- run with QT_QPA_PLATFORM=offscreen, no real display
    // needed.
    QGuiApplication app(argc, argv);
    if (argc < 2) { fprintf(stderr, "usage: %s <file> [keepPercent]\n", argv[0]); return 2; }
    QString path = argv[1];
    double keepPercent = argc >= 3 ? std::atof(argv[2]) : 50.0;

    auto* controller = new ls::PipelineController(&app);
    bool ok = true;

    auto drawAndSave = [&](int pos, bool aligned, const QString& outPath) {
        QImage base = aligned ? controller->alignedFramePreview(pos) : controller->rawSelectedFramePreview(pos);
        if (base.isNull()) { qWarning() << "null preview for pos" << pos; return; }
        QImage canvas = base.convertToFormat(QImage::Format_RGB32);
        const auto& points = controller->alignmentPoints();
        int patchSize = controller->alignmentPatchSize();
        auto shifts = controller->pointShiftsForFrame(pos);

        QPainter painter(&canvas);
        QPen pen(QColor(255, 220, 0));
        pen.setWidth(2);
        painter.setPen(pen);
        QFont f = painter.font(); f.setPointSize(11); f.setBold(true); painter.setFont(f);
        for (size_t i = 0; i < points.size(); ++i) {
            double px = points[i].x, py = points[i].y;
            if (!aligned && i < shifts.size()) { px += shifts[i].dx; py += shifts[i].dy; }
            QRectF box(px - patchSize / 2.0, py - patchSize / 2.0, patchSize, patchSize);
            painter.drawRect(box);
            painter.drawText(QPointF(box.left() + 2, box.top() - 4), QString::number(i + 1));
        }
        painter.end();
        canvas.save(outPath);
        qInfo() << "saved" << outPath << "pos" << pos << "aligned" << aligned << "points" << points.size()
                << "patchSize" << patchSize;
    };

    QObject::connect(controller, &ls::PipelineController::errorOccurred, [&](QString msg) {
        qWarning() << "ERROR:" << msg;
        ok = false;
        QCoreApplication::exit(1);
    });
    QObject::connect(controller, &ls::PipelineController::sequenceOpened, [&](int, int, int, QString) {
        controller->computeQuality();
    });
    QObject::connect(controller, &ls::PipelineController::qualityDone, [&](int) {
        controller->selectPercent(keepPercent);
    });
    QObject::connect(controller, &ls::PipelineController::selectionChanged, [&](int, int, double) {
        controller->alignSelected();
    });
    QObject::connect(controller, &ls::PipelineController::alignDone, [&](double conf) {
        qInfo() << "alignDone confidence=" << conf << "alignedFrameCount=" << controller->alignedFrameCount()
                << "points=" << controller->alignmentPoints().size()
                << "patchSize=" << controller->alignmentPatchSize();

        int n = controller->alignedFrameCount();
        for (int pos : {0, n / 4, n / 2, n - 1}) {
            drawAndSave(pos, false, QString("/tmp/inspector_raw_pos%1.png").arg(pos));
            drawAndSave(pos, true, QString("/tmp/inspector_aligned_pos%1.png").arg(pos));
        }
        QCoreApplication::exit(ok ? 0 : 1);
    });

    QTimer::singleShot(300000, [&]() { qWarning() << "TIMEOUT"; QCoreApplication::exit(1); });
    controller->openSequence(path);
    return app.exec();
}
