#include <QCoreApplication>

#include <lqtexec/EventLoopContext.h>
#include <lqtexec/EventLoopThread.h>
#include <lqtexec/ExecWithScope.h>
#include <lqtexec/ObjectScope.h>
#include <lqtexec/WaitSignal.h>

#ifdef CONSUMER_HAS_NETWORK
#include <QNetworkAccessManager>
#include <lqtexec/NetworkClient.h>
#endif

#include <chrono>

int main(int argc, char **argv) {
    auto app = QCoreApplication{argc, argv};
    auto gui = lqtexec::EventLoopContext{};
    auto worker = lqtexec::EventLoopThread{};
    auto scope = lexec::counting_scope{};
    auto stop = lexec::inplace_stop_source{};
#ifdef CONSUMER_HAS_NETWORK
    auto manager = QNetworkAccessManager{};
    auto const client = lqtexec::NetworkClient{gui.get_scheduler(), manager};
    static_cast<void>(client);
#endif
    auto completed = false;
    lexec::spawn(lqtexec::schedule_after(worker.get_scheduler(), std::chrono::milliseconds{10}) |
                     lexec::continues_on(gui.get_scheduler()) | lexec::then([&]() noexcept {
                         completed = true;
                         stop.request_stop();
                     }) |
                     lexec::upon_error([&](auto const &) noexcept { stop.request_stop(); }),
                 scope.get_token());
    auto const code = lqtexec::exec_with_scope(app, scope, stop);
    return code == 0 and completed ? 0 : 1;
}
