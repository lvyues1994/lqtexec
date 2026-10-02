// A Widgets application showing ObjectScope and how windows close: job windows whose work
// stops when they are closed, a cancel button racing the work, saves that nothing
// interrupts, and a shutdown, from the main window, Quit or SIGINT/SIGTERM, that drains the
// work before the event loop exits.
//
//   lqtexec_widgets_demo                      interactive
//   lqtexec_widgets_demo --script             drives itself through the UI, checks the journal
//   lqtexec_widgets_demo --script --screenshots DIR

#include "Journal.h"
#include "MainWindow.h"
#include "Script.h"
#include "Services.h"

#include <lqtexec/EventLoopContext.h>
#include <lqtexec/ExecWithScope.h>
#include <lqtexec/ThreadPoolScheduler.h>

#ifdef Q_OS_UNIX
#include <lqtexec/SignalStop.h>
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QThread>
#include <QThreadPool>

#include <memory>
#include <optional>

int main(int argc, char **argv) {
    // Before any thread, so that every thread inherits the blocked signals.
    auto stop = lexec::inplace_stop_source{};
#ifdef Q_OS_UNIX
    auto const signalStop = lqtexec::SignalStop{stop};
#endif
    auto app = QApplication{argc, argv};
    QThread::currentThread()->setObjectName(QStringLiteral("gui"));
    // The shutdown goes through the stop request, not through the last window closing.
    app.setQuitOnLastWindowClosed(false);

    auto parser = QCommandLineParser{};
    parser.addHelpOption();
    auto const scriptOption = QCommandLineOption{QStringLiteral("script"), QStringLiteral("Drive the demo and check it.")};
    auto const screenshotOption = QCommandLineOption{QStringLiteral("screenshots"),
                                                     QStringLiteral("Save screenshots of the scripted run into <dir>."),
                                                     QStringLiteral("dir")};
    parser.addOption(scriptOption);
    parser.addOption(screenshotOption);
    parser.process(app);

    auto gui = lqtexec::EventLoopContext{};
    auto journal = demo::Journal{};
    auto activity = demo::Activity{};
    auto scope = lexec::counting_scope{};
    auto const services = demo::Services{gui.get_scheduler(), lqtexec::ThreadPoolScheduler{QThreadPool::globalInstance()},
                                         &scope, &stop, &journal, &activity};
    auto window = demo::MainWindow{services};
    window.show();

    auto script = std::unique_ptr<demo::Script>{};
    if (parser.isSet(scriptOption)) {
        auto screenshots = std::optional<QDir>{};
        if (parser.isSet(screenshotOption)) {
            screenshots.emplace(parser.value(screenshotOption));
            screenshots->mkpath(QStringLiteral("."));
        }
        script = std::make_unique<demo::Script>(window, journal, std::move(screenshots));
        script->start();
    }

    auto const code = lqtexec::exec_with_scope(app, scope, stop);
    journal.write(QStringLiteral("app: drained, the event loop exited with %1").arg(code));
    if (script) {
        return script->verify() ? code : 2;
    }
    return code;
}
