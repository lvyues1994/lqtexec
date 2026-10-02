#pragma once

#include <lqtexec/detail/Lexec.h>
#include <lqtexec/detail/SignalTraits.h>

#include <QMetaObject>
#include <QObject>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lqtexec {

enum class ChannelOverflow : std::uint8_t {
    // A full channel drops its oldest emission, keeping the latest: for progress and state.
    dropOldest,
    // A full channel drops the new emission.
    dropNewest,
};

struct ChannelOptions {
    // Zero: unbounded.
    std::size_t capacity = 0;
    ChannelOverflow overflow = ChannelOverflow::dropOldest;
};

namespace detail {

// Guarded by the channel's lock.
enum class WaiterState : std::uint8_t { idle, waiting, claimed, cancelled };

template <class... Values>
struct ChannelWaiter {
    using DeliverFn = void(ChannelWaiter *, std::tuple<Values...> &&) noexcept;
    using StopFn = void(ChannelWaiter *) noexcept;

    ChannelWaiter(DeliverFn *deliver_, StopFn *stop_) noexcept : deliver(deliver_), stop(stop_) {}

    ChannelWaiter *prev = nullptr;
    ChannelWaiter *next = nullptr;
    WaiterState status = WaiterState::idle;
    DeliverFn *deliver;
    StopFn *stop;
};

// Shared by the channel, the slot objects Qt holds, and next() operations: a slot may run
// after the channel has disconnected and gone.
template <class... Values>
struct ChannelState {
    using Item = std::tuple<Values...>;
    using Waiter = ChannelWaiter<Values...>;

    explicit ChannelState(ChannelOptions const options_) noexcept : options(options_) {}

    // Hands the emission to the first waiter, on the emitting thread, or buffers it.
    void push(Item &&item) noexcept {
        Waiter *waiter = nullptr;
        {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            if (ended) {
                return;
            }
            waiter = popWaiterLocked();
            if (waiter == nullptr) {
                bufferLocked(static_cast<Item &&>(item));
                return;
            }
        }
        waiter->deliver(waiter, static_cast<Item &&>(item));
    }

    // No more emissions: waiters complete with set_stopped, and so does every later next()
    // once the buffer, kept unless `discardBuffer`, is empty.
    void end(bool const discardBuffer) noexcept {
        Waiter *waiters = nullptr;
        {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            ended = true;
            if (discardBuffer) {
                buffer.clear();
            }
            waiters = std::exchange(head, nullptr);
            tail = nullptr;
            for (auto *waiter = waiters; waiter != nullptr; waiter = waiter->next) {
                waiter->status = WaiterState::claimed;
            }
        }
        while (waiters != nullptr) {
            auto *const waiter = waiters;
            waiters = waiter->next;
            waiter->stop(waiter);
        }
    }

    std::size_t size() noexcept {
        auto const lock = std::lock_guard<std::mutex>{mutex};
        return buffer.size();
    }

    std::mutex mutex;
    ChannelOptions options;
    std::deque<Item> buffer;
    Waiter *head = nullptr;
    Waiter *tail = nullptr;
    bool ended = false;

    void enqueueLocked(Waiter *const waiter) noexcept {
        waiter->status = WaiterState::waiting;
        waiter->prev = tail;
        waiter->next = nullptr;
        if (tail == nullptr) {
            head = waiter;
        } else {
            tail->next = waiter;
        }
        tail = waiter;
    }

    void unlinkLocked(Waiter *const waiter) noexcept {
        (waiter->prev == nullptr ? head : waiter->prev->next) = waiter->next;
        (waiter->next == nullptr ? tail : waiter->next->prev) = waiter->prev;
        waiter->prev = nullptr;
        waiter->next = nullptr;
    }

private:
    Waiter *popWaiterLocked() noexcept {
        auto *const waiter = head;
        if (waiter != nullptr) {
            unlinkLocked(waiter);
            waiter->status = WaiterState::claimed;
        }
        return waiter;
    }

    // Losing an emission that cannot be stored beats failing the emitter.
    void bufferLocked(Item &&item) noexcept {
        if (options.capacity != 0 and buffer.size() >= options.capacity) {
            if (options.overflow == ChannelOverflow::dropNewest) {
                return;
            }
            buffer.pop_front();
        }
#if LEXEC_HAS_EXCEPTIONS
        try {
            buffer.push_back(static_cast<Item &&>(item));
        } catch (...) {
        }
#else
        buffer.push_back(static_cast<Item &&>(item));
#endif
    }
};

template <class State, class Args>
struct ChannelSlot;

// Takes exactly the public parameters, so that Qt passes no private signal tag.
template <class State, class... Args>
struct ChannelSlot<State, TypeList<Args...>> {
    void operator()(Args... args) const {
#if LEXEC_HAS_EXCEPTIONS
        try {
            state->push(typename State::Item{args...});
        } catch (...) {
        }
#else
        state->push(typename State::Item{args...});
#endif
    }

    std::shared_ptr<State> state;
};

template <class State>
struct ChannelEndSlot {
    void operator()() const { state->end(false); }

    std::shared_ptr<State> state;
};

template <class Rcvr, class... Values>
struct NextOperation final : ChannelWaiter<Values...> {
    using operation_state_concept = lexec::operation_state_t;
    using State = ChannelState<Values...>;
    using Waiter = ChannelWaiter<Values...>;

