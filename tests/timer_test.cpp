#include <lqtexec/EventLoopContext.h>
#include <lqtexec/EventLoopThread.h>

#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QThread>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace lqtexec_test;

namespace {

using Clock = std::chrono::steady_clock;

struct Labelled {
    void operator()() const { log->push_back(label); }

    std::vector<int> *log;
    int label;
};

using TimerOp = lexec::connect_result_t<lqtexec::detail::TimerSender, RecordingReceiver<Labelled>>;

} // namespace

TEST_CASE("schedule_after completes no earlier than its duration, on the context's thread") {
    auto context = lqtexec::EventLoopContext{};
    auto completion = Completion{};
    auto completedAt = std::optional<Clock::time_point>{};
    auto op = lexec::connect(lqtexec::schedule_after(context.get_scheduler(), 30ms),
                             recordInto(completion, [&] { completedAt = Clock::now(); }));
    auto const startedAt = Clock::now();
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::value);
    CHECK(completion.thread == QThread::currentThread());
    CHECK(*completedAt - startedAt >= 30ms);
}

TEST_CASE("timers complete in deadline order, equal deadlines in start order") {
    auto context = lqtexec::EventLoopContext{};
    auto const scheduler = context.get_scheduler();
    auto log = std::vector<int>{};
    auto completions = std::vector<Completion>(5);
    auto ops = std::vector<std::unique_ptr<TimerOp>>(5);
    auto const base = Clock::now() + 40ms;
    auto const deadlines = std::vector<Clock::time_point>{base + 20ms, base, base + 10ms, base, base + 10ms};
    for (auto i = std::size_t{0}; i < ops.size(); ++i) {
        ops[i] = heapConnect(lqtexec::schedule_at(scheduler, deadlines[i]),
                             recordInto(completions[i], Labelled{&log, static_cast<int>(i)}));
    }
    for (auto &op : ops) {
        lexec::start(*op);
    }
    REQUIRE(processEventsUntil([&] { return log.size() == ops.size(); }));
    CHECK(log == std::vector<int>{1, 3, 2, 4, 0});
}

TEST_CASE("a deadline already past completes through the FIFO, behind earlier tasks") {
    auto context = lqtexec::EventLoopContext{};
    auto const scheduler = context.get_scheduler();
    auto log = std::vector<int>{};
    auto first = Completion{};
    auto second = Completion{};
    auto scheduleOp = lexec::connect(lexec::schedule(scheduler), recordInto(first, Labelled{&log, 1}));
    auto timerOp = lexec::connect(lqtexec::schedule_at(scheduler, Clock::now() - 1s), recordInto(second, Labelled{&log, 2}));
    lexec::start(scheduleOp);
    lexec::start(timerOp);
    REQUIRE(processEventsUntil([&] { return second.done(); }));
    CHECK(log == std::vector<int>{1, 2});
}

TEST_CASE("a timer started on another thread fires on the context's thread") {
    auto context = lqtexec::EventLoopContext{};
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::schedule_after(context.get_scheduler(), 20ms), recordInto(completion));
    auto starter = std::thread{[&] { lexec::start(op); }};
    starter.join();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::value);
    CHECK(completion.thread == QThread::currentThread());
}

TEST_CASE("a timer on a worker thread") {
    auto worker = lqtexec::EventLoopThread{};
    auto const startedAt = Clock::now();
    auto result = lexec::sync_wait(lqtexec::schedule_after(worker.get_scheduler(), 20ms) |
                                   lexec::then([] { return QThread::currentThread(); }));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == worker.thread());
    CHECK(Clock::now() - startedAt >= 20ms);
}

TEST_CASE("a stop request completes a timer at once, on the requesting thread") {
    auto context = lqtexec::EventLoopContext{};
    auto source = lexec::inplace_stop_source{};
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::schedule_after(context.get_scheduler(), 1h),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    auto stopperThread = static_cast<QThread *>(nullptr);
    auto stopper = std::thread{[&] {
        stopperThread = QThread::currentThread();
        source.request_stop();
    }};
    stopper.join();
    REQUIRE(completion.done());
    CHECK(completion.outcome == Outcome::stopped);
    CHECK(completion.thread == stopperThread);
}

TEST_CASE("a timer losing a when_any does not hold the winner") {
    auto worker = lqtexec::EventLoopThread{};
    auto const startedAt = Clock::now();
    auto result = lexec::sync_wait(lexec::when_any(lqtexec::schedule_after(worker.get_scheduler(), 1h),
                                                   lqtexec::schedule_after(worker.get_scheduler(), 10ms)));
    CHECK(result.has_value());
    CHECK(Clock::now() - startedAt < 1s);
}

TEST_CASE("a stop request already made completes the timer in start") {
    auto context = lqtexec::EventLoopContext{};
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::schedule_after(context.get_scheduler(), 1h),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    CHECK(completion.done());
    CHECK(completion.outcome == Outcome::stopped);
}

TEST_CASE("many timers with random deadlines all complete, in deadline order") {
    auto context = lqtexec::EventLoopContext{};
    auto const scheduler = context.get_scheduler();
    auto rng = std::mt19937{7};
    auto delays = std::uniform_int_distribution<int>{0, 40};
    auto log = std::vector<int>{};
    auto completions = std::vector<Completion>(200);
    auto ops = std::vector<std::unique_ptr<TimerOp>>(200);
    auto deadlines = std::vector<Clock::time_point>(200);
    auto const base = Clock::now() + 10ms;
    for (auto i = std::size_t{0}; i < ops.size(); ++i) {
        deadlines[i] = base + std::chrono::milliseconds{delays(rng)};
        ops[i] = heapConnect(lqtexec::schedule_at(scheduler, deadlines[i]),
                             recordInto(completions[i], Labelled{&log, static_cast<int>(i)}));
    }
    for (auto &op : ops) {
        lexec::start(*op);
    }
    REQUIRE(processEventsUntil([&] { return log.size() == ops.size(); }));
    for (auto i = std::size_t{1}; i < log.size(); ++i) {
        CHECK(deadlines[static_cast<std::size_t>(log[i - 1])] <= deadlines[static_cast<std::size_t>(log[i])]);
    }
}

TEST_CASE("stop requests racing timers that start and fire: each completes exactly once") {
    auto worker = lqtexec::EventLoopThread{};
    auto const scheduler = worker.get_scheduler();
    for (auto round = 0; round < 300; ++round) {
        auto source = lexec::inplace_stop_source{};
        auto completion = Completion{};
        auto const delay = std::chrono::microseconds{round % 3 == 0 ? 0 : (round % 7) * 50};
        auto go = std::atomic<bool>{false};
        auto stopper = std::thread{[&] {
            while (not go.load(std::memory_order_acquire)) {
            }
            source.request_stop();
        }};
        go.store(true, std::memory_order_release);
        startSelfDestroying(lqtexec::schedule_after(scheduler, delay), completion, source.get_token());
        stopper.join();
        REQUIRE(processEventsUntil([&] { return completion.done(); }));
        processEventsFor(1ms);
        CHECK(completion.count == 1);
    }
}
