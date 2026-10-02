// A console application wiring lqtexec together: a worker thread with its own event loop,
// a timer's signal awaited as a sender, an object-bound scope, a deadline, and a shutdown
// that drains the work before the event loop exits.

#include <lqtexec/EventLoopContext.h>
#include <lqtexec/EventLoopThread.h>
#include <lqtexec/ExecWithScope.h>
#include <lqtexec/ObjectScope.h>
#include <lqtexec/WaitSignal.h>

#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <QThread>
#include <QTimer>

#include <chrono>

namespace {

using namespace std::chrono_literals;

// Waits for its timer's ticks, has each one processed on the worker thread, and asks the
// application to stop after the third.
struct Ticker : QObject {
    Ticker(lqtexec::EventLoopScheduler const gui, lqtexec::EventLoopScheduler const worker_,
           lexec::counting_scope &app, lexec::inplace_stop_source &stop_)
        : worker(worker_), stop(stop_), tasks{this, gui, app.get_token()} {
        timer.start(100);
    }

    // spawn takes only work whose errors are handled: connecting and hopping threads may fail
    // to allocate when exceptions are enabled.
    void waitForTick() {
        lexec::spawn(lqtexec::wait_signal(&timer, &QTimer::timeout) | lexec::continues_on(worker) |
                         lexec::then([]() noexcept { return QThread::currentThread()->objectName(); }) |
                         lqtexec::then_on(tasks, [this](QString const &where) noexcept { onTick(where); }) |
                         lexec::upon_error([](auto const &) noexcept { qWarning() << "a tick failed"; }),
                     tasks.get_token());
    }

private:
    void onTick(QString const &where) {
        ++ticks;
        qInfo() << "tick" << ticks << "processed on" << where;
        if (ticks == 3) {
            stop.request_stop();
        } else {
            waitForTick();
        }
    }

    lqtexec::EventLoopScheduler worker;
    lexec::inplace_stop_source &stop;
    QTimer timer;
    int ticks = 0;
    lqtexec::ObjectScope tasks;
};

} // namespace

int main(int argc, char **argv) {
    auto app = QCoreApplication{argc, argv};
    auto gui = lqtexec::EventLoopContext{};
    auto worker = lqtexec::EventLoopThread{QStringLiteral("worker")};
    auto scope = lexec::counting_scope{};
    auto stop = lexec::inplace_stop_source{};

    lexec::spawn(lqtexec::schedule_after(gui.get_scheduler(), 2s) | lexec::then([&stop]() noexcept {
                     qInfo() << "deadline reached";
                     stop.request_stop();
                 }),
                 scope.get_token());

    auto ticker = Ticker{gui.get_scheduler(), worker.get_scheduler(), scope, stop};
    ticker.waitForTick();
    auto const code = lqtexec::exec_with_scope(app, scope, stop);
    qInfo() << "drained, exiting with" << code;
    return code;
}
