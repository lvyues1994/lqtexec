#pragma once

#include <lqtexec/EventLoopScheduler.h>
#include <lqtexec/detail/AffineOperation.h>
#include <lqtexec/detail/Lexec.h>
#include <lqtexec/detail/SignalTraits.h>

#include <QFuture>
#include <QFutureInterface>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QObject>
#include <QtGlobal>

#if LEXEC_HAS_EXCEPTIONS
#include <QException>
#endif

#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lqtexec {

namespace detail {

template <class T>
struct FutureResultStorage {
    std::optional<T> value;
};

template <>
struct FutureResultStorage<void> {};

template <class T, class Rcvr>
struct FutureOperation final : AffineOperation<FutureOperation<T, Rcvr>, Rcvr> {
    using Base = AffineOperation<FutureOperation<T, Rcvr>, Rcvr>;

    FutureOperation(EventLoopScheduler const scheduler, QFuture<T> future_, Rcvr &&rcvr_)
        : Base(scheduler, static_cast<Rcvr &&>(rcvr_)), future(std::move(future_)) {}

private:
    friend Base;

    struct WatcherDeleter {
        void operator()(QFutureWatcher<T> *const target) const noexcept { target->deleteLater(); }
    };

    void begin() noexcept {
        watcher.reset(new QFutureWatcher<T>);
        connection = QObject::connect(watcher.get(), &QFutureWatcherBase::finished, watcher.get(),
                                      [this] { onFinished(); });
        watcher->setFuture(future);
    }

    // Qt cancels a future when it stores an exception, so the exception is looked for first.
    void onFinished() {
        release();
#if LEXEC_HAS_EXCEPTIONS
        try {
            future.waitForFinished();
        } catch (...) {
            error = std::current_exception();
            this->finish();
            return;
        }
#endif
        if (future.isCanceled()) {
            this->finishStopped();
            return;
        }
        if constexpr (not std::is_void_v<T>) {
            if (future.resultCount() == 0) {
                this->finishStopped();
                return;
            }
            storage.value.emplace(future.result());
        }
        this->finish();
    }

    // Stops waiting; the future itself is left alone.
    void cancel() noexcept {
        release();
        this->finishStopped();
    }

    void abandon() noexcept {
        QObject::disconnect(connection);
        delete watcher.release();
        this->finishStopped();
    }

    void deliver() noexcept {
        if (error) {
            lexec::set_error(static_cast<Rcvr &&>(this->rcvr), static_cast<std::exception_ptr &&>(error));
        } else if constexpr (std::is_void_v<T>) {
            lexec::set_value(static_cast<Rcvr &&>(this->rcvr));
        } else {
            lexec::set_value(static_cast<Rcvr &&>(this->rcvr), static_cast<T &&>(*storage.value));
        }
    }

    void release() noexcept {
        QObject::disconnect(connection);
        watcher.reset();
    }

    QFuture<T> future;
    std::unique_ptr<QFutureWatcher<T>, WatcherDeleter> watcher;
    QMetaObject::Connection connection;
    FutureResultStorage<T> storage;
    std::exception_ptr error;
};

template <class T>
struct FutureCompletions {
    using value = lexec::set_value_t(T);
};

template <>
struct FutureCompletions<void> {
    using value = lexec::set_value_t();
};

struct FutureAttrs {
    template <class Tag, class... Env,
              std::enable_if_t<std::is_same_v<Tag, lexec::set_value_t> or std::is_same_v<Tag, lexec::set_error_t> or
                                   std::is_same_v<Tag, lexec::set_stopped_t>,
                               int> = 0>
    EventLoopScheduler query(lexec::get_completion_scheduler_t<Tag>, Env const &...) const noexcept {
        return scheduler;
    }

    EventLoopScheduler scheduler;
};

template <class T>
struct FutureSender {
    using sender_concept = lexec::sender_t;
#if LEXEC_HAS_EXCEPTIONS
    using completion_signatures =
        lexec::completion_signatures<typename FutureCompletions<T>::value, lexec::set_error_t(std::exception_ptr),
                                     lexec::set_stopped_t()>;
#else
    using completion_signatures = lexec::completion_signatures<typename FutureCompletions<T>::value, lexec::set_stopped_t()>;
#endif

    template <class Rcvr>
    FutureOperation<T, Rcvr> connect(Rcvr rcvr) const {
        return {scheduler, future, static_cast<Rcvr &&>(rcvr)};
    }

