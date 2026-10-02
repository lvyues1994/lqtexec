#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

#include <mutex>
#include <optional>

class QPlainTextEdit;

namespace demo {

// Records what happens, from any thread: each line is printed at once and shown in the
// attached view. Create it on the GUI thread; it outlives the windows.
struct Journal {
    Journal();

    void write(QString const &text);

    // On the GUI thread; null detaches.
    void attach(QPlainTextEdit *view_);

    // The index of the first line containing `marker`, searching from `from`.
    std::optional<int> find(QString const &marker, int from = 0) const;
    bool contains(QString const &marker) const { return find(marker).has_value(); }

private:
    QElapsedTimer clock;
    mutable std::mutex mutex;
    QStringList lines;
    // Lives on the GUI thread and carries lines to the view.
    QObject relay;
    QPlainTextEdit *view = nullptr;
};

} // namespace demo
