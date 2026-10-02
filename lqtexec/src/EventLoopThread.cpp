#include <lqtexec/EventLoopThread.h>

#include <QThread>

namespace lqtexec {

EventLoopThread::EventLoopThread(QString const &name) : qthread(std::make_unique<QThread>()) {
    if (not name.isEmpty()) {
        qthread->setObjectName(name);
    }
    context.emplace(qthread.get());
    qthread->start();
}

EventLoopThread::~EventLoopThread() {
    qthread->quit();
    qthread->wait();
    context.reset();
}

} // namespace lqtexec
