#include <lqtexec/EventLoopContext.h>
#include <lqtexec/EventLoopThread.h>
#include <lqtexec/ObjectScope.h>

#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>

using namespace lqtexec_test;

namespace {

struct Owner : QObject {
    Owner(lqtexec::EventLoopScheduler const scheduler, lexec::counting_scope &parent)
        : tasks{this, scheduler, parent.get_token()} {}

    int value = 0;
    lqtexec::ObjectScope tasks;
};

static_assert(lexec::is_scheduler_v<lqtexec::ObjectScheduler>);
static_assert(lexec::is_scope_token_v<lqtexec::ObjectScopeToken>);

} // namespace

TEST_CASE("the ObjectScheduler resumes on the owner's thread while the owner lives") {
    auto context = lqtexec::EventLoopContext{};
    auto parent = lexec::counting_scope{};
    {
        auto owner = Owner{context.get_scheduler(), parent};
        auto completion = Completion{};
        auto op = lexec::connect(lexec::schedule(owner.tasks.scheduler()), recordInto(completion));
        lexec::start(op);
        REQUIRE(processEventsUntil([&] { return completion.done(); }));
        CHECK(completion.outcome == Outcome::value);
        CHECK(completion.thread == QThread::currentThread());
    }
    REQUIRE(joinScope(parent));
}

TEST_CASE("a hop started before the owner dies completes with set_stopped") {
    auto context = lqtexec::EventLoopContext{};
    auto parent = lexec::counting_scope{};
    auto owner = std::make_unique<Owner>(context.get_scheduler(), parent);
    auto completion = Completion{};
    {
        auto op = lexec::connect(lexec::schedule(owner->tasks.scheduler()), recordInto(completion));
        lexec::start(op);
        owner.reset();
        REQUIRE(processEventsUntil([&] { return completion.done(); }));
        CHECK(completion.outcome == Outcome::stopped);
    }
    REQUIRE(joinScope(parent));
}

TEST_CASE("the owner's destruction stops its work and returns at once") {
    auto context = lqtexec::EventLoopContext{};
    auto parent = lexec::counting_scope{};
    auto owner = std::make_unique<Owner>(context.get_scheduler(), parent);
    auto stoppedCount = std::atomic<int>{0};
    auto finishedCount = std::atomic<int>{0};
    lexec::spawn(lqtexec::schedule_after(context.get_scheduler(), std::chrono::hours{1}) |
                     lexec::upon_stopped([&]() noexcept { ++stoppedCount; }),
                 owner->tasks.get_token());
    lexec::spawn(lexec::unstoppable(lqtexec::schedule_after(context.get_scheduler(), 30ms)) |
                     lexec::then([&]() noexcept { ++finishedCount; }),
                 owner->tasks.get_token());
    auto const before = std::chrono::steady_clock::now();
    owner.reset();
    CHECK(std::chrono::steady_clock::now() - before < 20ms);
    CHECK(stoppedCount == 1);
    CHECK(finishedCount == 0);
    REQUIRE(joinScope(parent));
    CHECK(finishedCount == 1);
}

TEST_CASE("then_on runs on the owner's thread while it lives, and is skipped once it is gone") {
    auto context = lqtexec::EventLoopContext{};
    auto worker = lqtexec::EventLoopThread{};
    auto parent = lexec::counting_scope{};
    auto owner = std::make_unique<Owner>(context.get_scheduler(), parent);
    auto *const raw = owner.get();
    auto ranOn = std::atomic<QThread *>{nullptr};
    lexec::spawn(lexec::schedule(worker.get_scheduler()) | lexec::then([]() noexcept { return 42; }) |
                     lqtexec::then_on(owner->tasks,
                                      [raw, &ranOn](int const v) noexcept {
                                          raw->value = v;
                                          ranOn = QThread::currentThread();
                                      }),
                 owner->tasks.get_token());
    REQUIRE(processEventsUntil([&] { return raw->value == 42; }));
    CHECK(ranOn == QThread::currentThread());

    auto skipped = std::atomic<bool>{false};
    auto started = std::atomic<bool>{false};
    auto release = lexec::inplace_stop_source{};
    lexec::spawn(lexec::unstoppable(lexec::schedule(worker.get_scheduler()) | lexec::then([&]() noexcept {
                                        started = true;
                                        while (not release.stop_requested()) {
                                            std::this_thread::yield();
                                        }
                                    })) |
                     lqtexec::then_on(owner->tasks, [raw]() noexcept { raw->value = -1; }) |
                     lexec::upon_stopped([&]() noexcept { skipped = true; }),
                 owner->tasks.get_token());
    REQUIRE(processEventsUntil([&] { return started.load(); }));
    owner.reset();
    release.request_stop();
    REQUIRE(joinScope(parent));
    CHECK(skipped);
}

