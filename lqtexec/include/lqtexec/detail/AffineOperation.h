#pragma once

#include <lqtexec/EventLoopScheduler.h>
#include <lqtexec/detail/Lexec.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace lqtexec::detail {

// An operation driving a Qt object that lives on one context's thread. Starting, stop
// requests and completion all reach that thread through schedule operations embedded here,
// so nothing is allocated for them, and the operation completes on that thread only.
//
// Derived provides, all called on the context's thread:
//   void begin() noexcept;   creates the object and starts it; may finish() at once, and
//                            must not touch the operation after anything that can
//   void cancel() noexcept;  asks the begun object to stop; it finishes later, or at once
//   void abandon() noexcept; the context is going away: drops the object, then finishes
//   void deliver() noexcept; completes the receiver with the result finish() recorded
// and calls finish() or finishStopped() once, with its object released.
template <class Derived, class Rcvr>
struct AffineOperation {
    using operation_state_concept = lexec::operation_state_t;

    AffineOperation(EventLoopScheduler const scheduler, Rcvr &&rcvr_) noexcept(std::is_nothrow_move_constructible_v<Rcvr>)
        : rcvr(static_cast<Rcvr &&>(rcvr_)), startOp(lexec::connect(scheduler.schedule(), StartReceiver{this})),
          stopOp(lexec::connect(scheduler.schedule(), StopReceiver{this})) {}

    AffineOperation(AffineOperation &&) = delete;

    // The stop callback is registered first: a stop request inside the registration posts
    // its stop operation ahead of the start, which then finds the request and stops.
    void start() & noexcept {
        if constexpr (not lexec::is_unstoppable_token_v<Token>) {
            callback.emplace(lexec::get_stop_token(lexec::get_env(rcvr)), OnStop{this});
        }
        lexec::start(startOp);
    }

protected:
    // Whether a stop request reached the object: its ending is then a stop.
    bool cancelling() const noexcept { return cancelled; }

    void finish() noexcept {
        ending = Ending::result;
        tryComplete();
    }

    void finishStopped() noexcept {
        ending = Ending::stopped;
        tryComplete();
    }

    Rcvr rcvr;

private:
    using Token = lexec::stop_token_of_t<lexec::env_of_t<Rcvr>>;

    enum class Ending : std::uint8_t { none, result, stopped };

    Derived &derived() noexcept { return static_cast<Derived &>(*this); }

    struct StartReceiver {
        using receiver_concept = lexec::receiver_t;

        void set_value() && noexcept { op->onStart(); }
        // A stop request came first, or the context is going away.
        void set_stopped() && noexcept { op->finishStopped(); }

        // Spelled out: deducing it would need AffineOperation complete.
        lexec::env_of_t<Rcvr> get_env() const noexcept { return lexec::get_env(op->rcvr); }

        AffineOperation *op;
    };

    struct StopReceiver {
        using receiver_concept = lexec::receiver_t;

        void set_value() && noexcept { op->onStop(false); }
        void set_stopped() && noexcept { op->onStop(true); }

        AffineOperation *op;
    };

    struct OnStop {
        void operator()() noexcept {
            op->stopPosted.store(true, std::memory_order_release);
            lexec::start(op->stopOp);
        }

        AffineOperation *op;
    };

    void onStart() noexcept {
        begun = true;
        derived().begin();
    }

    // The last statements may complete the operation.
    void onStop(bool const discarded) noexcept {
        stopRan = true;
        if (ending != Ending::none) {
            complete();
        } else if (begun) {
            cancelled = true;
            if (discarded) {
                derived().abandon();
            } else {
                derived().cancel();
            }
        }
    }

    // Once the stop callback is gone its flag is final; a stop operation still queued then
    // completes the operation, so that no task of it outlives it.
    void tryComplete() noexcept {
        callback.reset();
        if (stopPosted.load(std::memory_order_acquire) and not stopRan) {
            return;
        }
        complete();
    }

    void complete() noexcept {
        if (ending == Ending::stopped) {
            lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
        } else {
            derived().deliver();
        }
    }

    std::optional<lexec::stop_callback_for_t<Token, OnStop>> callback;
    std::atomic<bool> stopPosted{false};
    // The context's thread only.
    bool begun = false;
    bool stopRan = false;
    bool cancelled = false;
    Ending ending = Ending::none;
    lexec::connect_result_t<ScheduleSender, StartReceiver> startOp;
    lexec::connect_result_t<ScheduleSender, StopReceiver> stopOp;
};

} // namespace lqtexec::detail
