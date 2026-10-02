#include <lqtexec/EventLoopContext.h>

#include <QBasicTimer>
#include <QCoreApplication>
#include <QEvent>
#include <QObject>
#include <QThread>
#include <QTimerEvent>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace lqtexec {

namespace detail {
namespace {

using Clock = std::chrono::steady_clock;

QEvent::Type wakeEventType() noexcept {
    static auto const type = static_cast<QEvent::Type>(QEvent::registerEventType());
    return type;
}

struct EventLoopCoreImpl;

// Lives on the context's thread and receives its wake events and timer.
struct Dispatcher final : QObject {
    explicit Dispatcher(EventLoopCoreImpl *const core_) : core(core_) {}

    bool event(QEvent *e) override;

    // A member, so that it stops before ~QObject: Qt does not unregister a QBasicTimer
    // whose target object goes away.
    QBasicTimer timer;

protected:
    void timerEvent(QTimerEvent *e) override;

private:
    EventLoopCoreImpl *core;
};

struct EventLoopCoreImpl final : EventLoopCore {
    explicit EventLoopCoreImpl(QThread *const thread_) : qthread(thread_), dispatcher(std::make_unique<Dispatcher>(this)) {
        Q_ASSERT(qthread != nullptr);
        if (qthread == QThread::currentThread()) {
            dispatcherThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
        } else {
            dispatcher->moveToThread(qthread);
        }
        // Emitted on the finishing thread before its event dispatcher goes, which is the
        // last chance to stop the timer from its own thread.
        QObject::connect(qthread, &QThread::finished, dispatcher.get(), [this] { onThreadFinished(); },
                         Qt::DirectConnection);
    }

    EventLoopCoreImpl(EventLoopCoreImpl &&) = delete;

    ~EventLoopCoreImpl() override {
        Q_ASSERT_X(isDispatcherThread() or not qthread->isRunning(), "lqtexec::EventLoopContext",
                   "destroyed off its thread while that thread's event loop may still run");
        Q_ASSERT_X(dispatchDepth == 0, "lqtexec::EventLoopContext", "destroyed by work it runs");
        dispatcher.reset();
        discardPending();
    }

    void post(Task *const task) noexcept override {
        auto wake = false;
        {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            enqueueLocked(task);
            wake = not std::exchange(wakePending, true);
        }
        if (wake) {
            postWake();
        }
    }

    bool arm(TimerNode *const timer) noexcept override {
        auto const now = Clock::now();
        auto const onThread = isDispatcherThread();
        auto wake = false;
        auto rearmHere = false;
        {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            if (timer->state == TimerState::cancelled) {
                return false;
            }
            if (timer->deadline <= now) {
                timer->state = TimerState::fired;
                enqueueLocked(timer);
                wake = not std::exchange(wakePending, true);
            } else {
                timer->state = TimerState::armed;
                timer->order = ++timerOrder;
                timers.push(timer);
                if (timers.top() == timer) {
                    if (onThread) {
                        rearmHere = true;
                    } else {
                        timersChanged = true;
                        wake = not std::exchange(wakePending, true);
                    }
                }
            }
        }
        // The timer may already have completed on another thread; it is not touched again.
        if (rearmHere) {
            rearm();
        } else if (wake) {
            postWake();
        }
        return true;
    }

    bool disarm(TimerNode *const timer) noexcept override {
        auto const lock = std::lock_guard<std::mutex>{mutex};
        switch (timer->state) {
        case TimerState::armed:
            timers.erase(timer);
            timer->state = TimerState::cancelled;
            return true;
        case TimerState::idle:
            timer->state = TimerState::cancelled;
            return false;
        case TimerState::fired:
        case TimerState::cancelled:
            break;
        }
        return false;
    }

    QThread *thread() const noexcept override { return qthread; }

    // Runs the tasks queued so far, one at a time from the shared FIFO: a nested event loop
    // started by one of them goes on with the rest, in order.
    void drain() noexcept {
        noteDispatcherThread();
        auto last = std::uint64_t{0};
        auto changed = false;
        {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            wakePending = false;
            last = lastSequence;
            changed = std::exchange(timersChanged, false);
        }
        if (changed) {
            rearm();
        }
        runUpTo(last);
    }

