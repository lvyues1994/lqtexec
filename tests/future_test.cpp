#include <lqtexec/EventLoopContext.h>
#include <lqtexec/Future.h>

#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QFuture>
#include <QFutureInterface>
#include <QThread>

#if LEXEC_HAS_EXCEPTIONS
#include <QException>
#endif

#include <optional>
#include <stdexcept>
#include <thread>

using namespace lqtexec_test;

namespace {

#if LEXEC_HAS_EXCEPTIONS
struct TestException : QException {
    void raise() const override { throw *this; }
    TestException *clone() const override { return new TestException(*this); }
};
#endif

template <class T>
QFutureInterface<T> startedInterface() {
    auto interface = QFutureInterface<T>{};
    interface.reportStarted();
    return interface;
}

} // namespace

TEST_CASE("from_future completes with the future's result, on the scheduler's thread") {
    auto context = lqtexec::EventLoopContext{};
    auto interface = startedInterface<int>();
    auto value = std::optional<int>{};
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::from_future(context.get_scheduler(), interface.future()),
                             recordInto(completion, [&value](int const v) noexcept { value = v; }));
    lexec::start(op);
    processEventsFor(5ms);
    CHECK_FALSE(completion.done());
    auto reporter = std::thread{[&] {
        interface.reportResult(42);
        interface.reportFinished();
    }};
    reporter.join();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(value == 42);
    CHECK(completion.thread == QThread::currentThread());
}

TEST_CASE("from_future on a finished QFuture<void>") {
    auto context = lqtexec::EventLoopContext{};
    auto interface = startedInterface<void>();
    interface.reportFinished();
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::from_future(context.get_scheduler(), interface.future()), recordInto(completion));
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::value);
}

TEST_CASE("from_future on a cancelled future completes with set_stopped") {
    auto context = lqtexec::EventLoopContext{};
    auto interface = startedInterface<int>();
    interface.reportCanceled();
    interface.reportFinished();
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::from_future(context.get_scheduler(), interface.future()), recordInto(completion));
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::stopped);
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("from_future passes the future's exception on as an error") {
    auto context = lqtexec::EventLoopContext{};
    auto interface = startedInterface<int>();
    interface.reportException(TestException{});
    interface.reportFinished();
    auto caught = false;
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::from_future(context.get_scheduler(), interface.future()) |
                                 lexec::upon_error([&caught](std::exception_ptr const &error) noexcept {
                                     try {
                                         std::rethrow_exception(error);
                                     } catch (TestException const &) {
                                         caught = true;
                                     } catch (...) {
                                     }
                                 }),
                             recordInto(completion));
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(caught);
}
#endif

TEST_CASE("a stop request stops waiting for the future, which goes on") {
    auto context = lqtexec::EventLoopContext{};
    auto interface = startedInterface<int>();
    auto source = lexec::inplace_stop_source{};
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::from_future(context.get_scheduler(), interface.future()),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    processEventsFor(5ms);
    source.request_stop();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::stopped);
    CHECK_FALSE(interface.future().isCanceled());
    interface.reportFinished();
}

TEST_CASE("to_future of work completing at once") {
    auto scope = lexec::counting_scope{};
    auto future = lqtexec::to_future(lexec::just(7), scope.get_token());
    CHECK(future.isFinished());
    CHECK(future.result() == 7);
    auto none = lqtexec::to_future(lexec::just(), scope.get_token());
    CHECK(none.isFinished());
    CHECK_FALSE(none.isCanceled());
    REQUIRE(joinScope(scope));
}

TEST_CASE("to_future of work finishing later on the event loop") {
    auto context = lqtexec::EventLoopContext{};
    auto scope = lexec::counting_scope{};
    auto future = lqtexec::to_future(
        lqtexec::schedule_after(context.get_scheduler(), 10ms) | lexec::then([]() noexcept { return QStringLiteral("late"); }),
        scope.get_token());
    CHECK_FALSE(future.isFinished());
    REQUIRE(processEventsUntil([&] { return future.isFinished(); }));
    CHECK(future.result() == QStringLiteral("late"));
    REQUIRE(joinScope(scope));
}

TEST_CASE("to_future cancels the future when the work stops, the scope stops it, or the scope is closed") {
    auto context = lqtexec::EventLoopContext{};
    auto scope = lexec::counting_scope{};
    auto stopped = lqtexec::to_future(lexec::just_stopped(), scope.get_token());
    CHECK(stopped.isCanceled());

    auto pending = lqtexec::to_future(lqtexec::schedule_after(context.get_scheduler(), std::chrono::hours{1}) |
                                          lexec::then([]() noexcept { return 1; }),
                                      scope.get_token());
    CHECK_FALSE(pending.isFinished());
    scope.request_stop();
    CHECK(pending.isFinished());
    CHECK(pending.isCanceled());

    scope.close();
    auto rejected = lqtexec::to_future(lexec::just(1), scope.get_token());
    CHECK(rejected.isFinished());
    CHECK(rejected.isCanceled());
    REQUIRE(joinScope(scope));
}

#if LEXEC_HAS_EXCEPTIONS
TEST_CASE("to_future turns an error into the future's exception") {
    auto scope = lexec::counting_scope{};
    auto future = lqtexec::to_future(lexec::just(0) | lexec::then([](int) -> int { throw std::runtime_error{"boom"}; }),
                                     scope.get_token());
    CHECK(future.isFinished());
    CHECK_THROWS(future.waitForFinished());
    REQUIRE(joinScope(scope));
}
#endif
