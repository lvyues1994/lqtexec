#pragma once

#include <lqtexec/EventLoopScheduler.h>
#include <lqtexec/detail/EventLoopCore.h>

#include <memory>

class QThread;

namespace lqtexec {

// Runs work on one thread's Qt event loop. Create it after the QCoreApplication and destroy
// it before; destroy it on its thread, or once that thread's event loop has exited, and not
// from work it runs. Work still pending then completes with set_stopped.
struct EventLoopContext {
    // Serves the calling thread.
    EventLoopContext();
    // Serves `thread`, which must run an event loop, as QThread::run() does by default.
    explicit EventLoopContext(QThread *thread);
    EventLoopContext(EventLoopContext &&) = delete;
    ~EventLoopContext();

    EventLoopScheduler get_scheduler() noexcept { return EventLoopScheduler{core.get()}; }
    QThread *thread() const noexcept { return core->thread(); }

private:
    std::unique_ptr<detail::EventLoopCore> core;
};

} // namespace lqtexec