    void onTimer() noexcept {
        noteDispatcherThread();
        auto const now = Clock::now();
        auto last = std::uint64_t{0};
        {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            while (not timers.empty() and timers.top()->deadline <= now) {
                auto *const timer = timers.pop();
                timer->state = TimerState::fired;
                enqueueLocked(timer);
            }
            last = lastSequence;
        }
        // QBasicTimer repeats, and Qt may fire it early: decide its next start afresh.
        armedDeadline.reset();
        rearm();
        runUpTo(last);
    }

private:
    bool isDispatcherThread() const noexcept {
        return dispatcherThread.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

    void noteDispatcherThread() noexcept {
        dispatcherThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
    }

    void enqueueLocked(Task *const task) noexcept {
        task->next = nullptr;
        task->sequence = ++lastSequence;
        if (tail == nullptr) {
            head = task;
        } else {
            tail->next = task;
        }
        tail = task;
    }

    // Qt owns posted events and deletes them after delivery. Failing to allocate one is,
    // as throughout Qt, not recoverable.
    void postWake() noexcept { QCoreApplication::postEvent(dispatcher.get(), new QEvent(wakeEventType())); }

    // While tasks remain behind the one running, a wake event stays posted: a nested event
    // loop the task starts then runs them, as it would Qt's own posted events.
    void runUpTo(std::uint64_t const last) noexcept {
        ++dispatchDepth;
        while (true) {
            Task *task = nullptr;
            auto wake = false;
            {
                auto const lock = std::lock_guard<std::mutex>{mutex};
                if (head == nullptr or head->sequence > last) {
                    break;
                }
                task = head;
                head = task->next;
                if (head == nullptr) {
                    tail = nullptr;
                } else {
                    wake = not std::exchange(wakePending, true);
                }
            }
            if (wake) {
                postWake();
            }
            task->complete(task, TaskOutcome::run);
        }
        --dispatchDepth;
    }

    // On the context's thread only.
    void rearm() noexcept {
        auto next = std::optional<Clock::time_point>{};
        {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            if (not timers.empty()) {
                next = timers.top()->deadline;
            }
        }
        if (next == armedDeadline) {
            return;
        }
        armedDeadline = next;
        if (not next) {
            dispatcher->timer.stop();
            return;
        }
        auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(*next - Clock::now()).count();
        auto const msec = std::clamp<std::chrono::milliseconds::rep>(remaining, 0, INT_MAX);
        dispatcher->timer.start(static_cast<int>(msec), Qt::PreciseTimer, dispatcher.get());
    }

    void onThreadFinished() noexcept {
        dispatcher->timer.stop();
        armedDeadline.reset();
    }

    void discardPending() noexcept {
        Task *list = nullptr;
        {
            auto const lock = std::lock_guard<std::mutex>{mutex};
            while (not timers.empty()) {
                auto *const timer = timers.pop();
                timer->state = TimerState::cancelled;
                enqueueLocked(timer);
            }
            list = std::exchange(head, nullptr);
            tail = nullptr;
        }
        auto count = 0;
        for (auto const *task = list; task != nullptr; task = task->next) {
            ++count;
        }
        if (count != 0) {
            qWarning("lqtexec: EventLoopContext destroyed with %d pending operations; they complete with set_stopped",
                     count);
        }
        while (list != nullptr) {
            auto *const task = list;
            list = task->next;
            task->complete(task, TaskOutcome::discard);
        }
    }

    QThread *qthread;
    std::unique_ptr<Dispatcher> dispatcher;
    std::atomic<std::thread::id> dispatcherThread{};

    std::mutex mutex;
    Task *head = nullptr;
    Task *tail = nullptr;
    std::uint64_t lastSequence = 0;
    bool wakePending = false;
    bool timersChanged = false;
    PairingHeap<TimerNode, TimerNodeLess> timers;
    std::uint64_t timerOrder = 0;

    // The context's thread only.
    std::optional<Clock::time_point> armedDeadline;
    int dispatchDepth = 0;
};

bool Dispatcher::event(QEvent *const e) {
    if (e->type() == wakeEventType()) {
        core->drain();
        return true;
    }
    return QObject::event(e);
}

void Dispatcher::timerEvent(QTimerEvent *const e) {
    if (e->timerId() == timer.timerId()) {
        core->onTimer();
    } else {
        QObject::timerEvent(e);
    }
}

} // namespace

std::unique_ptr<EventLoopCore> makeEventLoopCore(QThread *const thread) {
    return std::make_unique<EventLoopCoreImpl>(thread);
}

} // namespace detail

EventLoopContext::EventLoopContext() : EventLoopContext(QThread::currentThread()) {}

EventLoopContext::EventLoopContext(QThread *const thread) : core(detail::makeEventLoopCore(thread)) {}

EventLoopContext::~EventLoopContext() = default;

} // namespace lqtexec
