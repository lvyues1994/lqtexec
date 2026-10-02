#include <lqtexec/ExecWithScope.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>
#include <QtGlobal>

#include <atomic>

namespace lqtexec {

namespace {

struct Shutdown {
    struct JoinReceiver {
        using receiver_concept = lexec::receiver_t;

        void set_value() && noexcept { shutdown->joined(); }

        Shutdown *shutdown;
    };

    Shutdown(QCoreApplication &app_, lexec::counting_scope &scope_)
        : app(app_), scope(scope_), join(lexec::connect(scope_.join(), JoinReceiver{this})) {}

    Shutdown(Shutdown &&) = delete;

    // Any thread, once.
    void begin() noexcept {
        if (started.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        scope.close();
        scope.request_stop();
        lexec::start(join);
    }

    // Any thread. QCoreApplication::exit is safe on the main thread only, and has no
    // effect before exec() runs, so the news is queued to the main thread.
    void joined() noexcept {
        QMetaObject::invokeMethod(&app, [this] { onJoinedOnMainThread(); }, Qt::QueuedConnection);
    }

    void onJoinedOnMainThread() {
        finished = true;
        if (drainLoop != nullptr) {
            drainLoop->quit();
        } else {
            QCoreApplication::exit(0);
        }
    }

    QCoreApplication &app;
    lexec::counting_scope &scope;
    std::atomic<bool> started{false};
    // The main thread only.
    bool finished = false;
    QEventLoop *drainLoop = nullptr;
    lexec::connect_result_t<decltype(std::declval<lexec::counting_scope &>().join()), JoinReceiver> join;
};

} // namespace

int exec_with_scope(QCoreApplication &app, lexec::counting_scope &scope, lexec::inplace_stop_source &stop) {
    Q_ASSERT_X(QThread::currentThread() == app.thread(), "lqtexec::exec_with_scope", "called off the main thread");
    auto shutdown = Shutdown{app, scope};
    auto const onStop = lexec::inplace_stop_callback{stop.get_token(), [&shutdown]() noexcept { shutdown.begin(); }};
    auto const code = QCoreApplication::exec();
    if (not shutdown.finished) {
        // QCoreApplication::exec() resets the quit request on returning, so a new loop runs.
        shutdown.begin();
        auto loop = QEventLoop{};
        shutdown.drainLoop = &loop;
        loop.exec();
    }
    return code;
}

} // namespace lqtexec