    NextOperation(std::shared_ptr<State> state_, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : Waiter{&deliverItem, &deliverStopped}, state(static_cast<std::shared_ptr<State> &&>(state_)),
          rcvr(static_cast<Rcvr &&>(rcvr_)) {}

    NextOperation(NextOperation &&) = delete;

    // The stop callback is registered first: a stop request inside the registration only
    // marks the waiter cancelled, and is completed below.
    void start() & noexcept {
        if constexpr (not lexec::is_unstoppable_token_v<Token>) {
            auto const token = lexec::get_stop_token(lexec::get_env(rcvr));
            if (token.stop_requested()) {
                lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
                return;
            }
            callback.emplace(token, OnStop{this});
        }
        auto item = std::optional<typename State::Item>{};
        {
            auto const lock = std::lock_guard<std::mutex>{state->mutex};
            if (this->status != WaiterState::cancelled) {
                if (not state->buffer.empty()) {
                    item.emplace(static_cast<typename State::Item &&>(state->buffer.front()));
                    state->buffer.pop_front();
                } else if (not state->ended) {
                    state->enqueueLocked(this);
                    return;
                }
            }
        }
        if (item) {
            deliverItem(this, static_cast<typename State::Item &&>(*item));
        } else {
            deliverStopped(this);
        }
    }

private:
    using Token = lexec::stop_token_of_t<lexec::env_of_t<Rcvr>>;

    struct OnStop {
        void operator()() noexcept {
            auto &channel = *op->state;
            {
                auto const lock = std::lock_guard<std::mutex>{channel.mutex};
                switch (op->status) {
                case WaiterState::idle:
                    op->status = WaiterState::cancelled;
                    return;
                case WaiterState::waiting:
                    channel.unlinkLocked(op);
                    op->status = WaiterState::cancelled;
                    break;
                case WaiterState::claimed:
                case WaiterState::cancelled:
                    return;
                }
            }
            deliverStopped(op);
        }

        NextOperation *op;
    };

    static void deliverItem(Waiter *const waiter, typename State::Item &&item) noexcept {
        auto &self = *static_cast<NextOperation *>(waiter);
        self.callback.reset();
        std::apply(
            [&self](Values &...values) noexcept {
                lexec::set_value(static_cast<Rcvr &&>(self.rcvr), static_cast<Values &&>(values)...);
            },
            item);
    }

    static void deliverStopped(Waiter *const waiter) noexcept {
        auto &self = *static_cast<NextOperation *>(waiter);
        self.callback.reset();
        lexec::set_stopped(static_cast<Rcvr &&>(self.rcvr));
    }

    std::shared_ptr<State> state;
    Rcvr rcvr;
    std::optional<lexec::stop_callback_for_t<Token, OnStop>> callback;
};

template <class... Values>
struct NextSender {
    using sender_concept = lexec::sender_t;
    using completion_signatures = lexec::completion_signatures<lexec::set_value_t(Values...), lexec::set_stopped_t()>;

    template <class Rcvr>
    NextOperation<Rcvr, Values...> connect(Rcvr rcvr) const noexcept(std::is_nothrow_move_constructible_v<Rcvr>) {
        return {state, static_cast<Rcvr &&>(rcvr)};
    }

    std::shared_ptr<ChannelState<Values...>> state;
};

template <class Args>
struct ChannelTypes;

template <class... Args>
struct ChannelTypes<TypeList<Args...>> {
    using State = ChannelState<std::decay_t<Args>...>;
    using Next = NextSender<std::decay_t<Args>...>;
};

} // namespace detail

// Connects to a signal once and buffers its emissions, decayed copies of the arguments;
// next() delivers the oldest. Emissions keep coming while the consumer is busy, unlike
// repeating wait_signal. Destroying the channel disconnects it; waiting next() operations
// then complete with set_stopped. After the object's destruction, next() drains the buffer,
// then completes with set_stopped.
template <class Class, class... SignalArgs>
struct SignalChannel {
    using Signal = void (Class::*)(SignalArgs...);
    using Args = detail::PublicSignalArgs<SignalArgs...>;
    using State = typename detail::ChannelTypes<Args>::State;

    SignalChannel(Class const *const object, Signal const signal, ChannelOptions const options = {})
        : state(std::make_shared<State>(options)) {
        if (object == nullptr) {
            state->end(false);
            return;
        }
        signalConnection = QObject::connect(object, signal, object, detail::ChannelSlot<State, Args>{state},
                                            Qt::DirectConnection);
        destroyedConnection = QObject::connect(object, &QObject::destroyed, object,
                                               detail::ChannelEndSlot<State>{state}, Qt::DirectConnection);
    }

    SignalChannel(SignalChannel &&) = delete;

    ~SignalChannel() {
        QObject::disconnect(signalConnection);
        QObject::disconnect(destroyedConnection);
        state->end(true);
    }

    typename detail::ChannelTypes<Args>::Next next() const noexcept { return {state}; }

    // The emissions buffered right now.
    std::size_t size() const noexcept { return state->size(); }

private:
    std::shared_ptr<State> state;
    QMetaObject::Connection signalConnection;
    QMetaObject::Connection destroyedConnection;
};

template <class Object, class Class, class... SignalArgs>
SignalChannel(Object *, void (Class::*)(SignalArgs...)) -> SignalChannel<Class, SignalArgs...>;

template <class Object, class Class, class... SignalArgs>
SignalChannel(Object *, void (Class::*)(SignalArgs...), ChannelOptions) -> SignalChannel<Class, SignalArgs...>;

} // namespace lqtexec
