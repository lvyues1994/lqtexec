#pragma once

#include <lqtexec/detail/Lexec.h>

#include <memory>
#include <system_error>

namespace lqtexec {

namespace detail {
struct SignalStopState;
}

// POSIX only: turns SIGINT and SIGTERM into a request on `stop`, from a thread of its own,
// without needing an event loop; repeated signals while the work drains are taken too.
// Construct it first in main, before QCoreApplication or any other thread, so that every
// thread inherits the blocked signals: threads made earlier keep their mask. One per process;
// destroy it on the constructing thread once the work has drained. `stop` must outlive it.
struct SignalStop {
    explicit SignalStop(lexec::inplace_stop_source &stop);
    SignalStop(SignalStop &&) = delete;
    ~SignalStop();

    // What ended the waiting early, if anything did; a stop is requested then too.
    std::error_code error() const noexcept;

private:
    std::unique_ptr<detail::SignalStopState> state;
};

} // namespace lqtexec
