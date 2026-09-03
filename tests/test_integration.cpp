// End-to-end integration test: drives PipelineController exactly the way
// MainWindow does (open -> quality -> select -> align -> stack -> sharpen
// -> color -> export), against a synthetic SER file, using a QCoreApplication
// event loop so the QtConcurrent-threaded stages' queued signals actually
// get delivered. No widgets are created, so this runs headless with no
// display needed.

#include "gui/PipelineController.h"
#include "TestTempDir.h"
#include "TestLogging.h"

#include <QCoreApplication>
#include <QTimer>
#include <QDebug>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int W = 48, H = 40, NFRAMES = 10;

void writeI32LE(std::ofstream& f, int32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }
void writeI64LE(std::ofstream& f, int64_t v) { f.write(reinterpret_cast<const char*>(&v), 8); }

void renderDisk(std::vector<uint8_t>& buf, int w, int h, double cx, double cy, double radius) {
    buf.assign(static_cast<size_t>(w) * h, 10);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius) buf[static_cast<size_t>(y) * w + x] = 220;
        }
}

std::string writeSyntheticSer(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    char fileId[14] = {'L', 'U', 'C', 'A', 'M', '-', 'R', 'E', 'C', 'O', 'R', 'D', 'E', 'R'};
    f.write(fileId, 14);
    writeI32LE(f, 0);
    writeI32LE(f, 0);
    writeI32LE(f, 1);
    writeI32LE(f, W);
    writeI32LE(f, H);
    writeI32LE(f, 8);
    writeI32LE(f, NFRAMES);
    char pad40[40] = {0};
    f.write(pad40, 40);
    f.write(pad40, 40);
    f.write(pad40, 40);
    writeI64LE(f, 0);
    writeI64LE(f, 0);
    for (int i = 0; i < NFRAMES; ++i) {
        std::vector<uint8_t> buf;
        renderDisk(buf, W, H, W / 2.0 + 0.3 * i, H / 2.0 - 0.2 * i, 8.0);
        f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    }
    return path;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ls::test::installFlushingMessageHandler();
    qInfo() << "test_integration: starting";
    std::string path = writeSyntheticSer(ls::test::tempPath("ls_integration.ser"));

    auto* controller = new ls::PipelineController(&app);
    bool ok = true;

    QObject::connect(controller, &ls::PipelineController::errorOccurred, [&](QString msg) {
        qWarning() << "ERROR:" << msg;
        ok = false;
        QCoreApplication::exit(1);
    });

    QObject::connect(controller, &ls::PipelineController::sequenceOpened, [&](int w, int h, int n, QString fmt) {
        qInfo() << "sequenceOpened" << w << h << n << fmt;
        if (w != W || h != H || n != NFRAMES) {
            qWarning() << "sequence metadata mismatch";
            ok = false;
            QCoreApplication::exit(1);
            return;
        }
        controller->computeQuality();
    });

    QObject::connect(controller, &ls::PipelineController::qualityDone, [&](int total) {
        qInfo() << "qualityDone" << total;
        controller->selectPercent(70.0);
    });

    QObject::connect(controller, &ls::PipelineController::selectionChanged, [&](int kept, int totalScored, double estMB) {
        qInfo() << "selectionChanged" << kept << totalScored << "est MB=" << estMB;
        if (kept < 1) {
            ok = false;
            QCoreApplication::exit(1);
            return;
        }
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
        if (img.isNull()) {
            ok = false;
            QCoreApplication::exit(1);
            return;
        }
        ls::WaveletParams wp;
        controller->applySharpen(ls::SharpenMode::Wavelet, wp, ls::RLParams{});
    });

    QObject::connect(controller, &ls::PipelineController::sharpenDone, [&](QImage img) {
        qInfo() << "sharpenDone" << img.size();
        ls::LevelsParams lp;
        std::vector<std::pair<float, float>> curve = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        ls::SaturationParams satp;
        controller->applyColor(lp, curve, satp);
    });

    QObject::connect(controller, &ls::PipelineController::colorDone, [&](QImage img) {
        qInfo() << "colorDone" << img.size();
        bool exported = controller->exportImage(QString::fromStdString(ls::test::tempPath("ls_integration_out.png")));
        qInfo() << "export ok=" << exported;
        if (!exported) ok = false;
        QCoreApplication::exit(ok ? 0 : 1);
    });

    // 20s was tuned against this sandbox's Linux run (well under a second
    // end-to-end for this tiny 10-frame synthetic sequence) and had no
    // real margin for a slower or more loaded CI runner, so this was
    // widened on the theory that Windows CI just needed more headroom.
    // That theory turned out wrong: the very next CI run still failed at
    // ~20.4s despite this 60s watchdog, and with *zero* logged output --
    // not even the "starting" line above, let alone the per-stage ones
    // below. That means it isn't this timer firing at all; something is
    // killing the process outright (almost certainly an uncaught
    // exception reaching std::terminate() from inside a Qt slot, which
    // Qt does not catch for you) before it has a chance to log or return
    // gracefully. installFlushingMessageHandler() above and
    // ls::test::runEventLoop() below exist specifically to stop that from
    // erasing the evidence next time -- see docs/DEVELOPMENT.md.
    QTimer::singleShot(60000, [&]() {
        qWarning() << "TIMEOUT waiting for pipeline to complete";
        QCoreApplication::exit(1);
    });

    controller->openSequence(QString::fromStdString(path));

    int rc = ls::test::runEventLoop(app);
    if (rc == 0) qInfo() << "integration test passed";
    return rc;
}
