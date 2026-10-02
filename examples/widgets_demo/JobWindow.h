#pragma once

#include "Services.h"

#include <lqtexec/ObjectScope.h>

#include <QString>
#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;

namespace demo {

// A window running one job at a time: steps computed on the thread pool, each shown here,
// then a save that cannot be interrupted. Closing it destroys it at once, whatever runs.
struct JobWindow final : QWidget {
    JobWindow(Services const &services_, int id_, QWidget *parent);
    ~JobWindow() override;

    int id() const noexcept { return jobId; }
    QPushButton *startButton() const noexcept { return start; }
    QPushButton *cancelButton() const noexcept { return cancel; }

    static constexpr int kSteps = 20;

private:
    enum class Ending : int { finished, cancelled };

    void startJob();
    // On the GUI thread while the window lives; true once the last step is done.
    bool advance(QString const &where);
    void ended(Ending ending);
    void save();

    Services services;
    int jobId;
    int done = 0;
    QLabel *status = nullptr;
    QProgressBar *progress = nullptr;
    QPushButton *start = nullptr;
    QPushButton *cancel = nullptr;
    // Last, so that it goes first: its work stops before anything else here is destroyed.
    lqtexec::ObjectScope tasks;
};

} // namespace demo
