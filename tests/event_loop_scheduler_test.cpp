#include <lqtexec/EventLoopContext.h>
#include <lqtexec/EventLoopThread.h>

#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef LQTEXEC_TEST_NO_ALLOCATION_COUNTER
#include "support/allocation_counter.h"
#endif

using namespace lqtexec_test;

namespace {

using Scheduler = lqtexec::EventLoopScheduler;

static_assert(lexec::is_scheduler_v<Scheduler>);
static_assert(std::is_same_v<lexec::completion_signatures_of_t<decltype(std::declval<Scheduler>().schedule())>,
                             lexec::completion_signatures<lexec::set_value_t(), lexec::set_stopped_t()>>);

// Appends its label to a log when it completes with a value.
struct Labelled {
    void operator()() const { log->push_back(label); }

    std::vector<std::string> *log;
    std::string label;
};

} // namespace

TEST_CASE("schedule completes later, on the context's thread") {
    auto context = lqtexec::EventLoopContext{};
    auto completion = Completion{};
    auto op = lexec::connect(lexec::schedule(context.get_scheduler()), recordInto(completion));
    lexec::start(op);
    CHECK_FALSE(completion.done());
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::value);
    CHECK(completion.thread == QThread::currentThread());
}

TEST_CASE("schedule onto a worker thread, started from another thread") {
    auto worker = lqtexec::EventLoopThread{QStringLiteral("worker")};
    auto completion = Completion{};
    auto op = lexec::connect(lexec::schedule(worker.get_scheduler()), recordInto(completion));
    auto starter = std::thread{[&] { lexec::start(op); }};
    starter.join();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.thread == worker.thread());
}

TEST_CASE("sync_wait on a worker's scheduler from the main thread") {
    auto worker = lqtexec::EventLoopThread{};
    auto result = lexec::sync_wait(lexec::schedule(worker.get_scheduler()) |
                                   lexec::then([] { return QThread::currentThread(); }));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == worker.thread());
}

TEST_CASE("tasks run in the order they started") {
    auto context = lqtexec::EventLoopContext{};
    auto log = std::vector<std::string>{};
    using Op = lexec::connect_result_t<lqtexec::detail::ScheduleSender, RecordingReceiver<Labelled>>;
    auto completions = std::array<Completion, 50>{};
    auto ops = std::vector<std::unique_ptr<Op>>(50);
    for (auto i = std::size_t{0}; i < ops.size(); ++i) {
        ops[i] = heapConnect(lexec::schedule(context.get_scheduler()),
                             recordInto(completions[i], Labelled{&log, std::to_string(i)}));
    }
    for (auto &op : ops) {
        lexec::start(*op);
    }
    REQUIRE(processEventsUntil([&] { return log.size() == ops.size(); }));
    for (auto i = std::size_t{0}; i < ops.size(); ++i) {
        CHECK(log[i] == std::to_string(i));
    }
}

TEST_CASE("a task started by a task waits behind Qt events posted in between") {
    auto context = lqtexec::EventLoopContext{};
    auto log = std::vector<std::string>{};
    auto second = Completion{};
    auto secondOp = lexec::connect(lexec::schedule(context.get_scheduler()), recordInto(second, Labelled{&log, "B"}));
    auto first = Completion{};
    auto firstOp = lexec::connect(lexec::schedule(context.get_scheduler()), recordInto(first, [&] {
                                      log.emplace_back("A");
                                      QMetaObject::invokeMethod(
                                          QCoreApplication::instance(), [&] { log.emplace_back("Qt"); },
                                          Qt::QueuedConnection);
                                      lexec::start(secondOp);
                                  }));
    lexec::start(firstOp);
    REQUIRE(processEventsUntil([&] { return second.done(); }));
    processEventsFor(10ms);
    CHECK(log == std::vector<std::string>{"A", "Qt", "B"});
}

TEST_CASE("a nested event loop runs the tasks queued behind the one that started it") {
    auto context = lqtexec::EventLoopContext{};
    auto second = Completion{};
    auto secondOp = lexec::connect(lexec::schedule(context.get_scheduler()), recordInto(second));
    auto secondRanInsideFirst = false;
    auto first = Completion{};
    auto firstOp = lexec::connect(lexec::schedule(context.get_scheduler()), recordInto(first, [&] {
                                      secondRanInsideFirst = processEventsUntil([&] { return second.done(); }, 2s);
                                  }));
    lexec::start(firstOp);
    lexec::start(secondOp);
    REQUIRE(processEventsUntil([&] { return first.done(); }));
    CHECK(secondRanInsideFirst);
}

TEST_CASE("a stop request before the task runs completes it with set_stopped") {
    auto context = lqtexec::EventLoopContext{};
    auto source = lexec::inplace_stop_source{};
    auto completion = Completion{};
    auto op = lexec::connect(lexec::schedule(context.get_scheduler()),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    source.request_stop();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::stopped);
    CHECK(completion.thread == QThread::currentThread());
}

TEST_CASE("the schedule sender reports its scheduler as where it completes") {
    auto context = lqtexec::EventLoopContext{};
    auto const scheduler = context.get_scheduler();
    auto const sndr = lexec::schedule(scheduler);
    CHECK(lexec::get_completion_scheduler<lexec::set_value_t>(lexec::get_env(sndr)) == scheduler);
    CHECK(lexec::get_completion_scheduler<lexec::set_stopped_t>(lexec::get_env(sndr)) == scheduler);
    CHECK(lexec::get_forward_progress_guarantee(scheduler) == lexec::forward_progress_guarantee::parallel);
    auto other = lqtexec::EventLoopContext{};
    CHECK(other.get_scheduler() != scheduler);
    CHECK(scheduler.thread() == QThread::currentThread());
}

TEST_CASE("destroying a context completes its pending work with set_stopped") {
    auto context = std::optional<lqtexec::EventLoopContext>{};
    context.emplace();
    auto completion = Completion{};
    auto timerCompletion = Completion{};
    auto op = lexec::connect(lexec::schedule(context->get_scheduler()), recordInto(completion));
    auto timerOp = lexec::connect(lqtexec::schedule_after(context->get_scheduler(), 1h), recordInto(timerCompletion));
    lexec::start(op);
    lexec::start(timerOp);
    CHECK_FALSE(completion.done());
    CHECK_FALSE(timerCompletion.done());
    context.reset();
    CHECK(completion.outcome == Outcome::stopped);
    CHECK(timerCompletion.outcome == Outcome::stopped);
    CHECK(completion.thread == QThread::currentThread());
}

#ifndef LQTEXEC_TEST_NO_ALLOCATION_COUNTER
TEST_CASE("starting a batch of schedules allocates only one wake event") {
    auto context = lqtexec::EventLoopContext{};
    using Op = lexec::connect_result_t<lqtexec::detail::ScheduleSender, RecordingReceiver<>>;
    auto completions = std::array<Completion, 64>{};
    auto ops = std::vector<std::unique_ptr<Op>>(64);
    auto runBatch = [&] {
        for (auto i = std::size_t{0}; i < ops.size(); ++i) {
            completions[i].count = 0;
            ops[i] = heapConnect(lexec::schedule(context.get_scheduler()), recordInto(completions[i]));
        }
        auto const before = allocationCount();
        for (auto &op : ops) {
            lexec::start(*op);
        }
        auto const allocations = allocationCount() - before;
        REQUIRE(processEventsUntil([&] { return completions.back().done(); }));
        // Lets the wake event posted for the batch's remaining tasks arrive.
        processEventsFor(10ms);
        return allocations;
    };
    runBatch();
    CHECK(runBatch() == 1);
}
#endif
