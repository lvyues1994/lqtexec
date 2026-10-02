#include <lqtexec/SignalStop.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <thread>

#include <pthread.h>

namespace lqtexec {

namespace detail {

namespace {

// Without exceptions, a failure to set up is fatal.
[[noreturn]] void failSetup(char const *const what, int const error) {
#if LEXEC_HAS_EXCEPTIONS
    if (error == 0) {
        throw std::logic_error{what};
    }
    throw std::system_error{error, std::generic_category(), what};
#else
    std::fprintf(stderr, "%s (%d)\n", what, error);
    std::abort();
#endif
}

struct SignalOwnership {
    SignalOwnership() {
        if (active().exchange(true)) {
            failSetup("lqtexec: a SignalStop already exists", 0);
        }
    }

    SignalOwnership(SignalOwnership &&) = delete;

    ~SignalOwnership() { active().store(false); }

private:
    static std::atomic<bool> &active() {
        static auto value = std::atomic<bool>{false};
        return value;
    }
};

} // namespace

struct SignalStopState {
    explicit SignalStopState(lexec::inplace_stop_source &stop_) : stop(stop_) {
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        auto const result = pthread_sigmask(SIG_BLOCK, &signals, &previous);
        if (result != 0) {
            failSetup("lqtexec: blocking SIGINT and SIGTERM", result);
        }
#if LEXEC_HAS_EXCEPTIONS
        try {
            waiter = std::thread{[this] { wait(); }};
        } catch (...) {
            if (pthread_sigmask(SIG_SETMASK, &previous, nullptr) != 0) {
                std::terminate();
            }
            throw;
        }
#else
        waiter = std::thread{[this] { wait(); }};
#endif
    }

    SignalStopState(SignalStopState &&) = delete;

    ~SignalStopState() {
        if (owner != std::this_thread::get_id()) {
            std::terminate();
        }
        closing.store(true);
        waiter.join();
        if (pthread_sigmask(SIG_SETMASK, &previous, nullptr) != 0) {
            std::terminate();
        }
    }

    // Wakes every 50 ms to notice the destructor.
    void wait() noexcept {
        while (not closing.load()) {
            auto const timeout = timespec{0, 50'000'000};
            auto const signal = sigtimedwait(&signals, nullptr, &timeout);
            if (signal == SIGINT or signal == SIGTERM) {
                stop.request_stop();
            } else if (signal < 0 and errno != EAGAIN and errno != EINTR) {
                failure.store(errno);
                stop.request_stop();
                return;
            }
        }
    }

    SignalOwnership ownership;
    lexec::inplace_stop_source &stop;
    std::thread::id owner = std::this_thread::get_id();
    sigset_t signals{};
    sigset_t previous{};
    std::atomic<bool> closing{false};
    std::atomic<int> failure{0};
    std::thread waiter;
};

} // namespace detail

SignalStop::SignalStop(lexec::inplace_stop_source &stop) : state(std::make_unique<detail::SignalStopState>(stop)) {}

SignalStop::~SignalStop() = default;

std::error_code SignalStop::error() const noexcept { return {state->failure.load(), std::generic_category()}; }

} // namespace lqtexec
