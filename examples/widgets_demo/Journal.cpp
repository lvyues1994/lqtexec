#include "Journal.h"

#include <QDebug>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QThread>

namespace demo {

Journal::Journal() { clock.start(); }

void Journal::write(QString const &text) {
    auto thread = QThread::currentThread()->objectName();
    if (thread.isEmpty()) {
        thread = QStringLiteral("?");
    }
    auto const seconds = static_cast<double>(clock.elapsed()) / 1000.0;
    auto const line = QStringLiteral("%1s  %2  %3").arg(seconds, 6, 'f', 3).arg(thread, -16).arg(text);
    {
        auto const lock = std::lock_guard<std::mutex>{mutex};
        lines.append(line);
    }
    qInfo().noquote() << line;
    QMetaObject::invokeMethod(
        &relay,
        [this, line] {
            if (view != nullptr) {
                view->appendPlainText(line);
                view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
            }
        },
        Qt::QueuedConnection);
}

void Journal::attach(QPlainTextEdit *const view_) { view = view_; }

std::optional<int> Journal::find(QString const &marker, int const from) const {
    auto const lock = std::lock_guard<std::mutex>{mutex};
    for (auto i = from; i < lines.size(); ++i) {
        if (lines[i].contains(marker)) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace demo
