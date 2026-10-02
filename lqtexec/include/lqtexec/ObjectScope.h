#pragma once

#include <lqtexec/EventLoopScheduler.h>

#include <lqtexec/detail/Lexec.h>

#include <QObject>
#include <QThread>

#include <atomic>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lqtexec {

struct ObjectScope;
struct ObjectScheduler;

namespace detail {

// Told when an ObjectScope's scope has joined.
struct ScopeJoinListener {
    virtual void onScopeJoined() noexcept = 0;

protected:
    ~ScopeJoinListener() = default;
};

// What an ObjectScope's handle shares with its guardian and its join. It goes when the last
// of them lets go, which is never before the scope has joined.
struct ObjectScopeState {
    explicit ObjectScopeState(EventLoopScheduler const scheduler_) noexcept
        : scheduler(scheduler_), join(lexec::connect(scope.join(), JoinReceiver{this})) {}

    ObjectScopeState(ObjectScopeState &&) = delete;

    void addRef() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }

    void release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    // Starts the teardown once, from whichever comes first: the parent's stop request or
    // the handle's destruction. The join completes on the thread ending the last
    // association, so it needs no event loop.
    void beginJoin() noexcept {
        if (joining.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        addRef();
        scope.close();
        scope.request_stop();
        lexec::start(join);
    }

    struct JoinReceiver {
        using receiver_concept = lexec::receiver_t;

        void set_value() && noexcept { state->onJoined(); }

        ObjectScopeState *state;
    };

    void onJoined() noexcept {
        if (auto *const listener = guardian.exchange(nullptr, std::memory_order_acq_rel)) {
            listener->onScopeJoined();
        }
        release();
    }

    EventLoopScheduler scheduler;
    // Read and written on the owner's thread only.
    bool alive = true;
    std::atomic<int> refs{1};
    std::atomic<bool> joining{false};
    std::atomic<ScopeJoinListener *> guardian{nullptr};
    lexec::counting_scope scope;
    lexec::connect_result_t<decltype(std::declval<lexec::counting_scope &>().join()), JoinReceiver> join;
};

// A counted reference to an ObjectScopeState.
struct ObjectScopeStateRef {
    // The state's first reference; release() deletes it after the last.
    static ObjectScopeStateRef create(EventLoopScheduler const scheduler) {
        return ObjectScopeStateRef{new ObjectScopeState{scheduler}};
    }

    ObjectScopeStateRef(ObjectScopeStateRef const &other) noexcept : state(other.state) { state->addRef(); }
    ObjectScopeStateRef(ObjectScopeStateRef &&other) noexcept : state(std::exchange(other.state, nullptr)) {}
    ObjectScopeStateRef &operator=(ObjectScopeStateRef const &) = delete;
    ObjectScopeStateRef &operator=(ObjectScopeStateRef &&) = delete;
    ~ObjectScopeStateRef() {
        if (state != nullptr) {
            state->release();
        }
    }

    ObjectScopeState *operator->() const noexcept { return state; }
    ObjectScopeState *get() const noexcept { return state; }

private:
    explicit ObjectScopeStateRef(ObjectScopeState *const state_) noexcept : state(state_) {}

    ObjectScopeState *state;
};

// Spawned into the parent scope: a stop request there tears the child scope down, and the
// child's join completes it, so that the parent's join waits for the child.
template <class Rcvr>
struct GuardianOperation final : ScopeJoinListener {
    using operation_state_concept = lexec::operation_state_t;

