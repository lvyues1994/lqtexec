#pragma once

#include <lqtexec/detail/SignalTraits.h>

#include <lqtexec/detail/Lexec.h>

#include <QMetaObject>
#include <QObject>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lqtexec {

namespace detail {

// Shared by an operation and the slot objects Qt holds for it. A slot may still run after
// QObject::disconnect returns, when another thread is emitting, and so after the operation
// is gone: the slots touch only this block, unless they claim the operation first.
struct SignalControl {
    // Claims the operation for an emission: only after start() finished, and only first.
    bool claimEmission() noexcept {
        auto expected = kArmed;
        return state.compare_exchange_strong(expected, static_cast<std::uint8_t>(kArmed | kClaimed),
                                             std::memory_order_acq_rel, std::memory_order_acquire);
    }

    // Claims the operation for a stop request or the object's destruction. Before start()
    // has finished, records the request for start() to complete instead.
    bool claimStop() noexcept {
        auto current = state.load(std::memory_order_acquire);
        while (true) {
            if ((current & kClaimed) != 0) {
                return false;
            }
            auto const armed = (current & kArmed) != 0;
            auto const next = static_cast<std::uint8_t>(current | (armed ? kClaimed : kPendingStop));
            if (state.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return armed;
            }
        }
    }

    // Ends start(): true if a stop request or the object's destruction came meanwhile, and
    // the caller is to complete with set_stopped.
    bool finishStart() noexcept {
        auto current = state.load(std::memory_order_acquire);
        while (true) {
            auto const pending = (current & kPendingStop) != 0;
            auto const next = static_cast<std::uint8_t>(current | kArmed | (pending ? kClaimed : 0));
            if (state.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
                return pending;
            }
        }
    }

private:
    static constexpr std::uint8_t kArmed = 1;
    static constexpr std::uint8_t kClaimed = 2;
    static constexpr std::uint8_t kPendingStop = 4;

    std::atomic<std::uint8_t> state{0};
};

template <class Op, class Args>
struct EmitSlot;

// Takes exactly the public parameters, so that Qt passes no private signal tag.
template <class Op, class... Args>
struct EmitSlot<Op, TypeList<Args...>> {
    void operator()(Args... args) const {
        if (control->claimEmission()) {
            op->emitted(static_cast<Args &&>(args)...);
        }
    }

    std::shared_ptr<SignalControl> control;
    Op *op;
};

template <class Op>
struct DestroyedSlot {
    void operator()() const {
        if (control->claimStop()) {
            op->finishStopped();
        }
    }

    std::shared_ptr<SignalControl> control;
    Op *op;
};

template <class Class, class Signal, class Args, class Rcvr>
struct SignalOperation {
    using operation_state_concept = lexec::operation_state_t;

    SignalOperation(Class const *const object_, Signal const signal_,
                    Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : object(object_), signal(signal_), rcvr(static_cast<Rcvr &&>(rcvr_)) {}

    SignalOperation(SignalOperation &&) = delete;

    void start() & noexcept {
        if (object == nullptr) {
            lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
            return;
        }
#if LEXEC_HAS_EXCEPTIONS
        try {
            subscribe();
        } catch (...) {
            disconnect();
            callback.reset();
            lexec::set_error(static_cast<Rcvr &&>(rcvr), std::current_exception());
            return;
        }
#else
        subscribe();
#endif
        if (control->finishStart()) {
            finishStopped();
        }
    }

    template <class... Vs>
    void emitted(Vs &&...vs) noexcept {
        disconnect();
        callback.reset();
        lexec::set_value(static_cast<Rcvr &&>(rcvr), static_cast<Vs &&>(vs)...);
    }

    void finishStopped() noexcept {
        disconnect();
        callback.reset();
        lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
    }

private:
    using Token = lexec::stop_token_of_t<lexec::env_of_t<Rcvr>>;

    struct OnStop {
        void operator()() noexcept {
            if (op->control->claimStop()) {
                op->finishStopped();
            }
        }

        SignalOperation *op;
    };

    // Before finishStart(), the slots and the stop callback only record what happens.
    void subscribe() {
        control = std::make_shared<SignalControl>();
        signalConnection = QObject::connect(object, signal, object, EmitSlot<SignalOperation, Args>{control, this},
                                            Qt::DirectConnection);
        destroyedConnection = QObject::connect(object, &QObject::destroyed, object,
                                               DestroyedSlot<SignalOperation>{control, this}, Qt::DirectConnection);
        if constexpr (not lexec::is_unstoppable_token_v<Token>) {
            callback.emplace(lexec::get_stop_token(lexec::get_env(rcvr)), OnStop{this});
        }
        // Qt has already warned that the signal does not exist.
        if (not signalConnection) {
            control->claimStop();
        }
    }

    void disconnect() noexcept {
        QObject::disconnect(signalConnection);
        QObject::disconnect(destroyedConnection);
    }

    Class const *object;
    Signal signal;
    Rcvr rcvr;
    std::shared_ptr<SignalControl> control;
    QMetaObject::Connection signalConnection;
    QMetaObject::Connection destroyedConnection;
    std::optional<lexec::stop_callback_for_t<Token, OnStop>> callback;
};

template <class Args>
struct SignalCompletions;

template <class... Args>
struct SignalCompletions<TypeList<Args...>> {
#if LEXEC_HAS_EXCEPTIONS
    using type = lexec::completion_signatures<lexec::set_value_t(Args...), lexec::set_error_t(std::exception_ptr),
                                              lexec::set_stopped_t()>;
#else
    using type = lexec::completion_signatures<lexec::set_value_t(Args...), lexec::set_stopped_t()>;
#endif
};

template <class Class, class... SignalArgs>
struct SignalSender {
    using Signal = void (Class::*)(SignalArgs...);
    using Args = PublicSignalArgs<SignalArgs...>;

    using sender_concept = lexec::sender_t;
    using completion_signatures = typename SignalCompletions<Args>::type;

    template <class Rcvr>
    SignalOperation<Class, Signal, Args, Rcvr> connect(Rcvr rcvr) const
        noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {object, signal, static_cast<Rcvr &&>(rcvr)};
    }

    Class const *object;
    Signal signal;
};

} // namespace detail

// Completes with the arguments of the next emission of `signal` by `object`, on the
// emitting thread and by reference as declared. Emissions after start() returns are seen;
// one concurrent with start() may be missed. Completes with set_stopped if `object` is
// null or destroyed first, or on a stop request.
template <class Object, class Class, class... SignalArgs>
detail::SignalSender<Class, SignalArgs...> wait_signal(Object *const object, void (Class::*const signal)(SignalArgs...)) {
    static_assert(std::is_base_of_v<QObject, Class>, "lqtexec::wait_signal: the signal must belong to a QObject");
    static_assert(std::is_base_of_v<Class, std::remove_const_t<Object>>,
                  "lqtexec::wait_signal: the object must have the signal");
    return {object, signal};
}

} // namespace lqtexec
