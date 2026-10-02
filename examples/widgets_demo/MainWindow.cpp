#include "MainWindow.h"

#include "JobWindow.h"
#include "Journal.h"

#include <QCloseEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace demo {

MainWindow::MainWindow(Services const &services_)
    : services(services_), heartbeat{&heartbeatTimer, &QTimer::timeout, lqtexec::ChannelOptions{1}},
      tasks{this, services_.gui, services_.appScope->get_token()} {
    setWindowTitle(QStringLiteral("lqtexec: ObjectScope and closing windows"));
    resize(720, 520);

    newJob = new QPushButton{QStringLiteral("New job window")};
    quit = new QPushButton{QStringLiteral("Quit")};
    heartbeatLabel = new QLabel;
    activityLabel = new QLabel;
    stateLabel = new QLabel{QStringLiteral("Running.")};
    stateLabel->setStyleSheet(QStringLiteral("font-weight: bold;"));
    log = new QPlainTextEdit;
    log->setReadOnly(true);
    log->setStyleSheet(QStringLiteral("font-family: monospace;"));

    auto *const buttons = new QHBoxLayout;
    buttons->addWidget(newJob);
    buttons->addStretch();
    buttons->addWidget(quit);

    auto *const status = new QGroupBox{QStringLiteral("Status")};
    auto *const statusLayout = new QVBoxLayout{status};
    statusLayout->addWidget(stateLabel);
    statusLayout->addWidget(activityLabel);
    statusLayout->addWidget(heartbeatLabel);

    auto *const journalBox = new QGroupBox{QStringLiteral("Journal")};
    auto *const journalLayout = new QVBoxLayout{journalBox};
    journalLayout->addWidget(log);

    auto *const central = new QWidget;
    auto *const layout = new QVBoxLayout{central};
    layout->addLayout(buttons);
    layout->addWidget(status);
    layout->addWidget(journalBox, 1);
    setCentralWidget(central);

    services.journal->attach(log);
    QObject::connect(newJob, &QPushButton::clicked, this, [this] { openJobWindow(); });
    QObject::connect(quit, &QPushButton::clicked, this, [this] { close(); });
    QObject::connect(&refreshTimer, &QTimer::timeout, this, [this] { refreshStatus(); });
    refreshTimer.start(100);
    onShutdown.emplace(services.stop->get_token(), OnShutdown{this});
    startHeartbeat();
    refreshStatus();
}

MainWindow::~MainWindow() { services.journal->attach(nullptr); }

JobWindow *MainWindow::openJobWindow() {
    auto *const window = new JobWindow{services, nextJobId++, this};
    window->move(pos() + QPoint{40 + 30 * window->id(), 60 + 30 * window->id()});
    window->show();
    return window;
}

// The window stays until main() destroys it, after the work has drained.
void MainWindow::closeEvent(QCloseEvent *const event) {
    event->ignore();
    if (not services.stop->stop_requested()) {
        services.journal->write(QStringLiteral("main window: close requested"));
        services.stop->request_stop();
    }
}

void MainWindow::OnShutdown::operator()() noexcept {
    QMetaObject::invokeMethod(
        window, [target = window] { target->showShutdown(); }, Qt::QueuedConnection);
}

// A loop over the timer's ticks that never ends by itself: the application's stop request
// tears this window's scope down, which stops it.
void MainWindow::startHeartbeat() {
    auto *const journal = services.journal;
    auto *const activity = services.activity;
    activity->heartbeatRunning = true;
    lexec::spawn(lexec::repeat(heartbeat.next() | lqtexec::then_on(tasks, [this]() noexcept { ++ticks; })) |
                     lexec::upon_stopped([journal, activity]() noexcept {
                         activity->heartbeatRunning = false;
                         journal->write(QStringLiteral("main window: heartbeat loop stopped"));
                     }) |
                     lexec::upon_error([journal, activity](auto const &) noexcept {
                         activity->heartbeatRunning = false;
                         journal->write(QStringLiteral("main window: heartbeat failed"));
                     }),
                 tasks.get_token());
    heartbeatTimer.start(250);
}

void MainWindow::refreshStatus() {
    auto const jobs = services.activity->jobsRunning.load();
    auto const saves = services.activity->savesRunning.load();
    activityLabel->setText(QStringLiteral("Jobs running: %1      Saves in progress: %2").arg(jobs).arg(saves));
    heartbeatLabel->setText(services.activity->heartbeatRunning
                                ? QStringLiteral("Scoped heartbeat (SignalChannel in an ObjectScope): tick %1").arg(ticks)
                                : QStringLiteral("Scoped heartbeat: stopped with its scope after tick %1").arg(ticks));
    if (services.stop->stop_requested()) {
        stateLabel->setText(saves == 0 ? QStringLiteral("Shutting down: drained, exiting.")
                                       : QStringLiteral("Shutting down: work stopped, waiting for %1 save(s).")
                                             .arg(saves));
    }
}

void MainWindow::showShutdown() {
    services.journal->write(QStringLiteral("app: shutdown requested, the scopes stop their work"));
    newJob->setEnabled(false);
    quit->setEnabled(false);
    refreshStatus();
}

} // namespace demo
