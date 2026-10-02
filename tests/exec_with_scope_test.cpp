#include <lqtexec/EventLoopContext.h>
#include <lqtexec/ExecWithScope.h>

#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QCoreApplication>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <thread>

using namespace lqtexec_test;

namespace {

QCoreApplication &app() { return *QCoreApplication::instance(); }

} // namespace

TEST_CASE("a stop request drains the scope and ends exec_with_scope") {
    auto context = lqtexec::EventLoopContext{};
    auto scope = lexec::counting_scope{};
    auto stop = lexec::inplace_stop_source{};
    auto stopped = std::atomic<bool>{false};
    lexec::spawn(lqtexec::schedule_after(context.get_scheduler(), std::chrono::hours{1}) |
                     lexec::upon_stopped([&]() noexcept { stopped = true; }),
                 scope.get_token());
    QTimer::singleShot(10, [&] { stop.request_stop(); });
    CHECK(lqtexec::exec_with_scope(app(), scope, stop) == 0);
    CHECK(stopped);
}

TEST_CASE("quitting the event loop directly still drains the scope") {
    auto context = lqtexec::EventLoopContext{};
    auto scope = lexec::counting_scope{};
    auto stop = lexec::inplace_stop_source{};
    auto finished = std::atomic<bool>{false};
    lexec::spawn(lexec::unstoppable(lqtexec::schedule_after(context.get_scheduler(), 40ms)) |
                     lexec::then([&]() noexcept { finished = true; }),
                 scope.get_token());
    QTimer::singleShot(5, [] { QCoreApplication::quit(); });
    CHECK(lqtexec::exec_with_scope(app(), scope, stop) == 0);
    CHECK(finished);
}

TEST_CASE("a stop request made before exec_with_scope ends it at once") {
    auto scope = lexec::counting_scope{};
    auto stop = lexec::inplace_stop_source{};
    stop.request_stop();
    auto const before = std::chrono::steady_clock::now();
    CHECK(lqtexec::exec_with_scope(app(), scope, stop) == 0);
    CHECK(std::chrono::steady_clock::now() - before < 1s);
}

TEST_CASE("a stop request from another thread ends exec_with_scope") {
    auto context = lqtexec::EventLoopContext{};
    auto scope = lexec::counting_scope{};
    auto stop = lexec::inplace_stop_source{};
    lexec::spawn(lqtexec::schedule_after(context.get_scheduler(), std::chrono::hours{1}) |
                     lexec::upon_stopped([]() noexcept {}),
                 scope.get_token());
    auto requester = std::thread{[&] {
        std::this_thread::sleep_for(20ms);
        stop.request_stop();
    }};
    CHECK(lqtexec::exec_with_scope(app(), scope, stop) == 0);
    requester.join();
}

TEST_CASE("exec_with_scope returns the event loop's exit code") {
    auto scope = lexec::counting_scope{};
    auto stop = lexec::inplace_stop_source{};
    QTimer::singleShot(5, [] { QCoreApplication::exit(3); });
    CHECK(lqtexec::exec_with_scope(app(), scope, stop) == 3);
}
