#pragma once

#include <QObject>
#include <QString>

namespace lqtexec_test {

struct Emitter : QObject {
    Q_OBJECT

public:
    void emitPrivate(int const value) { emit privateSignal(value, QPrivateSignal{}); }

signals:
    void noArgs();
    void withInt(int value);
    void withString(QString const &text);
    void withTwo(int value, QString const &text);
    void privateSignal(int value, QPrivateSignal);
};

} // namespace lqtexec_test
