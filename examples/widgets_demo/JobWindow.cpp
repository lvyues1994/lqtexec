#include "JobWindow.h"

#include "Journal.h"

#include <lqtexec/WaitSignal.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include <chrono>
#include <thread>

namespace demo {

namespace {

using namespace std::chrono_literals;

// Stands for CPU work on a pool thread; reports where it ran.
QString computeStep() noexcept {
    std::this_thread::sleep_for(120ms);
    return QThread::currentThread()->objectName();
}

} // namespace

JobWindow::JobWindow(Services const &services_, int const id_, QWidget *const parent)
    : QWidget(parent, Qt::Window), services(services_), jobId(id_),
      tasks{this, services_.gui, services_.appScope->get_token()} {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Job #%1").arg(jobId));
    setMinimumWidth(380);

    auto *const title = new QLabel{QStringLiteral("Job #%1").arg(jobId)};
    auto titleFont = title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.25);
    title->setFont(titleFont);
    status = new QLabel{QStringLiteral("Ready.")};
    progress = new QProgressBar;
    progress->setRange(0, kSteps);
    progress->setValue(0);
    progress->setFormat(QStringLiteral("%v / %m steps"));
    start = new QPushButton{QStringLiteral("Start")};
    cancel = new QPushButton{QStringLiteral("Cancel")};
    cancel->setEnabled(false);
    auto *const hint = new QLabel{QStringLiteral(
        "Close this window at any time: its work stops, the result of a step still running is dropped, and a "
        "save in progress completes in the background.")};
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: palette(mid);"));

    auto *const buttons = new QHBoxLayout;
    buttons->addWidget(start);
    buttons->addWidget(cancel);
    buttons->addStretch();
    auto *const layout = new QVBoxLayout{this};
    layout->addWidget(title);
    layout->addWidget(status);
    layout->addWidget(progress);
    layout->addLayout(buttons);
    layout->addWidget(hint);

    QObject::connect(start, &QPushButton::clicked, this, [this] { startJob(); });
    services.journal->write(QStringLiteral("job %1: window opened").arg(jobId));
}

JobWindow::~JobWindow() {
    services.journal->write(QStringLiteral("job %1: window closing, its scope stops the work").arg(jobId));
}

// The steps loop until the last one, each hopping to the pool and back; the Cancel button
// races them. Only then_on continuations touch the window: they are skipped once it is gone,
// so the continuations that may run later use only what main() owns.
void JobWindow::startJob() {
    done = 0;
    progress->setValue(0);
    start->setEnabled(false);
    cancel->setEnabled(true);
    status->setText(QStringLiteral("Running…"));
    ++services.activity->jobsRunning;
    services.journal->write(QStringLiteral("job %1: started").arg(jobId));

    auto steps = lexec::repeat_until(lexec::schedule(services.pool) | lexec::then(computeStep) |
                                     lqtexec::then_on(tasks, [this](QString const &where) noexcept {
                                         return advance(where);
                                     }));
    auto cancelled = lqtexec::wait_signal(cancel, &QPushButton::clicked) |
                     lexec::then([](bool) noexcept { return Ending::cancelled; });
    auto *const journal = services.journal;
    auto *const activity = services.activity;
    auto *const stop = services.stop;
    auto const id_ = jobId;
    lexec::spawn(lexec::when_any(std::move(steps) | lexec::then([]() noexcept { return Ending::finished; }),
                                 std::move(cancelled)) |
                     lqtexec::then_on(tasks, [this](Ending const ending) noexcept { ended(ending); }) |
                     lexec::upon_stopped([journal, activity, stop, id_]() noexcept {
                         --activity->jobsRunning;
                         journal->write(stop->stop_requested()
                                            ? QStringLiteral("job %1: stopped by the shutdown").arg(id_)
                                            : QStringLiteral("job %1: stopped, its window is gone; the step "
                                                             "still running finished, its update was skipped")
                                                  .arg(id_));
                     }) |
                     lexec::upon_error([journal, activity, id_](auto const &) noexcept {
                         --activity->jobsRunning;
                         journal->write(QStringLiteral("job %1: failed").arg(id_));
                     }),
                 tasks.get_token());
}

bool JobWindow::advance(QString const &where) {
    ++done;
    progress->setValue(done);
    status->setText(QStringLiteral("Step %1 of %2, computed on %3").arg(done).arg(kSteps).arg(where));
    services.journal->write(QStringLiteral("job %1: step %2/%3").arg(jobId).arg(done).arg(kSteps));
    return done == kSteps;
}

void JobWindow::ended(Ending const ending) {
    --services.activity->jobsRunning;
    cancel->setEnabled(false);
    if (ending == Ending::cancelled) {
        status->setText(QStringLiteral("Cancelled at step %1 of %2.").arg(done).arg(kSteps));
        start->setEnabled(true);
        services.journal->write(QStringLiteral("job %1: cancelled at step %2/%3").arg(jobId).arg(done).arg(kSteps));
        return;
    }
    status->setText(QStringLiteral("Finished. Saving… (cannot be interrupted)"));
    services.journal->write(QStringLiteral("job %1: finished, saving").arg(jobId));
    save();
}

// unstoppable: neither closing the window nor the shutdown interrupts the save, and the
// shutdown waits for it. Only showing that it is saved is skipped once the window is gone.
void JobWindow::save() {
    ++services.activity->savesRunning;
    auto *const journal = services.journal;
    auto *const activity = services.activity;
    auto const id_ = jobId;
    lexec::spawn(lexec::unstoppable(lexec::schedule(services.pool) | lexec::then([journal, activity, id_]() noexcept {
                                        std::this_thread::sleep_for(700ms);
                                        --activity->savesRunning;
                                        journal->write(QStringLiteral("job %1: saved").arg(id_));
                                    })) |
                     lqtexec::then_on(tasks,
                                      [this]() noexcept {
                                          status->setText(QStringLiteral("Saved."));
                                          start->setEnabled(true);
                                      }) |
                     lexec::upon_stopped([journal, id_]() noexcept {
                         journal->write(QStringLiteral("job %1: showing the save skipped, its window is gone or the "
                                                       "app is shutting down")
                                            .arg(id_));
                     }) |
                     lexec::upon_error([journal, id_](auto const &) noexcept {
                         journal->write(QStringLiteral("job %1: save failed").arg(id_));
                     }),
                 tasks.get_token());
}

} // namespace demo
