#pragma once

#include <lqtexec/detail/PairingHeap.h>

#include <chrono>
#include <cstdint>
#include <memory>

class QThread;

namespace lqtexec::detail {

enum class TaskOutcome : std::uint8_t {
    // The context's thread runs the task.
    run,
    // The context is going away; the task completes with set_stopped on the destroying thread.
    discard,
};

// A node of a context's FIFO, embedded in the operation it completes; enqueueing never allocates.
struct Task {
    using CompleteFn = void(Task *, TaskOutcome) noexcept;

    explicit Task(CompleteFn *complete_) noexcept : complete(complete_) {}

    Task *next = nullptr;
    std::uint64_t sequence = 0;
    CompleteFn *complete;
};

// Guarded by the context's lock.
enum class TimerState : std::uint8_t { idle, armed, fired, cancelled };

// A timer waits in the context's deadline heap, then completes through the FIFO.
struct TimerNode : Task, PairingHeapHook {
    using Task::Task;

    std::chrono::steady_clock::time_point deadline{};
    // Orders timers with equal deadlines by when they started.
    std::uint64_t order = 0;
    TimerState state = TimerState::idle;
};

struct TimerNodeLess {
    bool operator()(TimerNode const &a, TimerNode const &b) const noexcept {
        return a.deadline < b.deadline or (a.deadline == b.deadline and a.order < b.order);
    }
};

// The execution resource behind EventLoopScheduler; EventLoopContext owns it.
struct EventLoopCore {
    virtual ~EventLoopCore() = default;

    // Appends a task to the FIFO; the context's thread completes it.
    virtual void post(Task *task) noexcept = 0;

    // Starts a timer whose deadline is set. False if a stop request cancelled it first,
    // in which case the caller completes it.
    virtual bool arm(TimerNode *timer) noexcept = 0;

    // A stop request. True if it took the timer out of the heap, in which case the caller
    // completes it; otherwise the timer completes through the FIFO, or arm() reports it.
    virtual bool disarm(TimerNode *timer) noexcept = 0;

    virtual QThread *thread() const noexcept = 0;
};

std::unique_ptr<EventLoopCore> makeEventLoopCore(QThread *thread);

} // namespace lqtexec::detail
