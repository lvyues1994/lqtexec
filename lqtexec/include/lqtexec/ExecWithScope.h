#pragma once

#include <lqtexec/detail/Lexec.h>

class QCoreApplication;

namespace lqtexec {

// Runs the application's event loop until `stop` is requested, then closes `scope`, asks
// its work to stop and keeps processing events until it has joined; returns the event
// loop's exit code. Request `stop` to quit instead of calling quit(): should the event loop
// exit some other way, the scope is still drained before this returns. Call it on the main
// thread.
int exec_with_scope(QCoreApplication &app, lexec::counting_scope &scope, lexec::inplace_stop_source &stop);

} // namespace lqtexec
