#include <lqtexec/SignalChannel.h>

#include "support/Emitter.h"
#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QString>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

using namespace lqtexec_test;

namespace {

using IntChannel = lqtexec::SignalChannel<Emitter, int>;

static_assert(std::is_same_v<lexec::completion_signatures_of_t<decltype(std::declval<IntChannel &>().next())>,
                             lexec::completion_signatures<lexec::set_value_t(int), lexec::set_stopped_t()>>);
static_assert(
    std::is_same_v<lexec::completion_signatures_of_t<decltype(std::declval<lqtexec::SignalChannel<Emitter, int, QString const &> &>().next())>,
                   lexec::completion_signatures<lexec::set_value_t(int, QString), lexec::set_stopped_t()>>);

// Starts next() and returns the value it completed with, if it did so in start.
template <class Channel>
std::optional<int> nextNow(Channel &channel, Completion &completion) {
    auto value = std::optional<int>{};
    auto op = lexec::connect(channel.next(), recordInto(completion, [&value](int const v) noexcept { value = v; }));
    lexec::start(op);
    return value;
}

} // namespace

TEST_CASE("buffered emissions are delivered in order, in start") {
    auto emitter = Emitter{};
    auto channel = lqtexec::SignalChannel{&emitter, &Emitter::withInt};
    emit emitter.withInt(1);
    emit emitter.withInt(2);
    emit emitter.withInt(3);
    CHECK(channel.size() == 3);
    for (auto expected = 1; expected <= 3; ++expected) {
        auto completion = Completion{};
        CHECK(nextNow(channel, completion) == expected);
        CHECK(completion.outcome == Outcome::value);
    }
    CHECK(channel.size() == 0);
}

TEST_CASE("a waiting next() takes the emission on the emitting thread, as a copy") {
    auto emitter = Emitter{};
    auto channel = lqtexec::SignalChannel{&emitter, &Emitter::withTwo};
    auto completion = Completion{};
    auto seenText = QString{};
    auto op = lexec::connect(channel.next(), recordInto(completion, [&](int, QString text) noexcept { seenText = text; }));
    lexec::start(op);
    CHECK_FALSE(completion.done());
    auto emitterThread = static_cast<QThread *>(nullptr);
    auto other = std::thread{[&] {
        emitterThread = QThread::currentThread();
        emit emitter.withTwo(1, QStringLiteral("hi"));
    }};
    other.join();
    REQUIRE(completion.done());
    CHECK(seenText == QStringLiteral("hi"));
    CHECK(completion.thread == emitterThread);
    CHECK(channel.size() == 0);
}

TEST_CASE("a private signal's channel drops the tag") {
    auto emitter = Emitter{};
    auto channel = lqtexec::SignalChannel{&emitter, &Emitter::privateSignal};
    emitter.emitPrivate(9);
    auto completion = Completion{};
    CHECK(nextNow(channel, completion) == 9);
}

TEST_CASE("a full channel drops its oldest or its newest emission") {
    auto emitter = Emitter{};
    auto oldest = lqtexec::SignalChannel{&emitter, &Emitter::withInt, lqtexec::ChannelOptions{2, lqtexec::ChannelOverflow::dropOldest}};
    auto newest = lqtexec::SignalChannel{&emitter, &Emitter::withInt, lqtexec::ChannelOptions{2, lqtexec::ChannelOverflow::dropNewest}};
    for (auto i = 1; i <= 4; ++i) {
        emit emitter.withInt(i);
    }
    auto completions = std::vector<Completion>(4);
    CHECK(nextNow(oldest, completions[0]) == 3);
    CHECK(nextNow(oldest, completions[1]) == 4);
    CHECK(nextNow(newest, completions[2]) == 1);
    CHECK(nextNow(newest, completions[3]) == 2);
}

TEST_CASE("after the object's destruction, the buffer drains, then next() stops") {
    auto emitter = std::make_unique<Emitter>();
    auto channel = lqtexec::SignalChannel{emitter.get(), &Emitter::withInt};
    emit emitter->withInt(7);
    emitter.reset();
    auto first = Completion{};
    CHECK(nextNow(channel, first) == 7);
    auto second = Completion{};
    CHECK_FALSE(nextNow(channel, second).has_value());
    CHECK(second.outcome == Outcome::stopped);
}