TEST_CASE("then_on in a loop body, connected anew each round, moves the values to the function") {
    auto context = lqtexec::EventLoopContext{};
    auto worker = lqtexec::EventLoopThread{};
    auto parent = lexec::counting_scope{};
    {
        auto owner = Owner{context.get_scheduler(), parent};
        auto rounds = 0;
        auto completion = Completion{};
        auto op = lexec::connect(lexec::repeat_until(lexec::schedule(worker.get_scheduler()) |
                                                     lexec::then([]() noexcept { return std::make_unique<int>(7); }) |
                                                     lqtexec::then_on(owner.tasks,
                                                                      [&](std::unique_ptr<int> value) noexcept {
                                                                          owner.value += *value;
                                                                          return ++rounds == 3;
                                                                      })),
                                 recordInto(completion));
        lexec::start(op);
        REQUIRE(processEventsUntil([&] { return completion.done(); }));
        CHECK(completion.outcome == Outcome::value);
        CHECK(owner.value == 21);
    }
    REQUIRE(joinScope(parent));
}

TEST_CASE("a stop request on the parent tears the child down while the owner lives") {
    auto context = lqtexec::EventLoopContext{};
    auto parent = lexec::counting_scope{};
    auto owner = Owner{context.get_scheduler(), parent};
    auto stopped = std::atomic<bool>{false};
    lexec::spawn(lqtexec::schedule_after(context.get_scheduler(), std::chrono::hours{1}) |
                     lexec::upon_stopped([&]() noexcept { stopped = true; }),
                 owner.tasks.get_token());
    parent.request_stop();
    CHECK(stopped);
    REQUIRE(joinScope(parent));

    auto late = std::atomic<bool>{false};
    lexec::spawn(lexec::just() | lexec::then([&]() noexcept { late = true; }), owner.tasks.get_token());
    CHECK_FALSE(late);
    auto completion = Completion{};
    auto op = lexec::connect(lexec::schedule(owner.tasks.scheduler()), recordInto(completion));
    lexec::start(op);
    CHECK(completion.outcome == Outcome::stopped);
}

TEST_CASE("a child of a closed parent takes no work") {
    auto context = lqtexec::EventLoopContext{};
    auto parent = lexec::counting_scope{};
    parent.close();
    auto owner = Owner{context.get_scheduler(), parent};
    auto ran = std::atomic<bool>{false};
    lexec::spawn(lexec::just() | lexec::then([&]() noexcept { ran = true; }), owner.tasks.get_token());
    CHECK_FALSE(ran);
    REQUIRE(joinScope(parent));
}

TEST_CASE("work spawned through the token sees the ObjectScheduler as its scheduler") {
    auto context = lqtexec::EventLoopContext{};
    auto parent = lexec::counting_scope{};
    {
        auto owner = Owner{context.get_scheduler(), parent};
        auto matches = std::atomic<bool>{false};
        auto const expected = owner.tasks.scheduler();
        lexec::spawn(lexec::read_env(lexec::get_scheduler) |
                         lexec::then([&](lqtexec::ObjectScheduler const scheduler) noexcept { matches = scheduler == expected; }),
                     owner.tasks.get_token());
        CHECK(matches);
    }
    REQUIRE(joinScope(parent));
}
