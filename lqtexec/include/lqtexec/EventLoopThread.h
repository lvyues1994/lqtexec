#pragma once

#include <lqtexec/EventLoopContext.h>
#include <lqtexec/EventLoopScheduler.h>

#include <QString>

#include <memory>
#include <optional>

class QThread;

namespace lqtexec {

// A thread of its own running a Qt event loop, with an EventLoopContext on it. Destroying
// it quits the event loop and waits for the thread; its work should be drained by then.
struct EventLoopThread {
    explicit EventLoopThread(QString const &name = {});
    EventLoopThread(EventLoopThread &&) = delete;
    ~EventLoopThread();

    EventLoopScheduler get_scheduler() noexcept { return context->get_scheduler(); }
    QThread *thread() const noexcept { return qthread.get(); }

private:
    std::unique_ptr<QThread> qthread;
    std::optional<EventLoopContext> context;
};

} // namespace lqtexec