TEST_CASE("the object's destruction stops a waiting next()") {
    auto emitter = std::make_unique<Emitter>();
    auto channel = lqtexec::SignalChannel{emitter.get(), &Emitter::withInt};
    auto completion = Completion{};
    auto op = lexec::connect(channel.next(), recordInto(completion));
    lexec::start(op);
    emitter.reset();
    REQUIRE(completion.done());
    CHECK(completion.outcome == Outcome::stopped);
}

TEST_CASE("destroying the channel stops waiting next() operations, and later ones") {
    auto emitter = Emitter{};
    auto completion = Completion{};
    auto later = Completion{};
    auto channel = std::make_unique<lqtexec::SignalChannel<Emitter, int>>(&emitter, &Emitter::withInt);
    auto sender = channel->next();
    auto op = lexec::connect(channel->next(), recordInto(completion));
    lexec::start(op);
    channel.reset();
    REQUIRE(completion.done());
    CHECK(completion.outcome == Outcome::stopped);
    emit emitter.withInt(1);
    auto laterOp = lexec::connect(sender, recordInto(later));
    lexec::start(laterOp);
    CHECK(later.outcome == Outcome::stopped);
}

TEST_CASE("a stop request takes a waiting next() out, and the next emission is kept") {
    auto emitter = Emitter{};
    auto channel = lqtexec::SignalChannel{&emitter, &Emitter::withInt};
    auto source = lexec::inplace_stop_source{};
    auto completion = Completion{};
    auto op = lexec::connect(channel.next(), recordInto(completion, IgnoreValues{}, source.get_token()));
    lexec::start(op);
    source.request_stop();
    REQUIRE(completion.done());
    CHECK(completion.outcome == Outcome::stopped);
    emit emitter.withInt(5);
    CHECK(channel.size() == 1);
    auto next = Completion{};
    CHECK(nextNow(channel, next) == 5);
}

TEST_CASE("a channel on a null object stops at once") {
    auto channel = lqtexec::SignalChannel{static_cast<Emitter *>(nullptr), &Emitter::withInt};
    auto completion = Completion{};
    CHECK_FALSE(nextNow(channel, completion).has_value());
    CHECK(completion.outcome == Outcome::stopped);
}

TEST_CASE("repeat_until over next() consumes a timer's ticks through the event loop") {
    auto timer = QTimer{};
    auto channel = lqtexec::SignalChannel{&timer, &QTimer::timeout};
    auto ticks = 0;
    auto completion = Completion{};
    auto op = lexec::connect(lexec::repeat_until(channel.next() | lexec::then([&ticks]() noexcept { return ++ticks == 3; })),
                             recordInto(completion));
    lexec::start(op);
    timer.start(2);
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(ticks == 3);
}

TEST_CASE("emissions from many threads, consumed with repeat_until: none is lost") {
    auto emitter = Emitter{};
    auto channel = lqtexec::SignalChannel{&emitter, &Emitter::withInt};
    constexpr auto threads = 4;
    constexpr auto perThread = 5000;
    auto count = 0;
    auto sum = 0LL;
    auto completion = Completion{};
    auto op = lexec::connect(lexec::repeat_until(channel.next() | lexec::then([&](int const v) noexcept {
                                                     sum += v;
                                                     return ++count == threads * perThread;
                                                 })),
                             recordInto(completion));
    lexec::start(op);
    auto emitters = std::vector<std::thread>{};
    for (auto t = 0; t < threads; ++t) {
        emitters.emplace_back([&] {
            for (auto i = 1; i <= perThread; ++i) {
                emit emitter.withInt(i);
            }
        });
    }
    for (auto &thread : emitters) {
        thread.join();
    }
    REQUIRE(completion.done());
    CHECK(count == threads * perThread);
    CHECK(sum == static_cast<long long>(threads) * perThread * (perThread + 1) / 2);
}
