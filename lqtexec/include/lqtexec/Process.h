#pragma once

#include <lqtexec/EventLoopScheduler.h>
#include <lqtexec/detail/AffineOperation.h>
#include <lqtexec/detail/Lexec.h>

#include <QByteArray>
#include <QMetaObject>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lqtexec {

struct ProcessSpec {
    QString program;
    QStringList arguments = {};
    // Empty: the current directory.
    QString workingDirectory = {};
    // Empty: the current environment.
    std::optional<QProcessEnvironment> environment = {};
    // Written once the process has started, then the channel is closed.
    QByteArray standardInput = {};
    // On a stop request the process is terminated, and killed if still running after this.
    std::chrono::milliseconds terminateGrace = std::chrono::seconds{3};
};

// A process that ran to its end, whatever its exit code; a crash ends with CrashExit.
struct ProcessResult {
    int exitCode = 0;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    QByteArray standardOutput;
    QByteArray standardError;
};

// A process that could not start.
struct ProcessError {
    QProcess::ProcessError error = QProcess::UnknownError;
    QString message;
};

namespace detail {

template <class Rcvr>
struct ProcessOperation final : AffineOperation<ProcessOperation<Rcvr>, Rcvr> {
    using Base = AffineOperation<ProcessOperation<Rcvr>, Rcvr>;

    ProcessOperation(EventLoopScheduler const scheduler, ProcessSpec &&spec_, Rcvr &&rcvr_)
        : Base(scheduler, static_cast<Rcvr &&>(rcvr_)), spec(static_cast<ProcessSpec &&>(spec_)) {}

private:
    friend Base;

    struct ProcessDeleter {
        void operator()(QProcess *const target) const noexcept { target->deleteLater(); }
    };

    using Finished = void (QProcess::*)(int, QProcess::ExitStatus);

    // QProcess::start() may report a failure to start at once.
    void begin() noexcept {
        process.reset(new QProcess);
        if (not spec.workingDirectory.isEmpty()) {
            process->setWorkingDirectory(spec.workingDirectory);
        }
        if (spec.environment) {
            process->setProcessEnvironment(*spec.environment);
        }
        connections[0] = QObject::connect(process.get(), &QProcess::started, process.get(),
                                          [this] { onStarted(); });
        connections[1] = QObject::connect(process.get(), static_cast<Finished>(&QProcess::finished), process.get(),
                                          [this](int const code, QProcess::ExitStatus const status) {
                                              onFinished(code, status);
                                          });
        connections[2] = QObject::connect(process.get(), &QProcess::errorOccurred, process.get(),
                                          [this](QProcess::ProcessError const failure) { onError(failure); });
        process->start(spec.program, spec.arguments);
    }

    void onStarted() {
        if (not spec.standardInput.isEmpty()) {
            process->write(spec.standardInput);
        }
        process->closeWriteChannel();
    }

    void onFinished(int const code, QProcess::ExitStatus const status) {
        if (this->cancelling()) {
            release();
            this->finishStopped();
            return;
        }
        result.emplace(ProcessResult{code, status, process->readAllStandardOutput(), process->readAllStandardError()});
        release();
        this->finish();
    }

    // Other errors are followed by finished(), with CrashExit for a crash.
    void onError(QProcess::ProcessError const error_) {
        if (error_ != QProcess::FailedToStart) {
            return;
        }
        if (this->cancelling()) {
            release();
            this->finishStopped();
            return;
        }
        error.emplace(ProcessError{error_, process->errorString()});
        release();
        this->finish();
    }

    // The kill timer goes with the process.
    void cancel() noexcept {
        auto *const target = process.get();
        QTimer::singleShot(spec.terminateGrace, target, [target] { target->kill(); });
        target->terminate();
    }

    // QProcess's destructor kills the process and waits for it, which works without the
    // context's event loop.
    void abandon() noexcept {
        for (auto const &connection : connections) {
            QObject::disconnect(connection);
        }
        delete process.release();
        this->finishStopped();
    }

    void deliver() noexcept {
        if (result) {
            lexec::set_value(static_cast<Rcvr &&>(this->rcvr), static_cast<ProcessResult &&>(*result));
        } else {
            lexec::set_error(static_cast<Rcvr &&>(this->rcvr), static_cast<ProcessError &&>(*error));
        }
    }

    // Inside the process's own signal: it goes later, from the event loop.
    void release() noexcept {
        for (auto const &connection : connections) {
            QObject::disconnect(connection);
        }
        process.reset();
    }

    ProcessSpec spec;
    std::unique_ptr<QProcess, ProcessDeleter> process;
    QMetaObject::Connection connections[3];
    std::optional<ProcessResult> result;
    std::optional<ProcessError> error;
};

struct ProcessAttrs {
    template <class Tag, class... Env,
              std::enable_if_t<std::is_same_v<Tag, lexec::set_value_t> or std::is_same_v<Tag, lexec::set_error_t> or
                                   std::is_same_v<Tag, lexec::set_stopped_t>,
                               int> = 0>
    EventLoopScheduler query(lexec::get_completion_scheduler_t<Tag>, Env const &...) const noexcept {
        return scheduler;
    }

    EventLoopScheduler scheduler;
};

struct ProcessSender {
    using sender_concept = lexec::sender_t;
    using completion_signatures =
        lexec::completion_signatures<lexec::set_value_t(ProcessResult), lexec::set_error_t(ProcessError),
                                     lexec::set_stopped_t()>;

    template <class Rcvr>
    ProcessOperation<Rcvr> connect(Rcvr rcvr) const & {
        return {scheduler, ProcessSpec{spec}, static_cast<Rcvr &&>(rcvr)};
    }

    template <class Rcvr>
    ProcessOperation<Rcvr> connect(Rcvr rcvr) && {
        return {scheduler, static_cast<ProcessSpec &&>(spec), static_cast<Rcvr &&>(rcvr)};
    }

    ProcessAttrs get_env() const noexcept { return {scheduler}; }

    EventLoopScheduler scheduler;
    ProcessSpec spec;
};

} // namespace detail

// Runs a process from the scheduler's thread, whose event loop drives it, and completes
// there with its exit code and output once it ends. A stop request terminates it, then kills
// it after the grace period, and completes with set_stopped.
inline detail::ProcessSender run_process(EventLoopScheduler const scheduler, ProcessSpec spec) {
    return {scheduler, std::move(spec)};
}

} // namespace lqtexec
