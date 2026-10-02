#include <lqtexec/ThreadPoolScheduler.h>

#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QCoreApplication>
#include <QThread>
#include <QThreadPool>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using namespace lqtexec_test;

namespace {

static_assert(lexec::is_scheduler_v<lqtexec::ThreadPoolScheduler>);

// Occupies the pool's only thread until released.
struct Blocker {
    explicit Blocker(QThreadPool &pool) : op(lexec::connect(lexec::schedule(lqtexec::ThreadPoolScheduler{&pool}),
                                                            recordInto(completion, Hold{this}))) {
        lexec::start(op);
        while (not running.load()) {
            std::this_thread::yield();
        }
    }

    ~Blocker() {
        release.store(true);
        while (not completion.done()) {
            std::this_thread::yield();
        }
    }

    struct Hold {
        void operator()() const noexcept {
            self->running.store(true);
            while (not self->release.load()) {
                std::this_thread::yield();
            }
        }

        Blocker *self;
    };

    std::atomic<bool> running{false};
    std::atomic<bool> release{false};
    Completion completion;
    lexec::connect_result_t<lqtexec::detail::ThreadPoolSender, RecordingReceiver<Hold>> op;
};

} // namespace

TEST_CASE("work scheduled on a thread pool runs on one of its threads") {
    auto pool = QThreadPool{};
    auto result = lexec::sync_wait(lexec::schedule(lqtexec::ThreadPoolScheduler{&pool}) |
                                   lexec::then([]() noexcept { return QThread::currentThread(); }));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) != QCoreApplication::instance()->thread());
}

TEST_CASE("a stop request takes back a queued runnable at once") {
    auto pool = QThreadPool{};
    pool.setMaxThreadCount(1);
    auto blocker = Blocker{pool};
    auto source = lexec::inplace_stop_source{};
    auto completion = Completion{};
    auto op = lexec::connect(lexec::schedule(lqtexec::ThreadPoolScheduler{&pool}),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    CHECK_FALSE(completion.done());
    source.request_stop();
    REQUIRE(completion.done());
    CHECK(completion.outcome == Outcome::stopped);
    CHECK(completion.thread == QThread::currentThread());
}

TEST_CASE("a stop request already made completes in start") {
    auto pool = QThreadPool{};
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto completion = Completion{};
    auto op = lexec::connect(lexec::schedule(lqtexec::ThreadPoolScheduler{&pool}),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    CHECK(completion.outcome == Outcome::stopped);
}

TEST_CASE("higher priority runnables run first") {
    auto pool = QThreadPool{};
    pool.setMaxThreadCount(1);
    auto mutex = std::mutex{};
    auto order = std::vector<int>{};
    auto low = Completion{};
    auto high = Completion{};
    auto record = [&](int const label) {
        return [&order, &mutex, label]() noexcept {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            order.push_back(label);
        };
    };
    auto lowOp = lexec::connect(lexec::schedule(lqtexec::ThreadPoolScheduler{&pool, 0}), recordInto(low, record(0)));
    auto highOp = lexec::connect(lexec::schedule(lqtexec::ThreadPoolScheduler{&pool, 5}), recordInto(high, record(5)));
    {
        auto blocker = Blocker{pool};
        lexec::start(lowOp);
        lexec::start(highOp);
    }
    pool.waitForDone();
    REQUIRE(low.done());
    REQUIRE(high.done());
    CHECK(order == std::vector<int>{5, 0});
}

TEST_CASE("thread pool schedulers compare by pool and priority, and report where they complete") {
    auto pool = QThreadPool{};
    auto const scheduler = lqtexec::ThreadPoolScheduler{&pool};
    CHECK(scheduler == lqtexec::ThreadPoolScheduler{&pool, 0});
    CHECK(scheduler != lqtexec::ThreadPoolScheduler{&pool, 1});
    CHECK(lexec::get_completion_scheduler<lexec::set_value_t>(lexec::get_env(lexec::schedule(scheduler))) == scheduler);
}

TEST_CASE("stop requests racing runnables: each completes exactly once") {
    auto pool = QThreadPool{};
    for (auto round = 0; round < 500; ++round) {
        auto source = lexec::inplace_stop_source{};
        auto completion = Completion{};
        auto go = std::atomic<bool>{false};
        auto stopper = std::thread{[&] {
            while (not go.load(std::memory_order_acquire)) {
            }
            source.request_stop();
        }};
        go.store(true, std::memory_order_release);
        startSelfDestroying(lexec::schedule(lqtexec::ThreadPoolScheduler{&pool}), completion, source.get_token());
        stopper.join();
        while (not completion.done()) {
            std::this_thread::yield();
        }
        pool.waitForDone();
        CHECK(completion.count == 1);
    }
}
