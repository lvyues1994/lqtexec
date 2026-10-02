#pragma once

#include <lqtexec/detail/EventLoopCore.h>

#include <lqtexec/detail/Lexec.h>

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>

class QThread;

namespace lqtexec {

struct EventLoopScheduler;

namespace detail {

// When a timer completes: at a time point, or a duration after it starts.
struct TimerDeadline {
    using clock = std::chrono::steady_clock;

    static TimerDeadline after(clock::duration const delay_) noexcept { return TimerDeadline{{}, delay_, true}; }
    static TimerDeadline at(clock::time_point const point_) noexcept { return TimerDeadline{point_, {}, false}; }

    // Saturates instead of overflowing for durations such as duration::max().
    clock::time_point resolve() const noexcept {
        if (not relative) {
            return point;
        }
        auto const now = clock::now();
        if (delay > clock::time_point::max() - now) {
            return clock::time_point::max();
        }
        return now + delay;
    }

private:
    TimerDeadline(clock::time_point const point_, clock::duration const delay_, bool const relative_) noexcept
        : point(point_), delay(delay_), relative(relative_) {}

    clock::time_point point;
    clock::duration delay;
    bool relative;
};

template <class... Tags>
struct CompletesOn {
    template <class Tag, class... Env, std::enable_if_t<(std::is_same_v<Tag, Tags> or ...), int> = 0>
    EventLoopScheduler query(lexec::get_completion_scheduler_t<Tag>, Env const &...) const noexcept;

    EventLoopCore *core;
};

template <class Rcvr>
struct ScheduleOperation final : Task {
    using operation_state_concept = lexec::operation_state_t;

    ScheduleOperation(EventLoopCore *const core_, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : Task{&completeTask}, core(core_), rcvr(static_cast<Rcvr &&>(rcvr_)) {}

    ScheduleOperation(ScheduleOperation &&) = delete;

    void start() & noexcept { core->post(this); }

private:
    static void completeTask(Task *const task, TaskOutcome const outcome) noexcept {
        auto &self = *static_cast<ScheduleOperation *>(task);
        if (outcome == TaskOutcome::discard or lexec::get_stop_token(lexec::get_env(self.rcvr)).stop_requested()) {
            lexec::set_stopped(static_cast<Rcvr &&>(self.rcvr));
        } else {
            lexec::set_value(static_cast<Rcvr &&>(self.rcvr));
        }
    }

    EventLoopCore *core;
    Rcvr rcvr;
};

struct ScheduleSender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(), lexec::set_stopped_t()>;

    template <class Rcvr>
    ScheduleOperation<Rcvr> connect(Rcvr rcvr) const noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {core, static_cast<Rcvr &&>(rcvr)};
    }

    CompletesOn<lexec::set_value_t, lexec::set_stopped_t> get_env() const noexcept { return {core}; }

    EventLoopCore *core;
};

// A stop request takes the timer out of the heap and completes it at once, on the
// requesting thread: a timer losing a when_any must not hold the winner until its deadline.
template <class Rcvr>
struct TimerOperation final : TimerNode {
    using operation_state_concept = lexec::operation_state_t;

    TimerOperation(EventLoopCore *const core_, TimerDeadline const when_,
                   Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : TimerNode{&completeTask}, core(core_), when(when_), rcvr(static_cast<Rcvr &&>(rcvr_)) {}

    TimerOperation(TimerOperation &&) = delete;

    // The stop callback is registered before the timer is armed: a stop request arriving
    // inside the registration only marks the timer cancelled, and arm() reports it.
    void start() & noexcept {
        deadline = when.resolve();
        if constexpr (lexec::is_unstoppable_token_v<Token>) {
            core->arm(this);
        } else {
            auto const token = lexec::get_stop_token(lexec::get_env(rcvr));
            if (token.stop_requested()) {
                lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
                return;
            }
            callback.emplace(token, OnStop{this});
            if (not core->arm(this)) {
                finishStopped();
            }
        }
    }

private:
    using Token = lexec::stop_token_of_t<lexec::env_of_t<Rcvr>>;

    struct OnStop {
        void operator()() noexcept {
            if (op->core->disarm(op)) {
                op->finishStopped();
            }
        }

        TimerOperation *op;
    };

    void finishStopped() noexcept {
        callback.reset();
        lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
    }

    static void completeTask(Task *const task, TaskOutcome const outcome) noexcept {
        auto &self = *static_cast<TimerOperation *>(task);
        self.callback.reset();
        if (outcome == TaskOutcome::discard or lexec::get_stop_token(lexec::get_env(self.rcvr)).stop_requested()) {
            lexec::set_stopped(static_cast<Rcvr &&>(self.rcvr));
        } else {
            lexec::set_value(static_cast<Rcvr &&>(self.rcvr));
        }
    }

    EventLoopCore *core;
    TimerDeadline when;
    Rcvr rcvr;
    std::optional<lexec::stop_callback_for_t<Token, OnStop>> callback;
};

struct TimerSender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(), lexec::set_stopped_t()>;

    template <class Rcvr>
    TimerOperation<Rcvr> connect(Rcvr rcvr) const noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {core, deadline, static_cast<Rcvr &&>(rcvr)};
    }

    // A stopped timer may complete on the thread that requested the stop.
    CompletesOn<lexec::set_value_t> get_env() const noexcept { return {core}; }

    EventLoopCore *core;
    TimerDeadline deadline;
};

} // namespace detail

// A handle to an EventLoopContext: work scheduled on it runs on that context's thread.
struct EventLoopScheduler {
    using scheduler_concept = lexec::scheduler_t;

    explicit EventLoopScheduler(detail::EventLoopCore *const core_) noexcept : core(core_) {}

    detail::ScheduleSender schedule() const noexcept { return {core}; }

    // The duration counts from when the operation starts.
    detail::TimerSender schedule_after(std::chrono::steady_clock::duration const delay) const noexcept {
        return {core, detail::TimerDeadline::after(delay)};
    }

    detail::TimerSender schedule_at(std::chrono::steady_clock::time_point const point) const noexcept {
        return {core, detail::TimerDeadline::at(point)};
    }

    QThread *thread() const noexcept { return core->thread(); }

    static constexpr lexec::forward_progress_guarantee query(lexec::get_forward_progress_guarantee_t) noexcept {
        return lexec::forward_progress_guarantee::parallel;
    }

    friend bool operator==(EventLoopScheduler const a, EventLoopScheduler const b) noexcept { return a.core == b.core; }
    friend bool operator!=(EventLoopScheduler const a, EventLoopScheduler const b) noexcept { return a.core != b.core; }

private:
    detail::EventLoopCore *core;
};

inline detail::TimerSender schedule_after(EventLoopScheduler const &scheduler,
                                          std::chrono::steady_clock::duration const delay) noexcept {
    return scheduler.schedule_after(delay);
}

inline detail::TimerSender schedule_at(EventLoopScheduler const &scheduler,
                                       std::chrono::steady_clock::time_point const point) noexcept {
    return scheduler.schedule_at(point);
}

template <class... Tags>
template <class Tag, class... Env, std::enable_if_t<(std::is_same_v<Tag, Tags> or ...), int>>
EventLoopScheduler detail::CompletesOn<Tags...>::query(lexec::get_completion_scheduler_t<Tag>,
                                                       Env const &...) const noexcept {
    return EventLoopScheduler{core};
}

} // namespace lqtexec
