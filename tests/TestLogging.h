#pragma once

// Shared helper for the Qt-event-loop-driven manual/test drivers in this
// directory (test_integration, export_validation, inspector_verify,
// manual_pipeline_run). Two problems observed for real in CI motivated
// this:
//
// 1. A test failed in ~20s with *zero* logged output -- not even the very
//    first qInfo() line the driver emits right after opening the sequence.
//    That's the signature of the process dying hard (an uncaught C++
//    exception reaching std::terminate(), the same class of bug as the
//    earlier /tmp-path crash, just this time from inside the pipeline's
//    own async-delivered slots rather than test fixture I/O) rather than
//    genuinely failing partway through: a graceful qInfo()/qWarning() and
//    a clean exit would have left *something* in the log even on a crash
//    path, but a process killed abruptly can lose whatever was still
//    sitting in a stream buffer.
// 2. Qt does not catch exceptions thrown from inside a slot -- one thrown
//    on a QtConcurrent worker thread and redelivered via a queued
//    connection, or thrown directly in a lambda connected to a signal,
//    propagates up through Qt's event dispatch and out of
//    QCoreApplication::exec() itself. Nothing in these single-main()
//    drivers was catching that, so it went straight to std::terminate().
//
// installFlushingMessageHandler() makes sure every qDebug/qInfo/qWarning/
// qCritical line is written AND flushed immediately, so a subsequent hard
// crash can no longer erase the log leading up to it. runEventLoop() wraps
// app.exec() in try/catch so an exception that reaches the event loop gets
// one clear diagnostic line instead of silently vanishing into an unhelpful
// process exit code -- see docs/DEVELOPMENT.md.

#include <QtGlobal>
#include <QString>

#include <cstdio>
#include <exception>

namespace ls::test {

inline void installFlushingMessageHandler() {
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext&, const QString& msg) {
        FILE* out = (type == QtInfoMsg || type == QtDebugMsg) ? stdout : stderr;
        fprintf(out, "%s\n", qPrintable(msg));
        fflush(out);
    });
}

template <typename App>
int runEventLoop(App& app) {
    try {
        return app.exec();
    } catch (const std::exception& e) {
        fprintf(stderr, "UNCAUGHT EXCEPTION reached the event loop: %s\n", e.what());
        fflush(stderr);
        return 1;
    } catch (...) {
        fprintf(stderr, "UNCAUGHT NON-STANDARD EXCEPTION reached the event loop\n");
        fflush(stderr);
        return 1;
    }
}

} // namespace ls::test