    GuardianOperation(ObjectScopeStateRef &&state_, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : state(static_cast<ObjectScopeStateRef &&>(state_)), rcvr(static_cast<Rcvr &&>(rcvr_)) {}

    GuardianOperation(GuardianOperation &&) = delete;

    // spawn() destroys the guardian without starting it when the parent takes no more work;
    // then neither does the child.
    ~GuardianOperation() {
        if (not started) {
            state->scope.close();
        }
    }

    // A stop request arriving inside the callback's registration is left to the end of
    // start(): beginJoin() may complete this operation.
    void start() & noexcept {
        started = true;
        state->guardian.store(this, std::memory_order_release);
        if constexpr (not lexec::is_unstoppable_token_v<Token>) {
            callback.emplace(lexec::get_stop_token(lexec::get_env(rcvr)), OnStop{this});
            if (phase.exchange(kRegistered, std::memory_order_acq_rel) == kStopRequested) {
                state->beginJoin();
            }
        }
    }

    void onScopeJoined() noexcept override {
        callback.reset();
        lexec::set_value(static_cast<Rcvr &&>(rcvr));
    }

private:
    using Token = lexec::stop_token_of_t<lexec::env_of_t<Rcvr>>;

    static constexpr int kRegistering = 0;
    static constexpr int kRegistered = 1;
    static constexpr int kStopRequested = 2;

    struct OnStop {
        void operator()() noexcept {
            if (op->phase.exchange(kStopRequested, std::memory_order_acq_rel) == kRegistered) {
                op->state->beginJoin();
            }
        }

        GuardianOperation *op;
    };

    ObjectScopeStateRef state;
    Rcvr rcvr;
    bool started = false;
    std::atomic<int> phase{kRegistering};
    std::optional<lexec::stop_callback_for_t<Token, OnStop>> callback;
};

struct GuardianSender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t()>;

    template <class Rcvr>
    GuardianOperation<Rcvr> connect(Rcvr rcvr) && noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {static_cast<ObjectScopeStateRef &&>(state), static_cast<Rcvr &&>(rcvr)};
    }

    ObjectScopeStateRef state;
};

template <class Rcvr>
struct GuardedScheduleOperation {
    using operation_state_concept = lexec::operation_state_t;

    struct InnerReceiver {
        using receiver_concept = lexec::receiver_t;

        void set_value() && noexcept { op->resume(); }
        void set_stopped() && noexcept { lexec::set_stopped(static_cast<Rcvr &&>(op->rcvr)); }

        // Spelled out: deducing it would need GuardedScheduleOperation complete.
        lexec::env_of_t<Rcvr> get_env() const noexcept { return lexec::get_env(op->rcvr); }

        GuardedScheduleOperation *op;
    };

    GuardedScheduleOperation(ObjectScopeState *const state_, Rcvr &&rcvr_)
        : state(state_), rcvr(static_cast<Rcvr &&>(rcvr_)),
          inner(lexec::connect(state_->scheduler.schedule(), InnerReceiver{this})) {}

    GuardedScheduleOperation(GuardedScheduleOperation &&) = delete;

    // The association keeps the state alive until this operation goes; failing to make one
    // means the owner is gone or going.
    void start() & noexcept {
        association = state->scope.get_token().try_associate();
        if (not association) {
            lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
            return;
        }
        lexec::start(inner);
    }

private:
    void resume() noexcept {
        Q_ASSERT_X(QThread::currentThread() == state->scheduler.thread(), "lqtexec::ObjectScheduler",
                   "resumed off the owner's thread");
        if (state->alive) {
            lexec::set_value(static_cast<Rcvr &&>(rcvr));
        } else {
            lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
        }
    }

    ObjectScopeState *state;
    Rcvr rcvr;
    lexec::counting_scope::association association;
    lexec::connect_result_t<ScheduleSender, InnerReceiver> inner;
};

struct GuardedScheduleAttrs {
    template <class... Env>
    ObjectScheduler query(lexec::get_completion_scheduler_t<lexec::set_value_t>, Env const &...) const noexcept;

    ObjectScopeState *state;
};

struct GuardedScheduleSender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(), lexec::set_stopped_t()>;

    template <class Rcvr>
    GuardedScheduleOperation<Rcvr> connect(Rcvr rcvr) const {
        return {state, static_cast<Rcvr &&>(rcvr)};
    }

    GuardedScheduleAttrs get_env() const noexcept { return {state}; }

    ObjectScopeState *state;
};

} // namespace detail

// Schedules onto the owner's thread, and completes with set_stopped instead once the
// owner's ObjectScope is gone: a function right after the hop may use the owner. Use it
// while the ObjectScope lives, or from work spawned into it.
struct ObjectScheduler {
    using scheduler_concept = lexec::scheduler_t;

    explicit ObjectScheduler(detail::ObjectScopeState *const state_) noexcept : state(state_) {}

    detail::GuardedScheduleSender schedule() const noexcept { return {state}; }

