#pragma once

#include <lqtexec/detail/Lexec.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

namespace lqtexec_test {

using namespace std::chrono_literals;

// Processes the calling thread's events until `done()` holds or `timeout` passes; true if
// it holds. A 1 ms timer keeps the loop waking for conditions no event announces.
template <class Done>
bool processEventsUntil(Done &&done, std::chrono::milliseconds const timeout = 5s) {
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    auto ticker = QTimer{};
    ticker.start(1);
    while (not done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents);
    }
    return true;
}

// Processes events for `duration`, whatever happens.
inline void processEventsFor(std::chrono::milliseconds const duration) {
    processEventsUntil([] { return false; }, duration);
}

enum class Outcome : std::uint8_t { pending, value, error, stopped };

// What a receiver saw: written by whichever thread completes, read once `count` is set.
struct Completion {
    std::atomic<Outcome> outcome{Outcome::pending};
    std::atomic<QThread *> thread{nullptr};
    std::atomic<int> count{0};

    bool done() const noexcept { return count.load(std::memory_order_acquire) != 0; }
};

struct IgnoreValues {
    template <class... Vs>
    void operator()(Vs &&...) const noexcept {}
};

// Records how and where it completed; OnValue sees the values. Its environment carries
// `token`, a never-stopping one unless a test sets it.
template <class OnValue = IgnoreValues>
struct RecordingReceiver {
    using receiver_concept = lexec::receiver_t;

    template <class... Vs>
    void set_value(Vs &&...vs) && noexcept {
        onValue(static_cast<Vs &&>(vs)...);
        record(Outcome::value);
    }

    template <class E>
    void set_error(E &&) && noexcept {
        record(Outcome::error);
    }

    void set_stopped() && noexcept { record(Outcome::stopped); }

    auto get_env() const noexcept { return lexec::prop{lexec::get_stop_token, token}; }

    void record(Outcome const outcome) const noexcept {
        completion->thread.store(QThread::currentThread(), std::memory_order_relaxed);
        completion->outcome.store(outcome, std::memory_order_relaxed);
        completion->count.fetch_add(1, std::memory_order_release);
    }

    Completion *completion;
    OnValue onValue{};
    lexec::inplace_stop_token token{};
};

template <class OnValue = IgnoreValues>
RecordingReceiver<OnValue> recordInto(Completion &completion, OnValue onValue = {},
                                      lexec::inplace_stop_token const token = {}) {
    return {&completion, std::move(onValue), token};
}

// connect() onto the heap: an operation cannot move, and only a new-expression lets its
// prvalue initialize a heap object in place.
template <class Sndr, class Rcvr>
std::unique_ptr<lexec::connect_result_t<Sndr, Rcvr>> heapConnect(Sndr &&sndr, Rcvr rcvr) {
    using Op = lexec::connect_result_t<Sndr, Rcvr>;
    return std::unique_ptr<Op>{new Op(lexec::connect(static_cast<Sndr &&>(sndr), std::move(rcvr)))};
}

// An operation on the heap that its receiver deletes on completion, as spawn does: nothing
// may touch an operation once it has completed.
template <class Sndr>
struct SelfDestroying {
    struct Receiver {
        using receiver_concept = lexec::receiver_t;

        template <class... Vs>
        void set_value(Vs &&...) && noexcept {
            finish(Outcome::value);
        }

        template <class E>
        void set_error(E &&) && noexcept {
            finish(Outcome::error);
        }

        void set_stopped() && noexcept { finish(Outcome::stopped); }

        auto get_env() const noexcept { return lexec::prop{lexec::get_stop_token, token}; }

        void finish(Outcome const outcome) const noexcept {
            auto *const completion_ = completion;
            auto *const self_ = self;
            delete self_;
            completion_->outcome.store(outcome, std::memory_order_relaxed);
            completion_->count.fetch_add(1, std::memory_order_release);
        }

        SelfDestroying *self;
        Completion *completion;
        lexec::inplace_stop_token token;
    };

    static void start(Sndr sndr, Completion &completion, lexec::inplace_stop_token const token = {}) {
        lexec::start((new SelfDestroying{std::move(sndr), completion, token})->op);
    }

    SelfDestroying(Sndr sndr, Completion &completion, lexec::inplace_stop_token const token)
        : op(lexec::connect(std::move(sndr), Receiver{this, &completion, token})) {}

    lexec::connect_result_t<Sndr, Receiver> op;
};

template <class Sndr>
void startSelfDestroying(Sndr sndr, Completion &completion, lexec::inplace_stop_token const token = {}) {
    SelfDestroying<Sndr>::start(std::move(sndr), completion, token);
}

// Joins a counting scope by processing events; a scope must be joined before it goes.
inline bool joinScope(lexec::counting_scope &scope, std::chrono::milliseconds const timeout = 5s) {
    auto completion = Completion{};
    auto op = lexec::connect(scope.join(), recordInto(completion));
    lexec::start(op);
    return processEventsUntil([&] { return completion.done(); }, timeout);
}

} // namespace lqtexec_test
