// A program of its own: SignalStop must exist before any other thread, so that every
// thread inherits the blocked signals. It sends itself SIGTERM, then SIGINT while the work
// drains, and succeeds if the work stopped, the event loop exited cleanly, and the second
// signal did not end the process.

#include <lqtexec/EventLoopContext.h>
#include <lqtexec/ExecWithScope.h>
#include <lqtexec/SignalStop.h>

#include <QCoreApplication>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>

#include <unistd.h>

int main(int argc, char **argv) {
    auto stop = lexec::inplace_stop_source{};
    auto const signalStop = lqtexec::SignalStop{stop};
    auto app = QCoreApplication{argc, argv};
    auto context = lqtexec::EventLoopContext{};
    auto scope = lexec::counting_scope{};
    auto stopped = std::atomic<bool>{false};
    auto drained = std::atomic<bool>{false};
    lexec::spawn(lqtexec::schedule_after(context.get_scheduler(), std::chrono::hours{1}) |
                     lexec::upon_stopped([&stopped]() noexcept { stopped = true; }),
                 scope.get_token());
    lexec::spawn(lexec::unstoppable(lqtexec::schedule_after(context.get_scheduler(), std::chrono::milliseconds{200})) |
                     lexec::then([&drained]() noexcept { drained = true; }),
                 scope.get_token());
    QTimer::singleShot(20, [] { ::kill(::getpid(), SIGTERM); });
    QTimer::singleShot(100, [] { ::kill(::getpid(), SIGINT); });
    auto const code = lqtexec::exec_with_scope(app, scope, stop);
    std::printf("exit code %d, stopped %d, drained %d, error %d\n", code, stopped.load(), drained.load(),
                signalStop.error().value());
    return code == 0 and stopped and drained and not signalStop.error() ? 0 : 1;
}
