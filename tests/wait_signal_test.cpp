#include <lqtexec/EventLoopContext.h>
#include <lqtexec/WaitSignal.h>

#include "support/Emitter.h"
#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QString>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <exception>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

using namespace lqtexec_test;

namespace {

using lqtexec::detail::TypeList;

template <class Sndr>
using SignalArgsOf = typename Sndr::Args;

#if LEXEC_HAS_EXCEPTIONS
template <class... Args>
using SignalSigs = lexec::completion_signatures<lexec::set_value_t(Args...), lexec::set_error_t(std::exception_ptr),
                                                lexec::set_stopped_t()>;
#else
template <class... Args>
using SignalSigs = lexec::completion_signatures<lexec::set_value_t(Args...), lexec::set_stopped_t()>;
#endif

using TwoSender = decltype(lqtexec::wait_signal(std::declval<Emitter *>(), &Emitter::withTwo));
using TimeoutSender = decltype(lqtexec::wait_signal(std::declval<QTimer *>(), &QTimer::timeout));
using PrivateSender = decltype(lqtexec::wait_signal(std::declval<Emitter *>(), &Emitter::privateSignal));

static_assert(lexec::is_sender_v<TwoSender>);
static_assert(std::is_same_v<SignalArgsOf<TwoSender>, TypeList<int, QString const &>>);
static_assert(std::is_same_v<lexec::completion_signatures_of_t<TwoSender>, SignalSigs<int, QString const &>>);
static_assert(std::is_same_v<SignalArgsOf<TimeoutSender>, TypeList<>>);
static_assert(std::is_same_v<SignalArgsOf<PrivateSender>, TypeList<int>>);
static_assert(not lqtexec::detail::isPrivateSignalTag<QString>());
static_assert(not lqtexec::detail::isPrivateSignalTag<int>());

} // namespace

TEST_CASE("wait_signal completes inline with the emitted arguments, by reference") {
    auto emitter = Emitter{};
    auto completion = Completion{};
    auto seenValue = 0;
    auto const *seenText = static_cast<QString const *>(nullptr);
    auto op = lexec::connect(lqtexec::wait_signal(&emitter, &Emitter::withTwo),
                             recordInto(completion, [&](int const value, QString const &text) {
                                 seenValue = value;
                                 seenText = &text;
                             }));
    lexec::start(op);
    CHECK_FALSE(completion.done());
    auto const text = QStringLiteral("hello");
    emit emitter.withTwo(7, text);
    REQUIRE(completion.done());
    CHECK(completion.outcome == Outcome::value);
    CHECK(seenValue == 7);
    CHECK(seenText == &text);
    CHECK(completion.thread == QThread::currentThread());
    emit emitter.withTwo(8, text);
    CHECK(completion.count == 1);
}

TEST_CASE("wait_signal drops a private signal's tag") {
    auto emitter = Emitter{};
    auto completion = Completion{};
    auto seen = 0;
    auto op = lexec::connect(lqtexec::wait_signal(&emitter, &Emitter::privateSignal),
                             recordInto(completion, [&](int const value) { seen = value; }));
    lexec::start(op);
    emitter.emitPrivate(5);
    REQUIRE(completion.done());
    CHECK(seen == 5);
}

TEST_CASE("wait_signal on a QTimer's timeout, through the event loop") {
    auto timer = QTimer{};
    timer.setSingleShot(true);
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::wait_signal(&timer, &QTimer::timeout) | lexec::then([] { return 1; }),
                             recordInto(completion));
    lexec::start(op);
    timer.start(5);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::value);
}

TEST_CASE("an operation destroyed by its completion is not touched by later emissions") {
    auto emitter = Emitter{};
    auto completion = Completion{};
    startSelfDestroying(lqtexec::wait_signal(&emitter, &Emitter::withInt), completion);
    emit emitter.withInt(1);
    emit emitter.withInt(2);
    CHECK(completion.count == 1);
    CHECK(completion.outcome == Outcome::value);
}

