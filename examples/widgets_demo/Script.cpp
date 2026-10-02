#include "Script.h"

#include "JobWindow.h"
#include "Journal.h"
#include "MainWindow.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QPixmap>
#include <QPushButton>

namespace demo {

Script::Script(MainWindow &main_, Journal &journal_, std::optional<QDir> screenshots_)
    : main(main_), journal(journal_), screenshots(std::move(screenshots_)) {
    steps.push_back({QStringLiteral("open and start three jobs"), [] { return true; },
                     [this] {
                         saving = main.openJobWindow();
                         cancelled = main.openJobWindow();
                         closed = main.openJobWindow();
                         saving->startButton()->click();
                         cancelled->startButton()->click();
                         closed->startButton()->click();
                     }});
    steps.push_back({QStringLiteral("cancel job 2"), [this] { return journal.contains(QStringLiteral("job 2: step 3/")); },
                     [this] {
                         capture(QStringLiteral("1-running"));
                         cancelled->cancelButton()->click();
                     }});
    steps.push_back({QStringLiteral("close job 3's window while it runs"),
                     [this] { return journal.contains(QStringLiteral("job 3: step 5/")); },
                     [this] { closed->close(); }});
    steps.push_back({QStringLiteral("close the main window while job 1 saves"),
                     [this] { return journal.contains(QStringLiteral("job 1: finished, saving")); },
                     [this] {
                         capture(QStringLiteral("2-saving"));
                         main.close();
                     }});
    steps.push_back({QStringLiteral("watch the drain"),
                     [this] { return journal.contains(QStringLiteral("app: shutdown requested")); },
                     [this] { capture(QStringLiteral("3-shutting-down")); }});
    QObject::connect(&timer, &QTimer::timeout, &timer, [this] { tick(); });
}

void Script::start() { timer.start(20); }

void Script::tick() {
    if (next == steps.size()) {
        timer.stop();
        return;
    }
    auto &step = steps[next];
    if (step.ready()) {
        qInfo().noquote() << "script:" << step.name;
        ++next;
        step.act();
    }
}

void Script::capture(QString const &name) {
    if (not screenshots) {
        return;
    }
    // The journal's lines reach the view as queued calls.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    main.refreshStatus();
    main.grab().save(screenshots->filePath(name + QStringLiteral("-main.png")));
    for (auto const &window : {saving, cancelled, closed}) {
        if (window) {
            window->grab().save(screenshots->filePath(name + QStringLiteral("-job-%1.png").arg(window->id())));
        }
    }
}

bool Script::verify() const {
    auto ok = next == steps.size();
    auto const expect = [&](bool const condition, QString const &what) {
        qInfo().noquote() << (condition ? "  ok:     " : "  FAILED: ") << what;
        ok = ok and condition;
    };
    auto const at = [this](QString const &marker) { return journal.find(marker); };
    expect(next == steps.size(), QStringLiteral("every scripted step ran"));
    expect(at(QStringLiteral("job 2: cancelled at step")).has_value(), QStringLiteral("job 2 was cancelled"));

    auto const closing = at(QStringLiteral("job 3: window closing"));
    expect(closing.has_value(), QStringLiteral("job 3's window was destroyed"));
    expect(at(QStringLiteral("job 3: stopped, its window is gone")).has_value(),
           QStringLiteral("job 3's work stopped once its window was gone"));
    expect(closing and not journal.find(QStringLiteral("job 3: step"), *closing + 1),
           QStringLiteral("nothing touched job 3's window after it was destroyed"));

    auto const closeRequested = at(QStringLiteral("main window: close requested"));
    auto const saved = at(QStringLiteral("job 1: saved"));
    auto const drained = at(QStringLiteral("app: drained"));
    expect(closeRequested and saved and *closeRequested < *saved,
           QStringLiteral("job 1's save went on after the shutdown was requested"));
    expect(saved and drained and *saved < *drained, QStringLiteral("the shutdown waited for job 1's save"));
    expect(at(QStringLiteral("main window: heartbeat loop stopped")).has_value(),
           QStringLiteral("the main window's heartbeat stopped with its scope"));
    return ok;
}

} // namespace demo
