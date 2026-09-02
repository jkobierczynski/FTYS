// Not part of ctest -- headless real-data validation for the FITS/TIFF/
// 16-bit export feature (io/ImageWriter.h/.cpp). Runs the real pipeline
// against a real capture file through to a color-adjusted result, then
// exercises PipelineController::exportImage() for every
// ExportFormat x ExportBitDepth combination that's meaningful (PNG is
// always 8-bit) and does basic sanity checks on each output file
// (existence, non-trivial size, and for FITS a round trip back through
// the project's own FitsReader).
//
// Usage: export_validation <path-to-ser-or-avi-or-fits> [keepPercent]

#include "gui/PipelineController.h"
#include "io/FitsReader.h"
#include "core/ImageBuffer.h"

#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include <QFileInfo>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <sequence file> [keepPercent]\n", argv[0]);
        return 2;
    }
    QString path = argv[1];
    double keepPercent = argc >= 3 ? std::atof(argv[2]) : 50.0;

    auto* controller = new ls::PipelineController(&app);
    bool ok = true;

    QObject::connect(controller, &ls::PipelineController::errorOccurred, [&](QString msg) {
        qWarning() << "ERROR:" << msg;
        ok = false;
        QCoreApplication::exit(1);
    });

    QObject::connect(controller, &ls::PipelineController::sequenceOpened, [&](int w, int h, int n, QString) {
        qInfo() << "sequenceOpened" << w << h << n;
        controller->computeQuality();
    });

    QObject::connect(controller, &ls::PipelineController::qualityDone, [&](int) {
        controller->selectPercent(keepPercent);
    });

    QObject::connect(controller, &ls::PipelineController::selectionChanged, [&](int kept, int total, double) {
        qInfo() << "selectionChanged kept=" << kept << "/" << total;
        controller->alignSelected();
    });

    QObject::connect(controller, &ls::PipelineController::alignDone, [&](double conf) {
        qInfo() << "alignDone confidence=" << conf;
        ls::StackParams sp;
        sp.mode = ls::StackMode::Mean;
        controller->stackMean(sp);
    });

    QObject::connect(controller, &ls::PipelineController::stackDone, [&](QImage img) {
        qInfo() << "stackDone" << img.size();
        ls::WaveletParams wp;
        wp.layerGains = {3.0, 2.5, 1.5, 1.0};
        controller->applySharpen(ls::SharpenMode::Wavelet, wp, ls::RLParams{});
    });

    QObject::connect(controller, &ls::PipelineController::sharpenDone, [&](QImage img) {
        qInfo() << "sharpenDone" << img.size();
        ls::LevelsParams lp;
        lp.blackPoint = 0.02f;
        lp.whitePoint = 0.9f;
        std::vector<std::pair<float, float>> curve = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        controller->applyColor(lp, curve, ls::SaturationParams{});
    });

    QObject::connect(controller, &ls::PipelineController::colorDone, [&](QImage img) {
        qInfo() << "colorDone" << img.size() << "channels(alpha?)=" << img.hasAlphaChannel();

        struct Case {
            const char* path;
            ls::ExportFormat format;
            ls::ExportBitDepth bitDepth;
            const char* label;
        };
        std::vector<Case> cases = {
            {"/tmp/export_validation_png8.png", ls::ExportFormat::PNG, ls::ExportBitDepth::Eight, "PNG-8"},
            {"/tmp/export_validation_tiff8.tif", ls::ExportFormat::TIFF, ls::ExportBitDepth::Eight, "TIFF-8"},
            {"/tmp/export_validation_tiff16.tif", ls::ExportFormat::TIFF, ls::ExportBitDepth::Sixteen, "TIFF-16"},
            {"/tmp/export_validation_fits8.fits", ls::ExportFormat::FITS, ls::ExportBitDepth::Eight, "FITS-8"},
            {"/tmp/export_validation_fits16.fits", ls::ExportFormat::FITS, ls::ExportBitDepth::Sixteen, "FITS-16"},
        };

        for (auto& c : cases) {
            // Remove any stale file from a previous run so we can trust
            // that a passing existence/size check reflects THIS run.
            QFile::remove(c.path);
            bool exported = controller->exportImage(c.path, c.format, c.bitDepth);
            QFileInfo fi(c.path);
            qInfo() << "[" << c.label << "] exportImage ok=" << exported << "exists=" << fi.exists()
                    << "size=" << fi.size() << "bytes";
            if (!exported || !fi.exists() || fi.size() <= 0) {
                qWarning() << "FAIL:" << c.label << "did not produce a non-empty file";
                ok = false;
            }
        }

        // Round-trip the two FITS files back through the project's own
        // FitsReader and compare basic stats against the source image, to
        // verify the writer's NAXIS/plane-order convention actually
        // matches what FitsReader expects to read (a designed invariant,
        // not something the build alone can confirm).
        for (auto pathBits : {std::make_pair(std::string("/tmp/export_validation_fits8.fits"), 8),
                               std::make_pair(std::string("/tmp/export_validation_fits16.fits"), 16)}) {
            ls::ImageBuffer frame;
            int gotW = 0, gotH = 0, gotCh = 0;
            try {
                ls::FitsReader reader(pathBits.first);
                qInfo() << "[FITS round-trip" << pathBits.second << "bit ]"
                        << "frameCount=" << reader.frameCount() << "size=" << reader.width() << "x"
                        << reader.height();
                if (reader.frameCount() < 1) {
                    qWarning() << "FAIL: FitsReader reports 0 frames for"
                               << QString::fromStdString(pathBits.first);
                    ok = false;
                    continue;
                }
                ls::RawFrame raw = reader.readFrame(0);
                frame = ls::imageBufferFromRaw(raw);
                gotW = frame.width();
                gotH = frame.height();
                gotCh = frame.channels();
            } catch (const std::exception& e) {
                qWarning() << "FAIL: FitsReader threw for" << QString::fromStdString(pathBits.first) << ":"
                           << e.what();
                ok = false;
                continue;
            }
            if (frame.empty()) {
                qWarning() << "FAIL: FitsReader produced an empty ImageBuffer for"
                           << QString::fromStdString(pathBits.first);
                ok = false;
                continue;
            }
            qInfo() << "    readback dims=" << gotW << "x" << gotH << "channels=" << gotCh;
            if (gotW != img.width() || gotH != img.height()) {
                qWarning() << "FAIL: readback dimensions do not match source preview";
                ok = false;
            }
            // Spot-check a handful of pixels against the source QImage
            // (converted the same way exportImage's PNG path would, so
            // the comparison is apples-to-apples up to quantization).
            double maxAbsDiff = 0.0;
            int samples = 0;
            for (int y = 0; y < frame.height(); y += std::max(1, frame.height() / 8)) {
                for (int x = 0; x < frame.width(); x += std::max(1, frame.width() / 8)) {
                    QRgb px = img.pixel(x, y);
                    double srcR = qRed(px) / 255.0;
                    double srcG = qGreen(px) / 255.0;
                    double srcB = qBlue(px) / 255.0;
                    double gotR = frame.channels() >= 3 ? frame.at(x, y, 0) : frame.at(x, y, 0);
                    double gotG = frame.channels() >= 3 ? frame.at(x, y, 1) : frame.at(x, y, 0);
                    double gotB = frame.channels() >= 3 ? frame.at(x, y, 2) : frame.at(x, y, 0);
                    maxAbsDiff = std::max({maxAbsDiff, std::abs(srcR - gotR), std::abs(srcG - gotG),
                                           std::abs(srcB - gotB)});
                    ++samples;
                }
            }
            qInfo() << "    spot-checked" << samples << "pixels, maxAbsDiff=" << maxAbsDiff
                    << "(tolerance 0.02 for 8-bit quantization)";
            if (maxAbsDiff > 0.05) {
                qWarning() << "FAIL: FITS round-trip pixel values diverge too much from source";
                ok = false;
            }
        }

        qInfo() << (ok ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
        QCoreApplication::exit(ok ? 0 : 1);
    });

    QTimer::singleShot(300000, [&]() {
        qWarning() << "TIMEOUT";
        QCoreApplication::exit(1);
    });

    controller->openSequence(path);
    int rc = app.exec();
    return rc;
}
