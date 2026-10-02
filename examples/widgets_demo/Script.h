#pragma once

#include <QDir>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>
#include <vector>

namespace demo {

struct Journal;
struct JobWindow;
struct MainWindow;

// Drives the demo through the real UI, as a user would: three jobs, one cancelled, one
// whose window is closed while it runs, then closing the main window while the third saves.
// Each step waits for what the journal reports. Optionally saves screenshots on the way.
struct Script {
    Script(MainWindow &main_, Journal &journal_, std::optional<QDir> screenshots_);

    void start();

    // After the event loop: whether the journal shows the expected flow.
    bool verify() const;

private:
    struct Step {
        QString name;
        std::function<bool()> ready;
        std::function<void()> act;
    };

    void tick();
    void capture(QString const &name);

    MainWindow &main;
    Journal &journal;
    std::optional<QDir> screenshots;
    std::vector<Step> steps;
    std::size_t next = 0;
    QTimer timer;
    QPointer<JobWindow> saving;
    QPointer<JobWindow> cancelled;
    QPointer<JobWindow> closed;
};

} // namespace demo
