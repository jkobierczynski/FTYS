// Manual driver (not part of ctest) for running the real pipeline against
// a real capture file from the command line, with peak-memory reporting at
// each stage -- used to verify the frame-caching fix against actual large
// AVI captures rather than only small synthetic test data.
//
// Usage: manual_pipeline_run <path-to-ser-or-avi-or-fits> [keepPercent]

#include "gui/PipelineController.h"

#include <QCoreApplication>
#include <QTimer>
#include <QDebug>
#include <QElapsedTimer>
#include <QDir>

#include <algorithm>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <fstream>
#include <sstream>
#endif

namespace {

// Current resident set size, in KB -- used only for this diagnostic's own
// per-stage memory reporting, not by the app itself. /proc/self/status is
// Linux-specific; GetProcessMemoryInfo is the equivalent on Windows.
long vmRssKb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<long>(pmc.WorkingSetSize / 1024);
    }
    return -1;
#else
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            long kb = 0;
            iss >> kb;
            return kb;
        }
    }
    return -1;
#endif
}

void reportMem(const char* stage) {
    qInfo().noquote() << QString("[%1] VmRSS = %2 MB").arg(stage).arg(vmRssKb() / 1024.0, 0, 'f', 1);
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <sequence file> [keepPercent]\n", argv[0]);
        return 2;
    }
    QString path = argv[1];
    double keepPercent = argc >= 3 ? std::atof(argv[2]) : 50.0;
    double g1 = argc >= 4 ? std::atof(argv[3]) : 3.0;
    double g2 = argc >= 5 ? std::atof(argv[4]) : 2.5;
    double g3 = argc >= 6 ? std::atof(argv[5]) : 1.5;
    double g4 = argc >= 7 ? std::atof(argv[6]) : 1.0;
    double blackPoint = argc >= 8 ? std::atof(argv[7]) : 0.02;
    double whitePoint = argc >= 9 ? std::atof(argv[8]) : 0.9;
    std::string stackMode = argc >= 10 ? argv[9] : "mean"; // "mean" or "drizzle"
    double drizzleScale = argc >= 11 ? std::atof(argv[10]) : 2.0;

    auto* controller = new ls::PipelineController(&app);
    bool ok = true;
    QElapsedTimer timer;
    timer.start();

    QObject::connect(controller, &ls::PipelineController::errorOccurred, [&](QString msg) {
        qWarning() << "ERROR:" << msg;
        ok = false;
        QCoreApplication::exit(1);
    });

    QObject::connect(controller, &ls::PipelineController::sequenceOpened, [&](int w, int h, int n, QString fmt) {
        qInfo() << "sequenceOpened" << w << h << n << fmt << "at" << timer.elapsed() << "ms";
        reportMem("opened");
        // Save a handful of raw single frames (unaligned, unstacked) so we
        // can compare native per-frame sharpness against the stacked
        // result -- tells us whether softness comes from the source
        // footage itself or from something introduced by alignment/stacking.
        for (int idx : {0, n / 4, n / 2}) {
            QImage f = controller->previewFrame(idx);
            f.save(QDir(QDir::tempPath()).filePath(QString("real_test_raw_frame_%1.png").arg(idx)));
        }
        controller->computeQuality();
    });

    QObject::connect(controller, &ls::PipelineController::qualityDone, [&](int total) {
        qInfo() << "qualityDone" << total << "at" << timer.elapsed() << "ms";
        reportMem("quality-done");

        auto scores = controller->qualityScores();
        std::sort(scores.begin(), scores.end(), [](const ls::FrameQuality& a, const ls::FrameQuality& b) {
            return a.score > b.score;
        });
        qInfo() << "--- top 8 quality scores ---";
        for (int i = 0; i < 8 && i < static_cast<int>(scores.size()); ++i)
            qInfo() << "  idx" << static_cast<int>(scores[i].index) << "score" << scores[i].score;
        qInfo() << "--- bottom 3 quality scores ---";
        for (int i = std::max(0, static_cast<int>(scores.size()) - 3); i < static_cast<int>(scores.size()); ++i)
            qInfo() << "  idx" << static_cast<int>(scores[i].index) << "score" << scores[i].score;

        // Save the single best-scoring raw frame (unaligned, unstacked) so
        // we can compare its native sharpness directly against both frame
        // 0 and the final stack.
        if (!scores.empty()) {
            QImage best = controller->previewFrame(static_cast<int>(scores[0].index));
            best.save(QDir(QDir::tempPath()).filePath(QString("real_test_best_raw_frame_%1.png").arg(scores[0].index)));
            qInfo() << "saved best raw frame idx" << static_cast<int>(scores[0].index);

            QImage worst = controller->previewFrame(static_cast<int>(scores.back().index));
            worst.save(QDir(QDir::tempPath()).filePath(QString("real_test_worst_raw_frame_%1.png").arg(scores.back().index)));
            qInfo() << "saved worst raw frame idx" << static_cast<int>(scores.back().index);
        }

        controller->selectPercent(keepPercent);
    });

    QObject::connect(controller, &ls::PipelineController::selectionChanged, [&](int kept, int totalScored, double estMB) {
        qInfo() << "selectionChanged kept=" << kept << "/" << totalScored << "estMB=" << estMB;
        controller->alignSelected();
    });

    QObject::connect(controller, &ls::PipelineController::alignDone, [&](double conf) {
        qInfo() << "alignDone confidence=" << conf << "at" << timer.elapsed() << "ms";
        reportMem("align-done");
        if (stackMode == "drizzle") {
            ls::DrizzleParams dp;
            dp.scale = drizzleScale;
            dp.dropFraction = 0.65;
            qInfo() << "stacking with drizzle scale=" << drizzleScale;
            controller->stackDrizzle(dp);
        } else {
            ls::StackParams sp;
            sp.mode = ls::StackMode::Mean;
            controller->stackMean(sp);
        }
    });

    QObject::connect(controller, &ls::PipelineController::stackDone, [&](QImage img) {
        qInfo() << "stackDone" << img.size() << "at" << timer.elapsed() << "ms";
        reportMem("stack-done");
        img.save(QDir(QDir::tempPath()).filePath("real_test_stack_only.png"));
        ls::WaveletParams wp;
        wp.layerGains = {g1, g2, g3, g4};
        qInfo() << "wavelet gains" << g1 << g2 << g3 << g4;
        controller->applySharpen(ls::SharpenMode::Wavelet, wp, ls::RLParams{});
    });

    QObject::connect(controller, &ls::PipelineController::sharpenDone, [&](QImage img) {
        qInfo() << "sharpenDone" << img.size() << "at" << timer.elapsed() << "ms";
        reportMem("sharpen-done");
        img.save(QDir(QDir::tempPath()).filePath("real_test_sharpen_only.png"));
        ls::LevelsParams lp;
        lp.blackPoint = static_cast<float>(blackPoint);
        lp.whitePoint = static_cast<float>(whitePoint);
        std::vector<std::pair<float, float>> curve = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        ls::SaturationParams satp;
        controller->applyColor(lp, curve, satp);
    });

    QObject::connect(controller, &ls::PipelineController::colorDone, [&](QImage img) {
        qInfo() << "colorDone" << img.size() << "at" << timer.elapsed() << "ms";
        reportMem("color-done");
        bool exported = controller->exportImage(QDir(QDir::tempPath()).filePath("real_test_out.png"));
        qInfo() << "export ok=" << exported;
        ok = ok && exported;
        QCoreApplication::exit(ok ? 0 : 1);
    });

    QTimer::singleShot(300000, [&]() {
        qWarning() << "TIMEOUT";
        QCoreApplication::exit(1);
    });

    controller->openSequence(path);
    int rc = app.exec();
    qInfo() << "TOTAL TIME" << timer.elapsed() << "ms, peak reporting above";
    return rc;
}
