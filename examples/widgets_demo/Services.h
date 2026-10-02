#pragma once

#include <lqtexec/EventLoopScheduler.h>
#include <lqtexec/ThreadPoolScheduler.h>

#include <atomic>

namespace demo {

struct Journal;

// What is going on right now, for display; written from any thread.
struct Activity {
    std::atomic<int> jobsRunning{0};
    std::atomic<int> savesRunning{0};
    std::atomic<bool> heartbeatRunning{false};
};

// The collaborators main() hands to every window; all of them outlive the windows.
struct Services {
    lqtexec::EventLoopScheduler gui;
    lqtexec::ThreadPoolScheduler pool;
    lexec::counting_scope *appScope;
    lexec::inplace_stop_source *stop;
    Journal *journal;
    Activity *activity;
};

} // namespace demo
