#pragma once

#include "Services.h"

#include <lqtexec/ObjectScope.h>
#include <lqtexec/SignalChannel.h>

#include <QMainWindow>
#include <QTimer>

#include <optional>

class QCloseEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace demo {

struct JobWindow;

// Opens job windows and shows what goes on. Closing it, or Quit, requests the application's
// stop; it stays open, showing the shutdown, until the work has drained.
struct MainWindow final : QMainWindow {
    explicit MainWindow(Services const &services_);
    ~MainWindow() override;

    JobWindow *openJobWindow();
    QPushButton *quitButton() const noexcept { return quit; }

    // Also runs every 100 ms.
    void refreshStatus();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    // Runs on the thread requesting the stop.
    struct OnShutdown {
        void operator()() noexcept;

        MainWindow *window;
    };

    void startHeartbeat();
    void showShutdown();

    Services services;
    int nextJobId = 1;
    int ticks = 0;
    QLabel *heartbeatLabel = nullptr;
    QLabel *activityLabel = nullptr;
    QLabel *stateLabel = nullptr;
    QPushButton *newJob = nullptr;
    QPushButton *quit = nullptr;
    QPlainTextEdit *log = nullptr;
    QTimer heartbeatTimer;
    QTimer refreshTimer;
    // Ticks pile up into one while the loop is busy.
    lqtexec::SignalChannelFor<&QTimer::timeout> heartbeat;
    std::optional<lexec::inplace_stop_callback<OnShutdown>> onShutdown;
    // Last, so that it goes first.
    lqtexec::ObjectScope tasks;
};

} // namespace demo