    FutureAttrs get_env() const noexcept { return {scheduler}; }

    EventLoopScheduler scheduler;
    QFuture<T> future;
};

// Finishes the future once, with the work's outcome, or cancelled if the work never ran.
template <class T>
struct FutureReporter {
    FutureReporter() { future.reportStarted(); }

    FutureReporter(FutureReporter &&) = delete;

    ~FutureReporter() {
        if (not finished) {
            future.reportCanceled();
            future.reportFinished();
        }
    }

    template <class... Vs>
    void reportValue(Vs &&...vs) {
        if constexpr (not std::is_void_v<T>) {
            future.reportResult(T(static_cast<Vs &&>(vs)...));
        }
        finish();
    }

    template <class E>
    void reportError([[maybe_unused]] E &&e) noexcept {
#if LEXEC_HAS_EXCEPTIONS
        auto exception = std::exception_ptr{};
        if constexpr (std::is_same_v<std::decay_t<E>, std::exception_ptr>) {
            exception = e;
        } else {
            exception = std::make_exception_ptr(static_cast<E &&>(e));
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        future.reportException(exception);
#else
        future.reportException(QUnhandledException{});
#endif
#else
        future.reportCanceled();
#endif
        finish();
    }

    void reportStopped() noexcept {
        future.reportCanceled();
        finish();
    }

    QFutureInterface<T> future;

private:
    void finish() noexcept {
        finished = true;
        future.reportFinished();
    }

    bool finished = false;
};

template <class T>
struct ReportValue {
    template <class... Vs>
    void operator()(Vs &&...vs) const noexcept {
        reporter->reportValue(static_cast<Vs &&>(vs)...);
    }

    std::shared_ptr<FutureReporter<T>> reporter;
};

template <class T>
struct ReportError {
    template <class E>
    void operator()(E &&e) const noexcept {
        reporter->reportError(static_cast<E &&>(e));
    }

    std::shared_ptr<FutureReporter<T>> reporter;
};

template <class T>
struct ReportStopped {
    void operator()() const noexcept { reporter->reportStopped(); }

    std::shared_ptr<FutureReporter<T>> reporter;
};

template <class ValueTypes>
struct FutureValueOf {
    static_assert(sizeof(ValueTypes) == 0,
                  "lqtexec::to_future: the sender must have at most one value completion, of at most one value");
};

// A sender without a value completion can only cancel the future or fail it.
template <>
struct FutureValueOf<TypeList<>> {
    using type = void;
};

template <>
struct FutureValueOf<TypeList<TypeList<>>> {
    using type = void;
};

template <class Arg>
struct FutureValueOf<TypeList<TypeList<Arg>>> {
    using type = std::decay_t<Arg>;
};

template <class Sndr>
using FutureValueOfSender =
    typename FutureValueOf<lexec::value_types_of_t<Sndr, lexec::env<>, TypeList, TypeList>>::type;

} // namespace detail

// Completes with the future's result once it finishes, on the scheduler's thread: its first
// result, or none for QFuture<void>; set_error with its exception; set_stopped if it was
// cancelled. A stop request stops the waiting, not the future.
template <class T>
detail::FutureSender<T> from_future(EventLoopScheduler const scheduler, QFuture<T> future) {
    return {scheduler, std::move(future)};
}

// Spawns the sender into the scope and returns a future of its outcome: its one value, or a
// QFuture<void> for none; an error as the future's exception; cancellation when it stops or
// the scope does not take it. Cancelling the future does not stop the work; the scope's stop
// request does.
template <class Sndr, class Token, std::enable_if_t<lexec::is_sender_v<Sndr> and lexec::is_scope_token_v<Token>, int> = 0>
QFuture<detail::FutureValueOfSender<Sndr>> to_future(Sndr &&sndr, Token const token) {
    using T = detail::FutureValueOfSender<Sndr>;
    auto reporter = std::make_shared<detail::FutureReporter<T>>();
    auto future = reporter->future.future();
    lexec::spawn(static_cast<Sndr &&>(sndr) | lexec::then(detail::ReportValue<T>{reporter}) |
                     lexec::upon_error(detail::ReportError<T>{reporter}) |
                     lexec::upon_stopped(detail::ReportStopped<T>{reporter}),
                 token);
    return future;
}

} // namespace lqtexec
