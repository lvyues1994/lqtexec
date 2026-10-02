#pragma once

#include <lqtexec/detail/Lexec.h>

#include <QRunnable>
#include <QThreadPool>

#include <optional>
#include <type_traits>
#include <utility>

namespace lqtexec {

struct ThreadPoolScheduler;

namespace detail {

// The operation is the runnable: the pool reads autoDelete() before run(), so the receiver
// may destroy it inside its completion.
template <class Rcvr>
struct ThreadPoolOperation final : QRunnable {
    using operation_state_concept = lexec::operation_state_t;

    ThreadPoolOperation(QThreadPool *const pool_, int const priority_,
                        Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : pool(pool_), priority(priority_), rcvr(static_cast<Rcvr &&>(rcvr_)) {
        setAutoDelete(false);
    }

    ThreadPoolOperation(ThreadPoolOperation &&) = delete;

    // The stop callback is registered before the pool gets the runnable: a stop request
    // in between finds nothing to take back, and run() sees it.
    void start() & noexcept {
        if constexpr (not lexec::is_unstoppable_token_v<Token>) {
            auto const token = lexec::get_stop_token(lexec::get_env(rcvr));
            if (token.stop_requested()) {
                lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
                return;
            }
            callback.emplace(token, OnStop{this});
        }
        pool->start(this, priority);
    }

    void run() override {
        callback.reset();
        if (lexec::get_stop_token(lexec::get_env(rcvr)).stop_requested()) {
            lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
        } else {
            lexec::set_value(static_cast<Rcvr &&>(rcvr));
        }
    }

private:
    using Token = lexec::stop_token_of_t<lexec::env_of_t<Rcvr>>;

    // Taking the runnable back means it never runs, and its completion falls to us.
    struct OnStop {
        void operator()() noexcept {
            if (op->pool->tryTake(op)) {
                op->callback.reset();
                lexec::set_stopped(static_cast<Rcvr &&>(op->rcvr));
            }
        }

        ThreadPoolOperation *op;
    };

    QThreadPool *pool;
    int priority;
    Rcvr rcvr;
    std::optional<lexec::stop_callback_for_t<Token, OnStop>> callback;
};

struct ThreadPoolAttrs {
    template <class... Env>
    ThreadPoolScheduler query(lexec::get_completion_scheduler_t<lexec::set_value_t>, Env const &...) const noexcept;

    QThreadPool *pool;
    int priority;
};

struct ThreadPoolSender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(), lexec::set_stopped_t()>;

    template <class Rcvr>
    ThreadPoolOperation<Rcvr> connect(Rcvr rcvr) const noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {pool, priority, static_cast<Rcvr &&>(rcvr)};
    }

    ThreadPoolAttrs get_env() const noexcept { return {pool, priority}; }

    QThreadPool *pool;
    int priority;
};

} // namespace detail

// Runs work on a QThreadPool, at a priority among its queued runnables. Shares the pool
// with QtConcurrent; a stop request takes back a runnable still queued.
struct ThreadPoolScheduler {
    using scheduler_concept = lexec::scheduler_t;

    explicit ThreadPoolScheduler(QThreadPool *const pool_, int const priority_ = 0) noexcept
        : pool(pool_), priority(priority_) {}

    detail::ThreadPoolSender schedule() const noexcept { return {pool, priority}; }

    static constexpr lexec::forward_progress_guarantee query(lexec::get_forward_progress_guarantee_t) noexcept {
        return lexec::forward_progress_guarantee::parallel;
    }

    friend bool operator==(ThreadPoolScheduler const a, ThreadPoolScheduler const b) noexcept {
        return a.pool == b.pool and a.priority == b.priority;
    }
    friend bool operator!=(ThreadPoolScheduler const a, ThreadPoolScheduler const b) noexcept { return not(a == b); }

private:
    QThreadPool *pool;
    int priority;
};

template <class... Env>
ThreadPoolScheduler detail::ThreadPoolAttrs::query(lexec::get_completion_scheduler_t<lexec::set_value_t>,
                                                   Env const &...) const noexcept {
    return ThreadPoolScheduler{pool, priority};
}

} // namespace lqtexec
