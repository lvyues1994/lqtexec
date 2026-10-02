#include <lqtexec/EventLoopContext.h>
#include <lqtexec/EventLoopThread.h>
#include <lqtexec/Process.h>

#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QThread>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

using namespace lqtexec_test;

namespace {

using Clock = std::chrono::steady_clock;

static_assert(std::is_same_v<lexec::completion_signatures_of_t<lqtexec::detail::ProcessSender>,
                             lexec::completion_signatures<lexec::set_value_t(lqtexec::ProcessResult),
                                                          lexec::set_error_t(lqtexec::ProcessError),
                                                          lexec::set_stopped_t()>>);

lqtexec::ProcessSpec shell(QString const &script) {
    return lqtexec::ProcessSpec{QStringLiteral("/bin/sh"), {QStringLiteral("-c"), script}};
}

// Records the result or the error the process completed with.
struct Outcomes {
    void operator()(lqtexec::ProcessResult result_) noexcept { result = std::move(result_); }

    std::optional<lqtexec::ProcessResult> &result;
};

struct ErrorReceiver {
    using receiver_concept = lexec::receiver_t;

    void set_value(lqtexec::ProcessResult) && noexcept { completion->record(Outcome::value); }
    void set_error(lqtexec::ProcessError error_) && noexcept {
        *error = std::move(error_);
        completion->record(Outcome::error);
    }
    void set_stopped() && noexcept { completion->record(Outcome::stopped); }

    RecordingReceiver<> *completion;
    std::optional<lqtexec::ProcessError> *error;
};

} // namespace

TEST_CASE("run_process completes with the exit code and the output, on the scheduler's thread") {
    auto context = lqtexec::EventLoopContext{};
    auto result = std::optional<lqtexec::ProcessResult>{};
    auto completion = Completion{};
    auto op = lexec::connect(
        lqtexec::run_process(context.get_scheduler(), shell(QStringLiteral("printf hello; printf oops >&2; exit 3"))),
        recordInto(completion, Outcomes{result}));
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 3);
    CHECK(result->exitStatus == QProcess::NormalExit);
    CHECK(result->standardOutput == QByteArrayLiteral("hello"));
    CHECK(result->standardError == QByteArrayLiteral("oops"));
    CHECK(completion.thread == QThread::currentThread());
}

TEST_CASE("run_process writes the standard input") {
    auto context = lqtexec::EventLoopContext{};
    auto spec = lqtexec::ProcessSpec{QStringLiteral("/bin/cat")};
    spec.standardInput = QByteArrayLiteral("piped");
    auto result = std::optional<lqtexec::ProcessResult>{};
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::run_process(context.get_scheduler(), spec), recordInto(completion, Outcomes{result}));
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    REQUIRE(result.has_value());
    CHECK(result->standardOutput == QByteArrayLiteral("piped"));
}

TEST_CASE("a process that cannot start completes with a ProcessError") {
    auto context = lqtexec::EventLoopContext{};
    auto completion = Completion{};
    auto recorder = recordInto(completion);
    auto error = std::optional<lqtexec::ProcessError>{};
    auto op = lexec::connect(
        lqtexec::run_process(context.get_scheduler(), lqtexec::ProcessSpec{QStringLiteral("/nonexistent/lqtexec-test")}),
        ErrorReceiver{&recorder, &error});
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::error);
    REQUIRE(error.has_value());
    CHECK(error->error == QProcess::FailedToStart);
}

TEST_CASE("a stop request terminates the process") {
    auto context = lqtexec::EventLoopContext{};
    auto source = lexec::inplace_stop_source{};
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::run_process(context.get_scheduler(), lqtexec::ProcessSpec{QStringLiteral("/bin/sleep"), {QStringLiteral("30")}}),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    processEventsFor(50ms);
    auto const before = Clock::now();
    source.request_stop();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::stopped);
    CHECK(completion.thread == QThread::currentThread());
    CHECK(Clock::now() - before < 2s);
}

TEST_CASE("a process ignoring termination is killed after the grace period") {
    auto context = lqtexec::EventLoopContext{};
    auto source = lexec::inplace_stop_source{};
    auto spec = shell(QStringLiteral("trap '' TERM; exec sleep 30"));
    spec.terminateGrace = 100ms;
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::run_process(context.get_scheduler(), spec),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    processEventsFor(50ms);
    auto const before = Clock::now();
    source.request_stop();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::stopped);
    CHECK(Clock::now() - before >= 100ms);
}

TEST_CASE("a stop request made before start runs nothing") {
    auto context = lqtexec::EventLoopContext{};
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::run_process(context.get_scheduler(), shell(QStringLiteral("exit 0"))),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::stopped);
}

TEST_CASE("run_process started from another thread completes on the scheduler's thread") {
    auto context = lqtexec::EventLoopContext{};
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::run_process(context.get_scheduler(), shell(QStringLiteral("exit 0"))),
                             recordInto(completion));
    auto starter = std::thread{[&] { lexec::start(op); }};
    starter.join();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::value);
    CHECK(completion.thread == QThread::currentThread());
}

TEST_CASE("run_process on a worker thread, awaited with sync_wait") {
    auto worker = lqtexec::EventLoopThread{};
    auto result = lexec::sync_wait(lqtexec::run_process(worker.get_scheduler(), shell(QStringLiteral("printf done"))) |
                                   lexec::then([](lqtexec::ProcessResult r) noexcept {
                                       return std::pair{r.standardOutput, QThread::currentThread()};
                                   }) |
                                   lexec::upon_error([](lqtexec::ProcessError) noexcept {
                                       return std::pair{QByteArray{}, static_cast<QThread *>(nullptr)};
                                   }));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result).first == QByteArrayLiteral("done"));
    CHECK(std::get<0>(*result).second == worker.thread());
}

TEST_CASE("stop requests racing short processes: each completes exactly once") {
    auto worker = lqtexec::EventLoopThread{};
    for (auto round = 0; round < 40; ++round) {
        auto source = lexec::inplace_stop_source{};
        auto completion = Completion{};
        auto go = std::atomic<bool>{false};
        auto stopper = std::thread{[&] {
            while (not go.load(std::memory_order_acquire)) {
            }
            std::this_thread::sleep_for(std::chrono::microseconds{(round % 5) * 400});
            source.request_stop();
        }};
        go.store(true, std::memory_order_release);
        startSelfDestroying(lqtexec::run_process(worker.get_scheduler(), shell(QStringLiteral("exit 0"))), completion,
                            source.get_token());
        stopper.join();
        REQUIRE(processEventsUntil([&] { return completion.done(); }));
        processEventsFor(2ms);
        CHECK(completion.count == 1);
    }
}