    static constexpr lexec::forward_progress_guarantee query(lexec::get_forward_progress_guarantee_t) noexcept {
        return lexec::forward_progress_guarantee::parallel;
    }

    friend bool operator==(ObjectScheduler const a, ObjectScheduler const b) noexcept { return a.state == b.state; }
    friend bool operator!=(ObjectScheduler const a, ObjectScheduler const b) noexcept { return a.state != b.state; }

private:
    detail::ObjectScopeState *state;
};

// A scope token: wrapped senders see the scope's stop requests, and their environment's
// get_scheduler answers the ObjectScheduler. Valid while the ObjectScope lives.
struct ObjectScopeToken {
    template <class Sndr, std::enable_if_t<lexec::is_sender_v<Sndr>, int> = 0>
    auto wrap(Sndr &&sndr) const {
        return lexec::write_env(state->scope.get_token().wrap(static_cast<Sndr &&>(sndr)),
                                lexec::prop{lexec::get_scheduler, ObjectScheduler{state}});
    }

    lexec::counting_scope::association try_associate() const noexcept {
        return state->scope.get_token().try_associate();
    }

    detail::ObjectScopeState *state;
};

// Binds a counting scope to an object's lifetime without blocking its destructor. Make it
// the owner's last member, so that it goes first: its destruction stops the work spawned
// into it and starts the join, and returns at once; ObjectScheduler hops then complete with
// set_stopped. The scope is also a child of `parent`: a stop request there tears it down
// too, and the parent's join waits for it.
struct ObjectScope {
    template <class ParentToken, std::enable_if_t<lexec::is_scope_token_v<ParentToken>, int> = 0>
    ObjectScope(QObject const *const owner, EventLoopScheduler const scheduler, ParentToken const parent)
        : state(detail::ObjectScopeStateRef::create(scheduler)) {
        Q_ASSERT_X(owner != nullptr and owner->thread() == scheduler.thread(), "lqtexec::ObjectScope",
                   "the scheduler must run on the owner's thread");
        lexec::spawn(detail::GuardianSender{state}, parent);
    }

    ObjectScope(ObjectScope &&) = delete;

    ~ObjectScope() {
        Q_ASSERT_X(QThread::currentThread() == state->scheduler.thread(), "lqtexec::ObjectScope",
                   "destroyed off the owner's thread");
        state->alive = false;
        state->beginJoin();
    }

    ObjectScheduler scheduler() const noexcept { return ObjectScheduler{state.get()}; }
    ObjectScopeToken get_token() const noexcept { return ObjectScopeToken{state.get()}; }

private:
    detail::ObjectScopeStateRef state;
};

template <class Fn>
struct ThenOnClosure : lexec::sender_adaptor_closure<ThenOnClosure<Fn>> {
    template <class Sndr>
    auto operator()(Sndr &&sndr) && {
        return lexec::then(lexec::continues_on(static_cast<Sndr &&>(sndr), scheduler), static_cast<Fn &&>(fn));
    }

    template <class Sndr>
    auto operator()(Sndr &&sndr) const & {
        return lexec::then(lexec::continues_on(static_cast<Sndr &&>(sndr), scheduler), fn);
    }

    ObjectScheduler scheduler;
    Fn fn;
};

// continues_on(scope.scheduler()) | then(fn): fn runs on the owner's thread while the owner
// lives, and is skipped with set_stopped once it is gone.
template <class Sndr, class Fn, std::enable_if_t<lexec::is_sender_v<Sndr>, int> = 0>
auto then_on(Sndr &&sndr, ObjectScope const &scope, Fn &&fn) {
    return lexec::then(lexec::continues_on(static_cast<Sndr &&>(sndr), scope.scheduler()), static_cast<Fn &&>(fn));
}

template <class Fn>
ThenOnClosure<std::decay_t<Fn>> then_on(ObjectScope const &scope, Fn &&fn) {
    return {{}, scope.scheduler(), static_cast<Fn &&>(fn)};
}

template <class... Env>
ObjectScheduler detail::GuardedScheduleAttrs::query(lexec::get_completion_scheduler_t<lexec::set_value_t>,
                                                    Env const &...) const noexcept {
    return ObjectScheduler{state};
}

} // namespace lqtexec