TEST_CASE("the object's destruction completes wait_signal with set_stopped") {
    auto emitter = std::make_unique<Emitter>();
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::wait_signal(emitter.get(), &Emitter::noArgs), recordInto(completion));
    lexec::start(op);
    emitter.reset();
    REQUIRE(completion.done());
    CHECK(completion.outcome == Outcome::stopped);
}

TEST_CASE("a null object completes wait_signal with set_stopped") {
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::wait_signal(static_cast<Emitter *>(nullptr), &Emitter::noArgs),
                             recordInto(completion));
    lexec::start(op);
    CHECK(completion.outcome == Outcome::stopped);
}

TEST_CASE("a stop request completes wait_signal with set_stopped and disconnects") {
    auto emitter = Emitter{};
    auto source = lexec::inplace_stop_source{};
    auto completion = Completion{};
    startSelfDestroying(lqtexec::wait_signal(&emitter, &Emitter::noArgs), completion, source.get_token());
    source.request_stop();
    REQUIRE(completion.done());
    CHECK(completion.outcome == Outcome::stopped);
    emit emitter.noArgs();
    CHECK(completion.count == 1);
}

TEST_CASE("a stop request already made completes wait_signal in start") {
    auto emitter = Emitter{};
    auto source = lexec::inplace_stop_source{};
    source.request_stop();
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::wait_signal(&emitter, &Emitter::noArgs),
                             recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    CHECK(completion.outcome == Outcome::stopped);
}

TEST_CASE("wait_signal completes on the thread that emits") {
    auto emitter = Emitter{};
    auto completion = Completion{};
    auto op = lexec::connect(lqtexec::wait_signal(&emitter, &Emitter::withInt), recordInto(completion));
    lexec::start(op);
    auto emitterThread = static_cast<QThread *>(nullptr);
    auto other = std::thread{[&] {
        emitterThread = QThread::currentThread();
        emit emitter.withInt(3);
    }};
    other.join();
    REQUIRE(completion.done());
    CHECK(completion.thread == emitterThread);
}

TEST_CASE("wait_signal loses a when_any to a timer and stops listening") {
    auto context = lqtexec::EventLoopContext{};
    auto emitter = Emitter{};
    auto completion = Completion{};
    auto op = lexec::connect(lexec::when_any(lqtexec::wait_signal(&emitter, &Emitter::noArgs),
                                             lqtexec::schedule_after(context.get_scheduler(), 5ms)),
                             recordInto(completion));
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::value);
    emit emitter.noArgs();
    CHECK(completion.count == 1);
}

TEST_CASE("emissions from many threads complete each operation exactly once") {
    auto emitter = Emitter{};
    auto running = std::atomic<bool>{true};
    auto emitters = std::vector<std::thread>{};
    for (auto i = 0; i < 4; ++i) {
        emitters.emplace_back([&, i] {
            while (running.load(std::memory_order_relaxed)) {
                emit emitter.withInt(i);
            }
        });
    }
    for (auto round = 0; round < 500; ++round) {
        auto completion = Completion{};
        startSelfDestroying(lqtexec::wait_signal(&emitter, &Emitter::withInt), completion);
        while (not completion.done()) {
            std::this_thread::yield();
        }
        CHECK(completion.count == 1);
    }
    running = false;
    for (auto &thread : emitters) {
        thread.join();
    }
}

TEST_CASE("an emission racing a stop request: exactly one completion") {
    auto emitter = Emitter{};
    for (auto round = 0; round < 500; ++round) {
        auto source = lexec::inplace_stop_source{};
        auto completion = Completion{};
        startSelfDestroying(lqtexec::wait_signal(&emitter, &Emitter::noArgs), completion, source.get_token());
        auto go = std::atomic<bool>{false};
        auto stopper = std::thread{[&] {
            while (not go.load(std::memory_order_acquire)) {
            }
            source.request_stop();
        }};
        auto signaller = std::thread{[&] {
            while (not go.load(std::memory_order_acquire)) {
            }
            emit emitter.noArgs();
        }};
        go.store(true, std::memory_order_release);
        stopper.join();
        signaller.join();
        REQUIRE(completion.done());
        CHECK(completion.count == 1);
    }
}
