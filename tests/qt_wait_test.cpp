#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QCoreApplication>
#include <QTimer>

#include <chrono>

using namespace lqtexec_test;

TEST_CASE("the tests run inside a QCoreApplication") { CHECK(QCoreApplication::instance() != nullptr); }

TEST_CASE("processEventsUntil returns once an event makes the condition hold") {
    auto fired = false;
    QTimer::singleShot(5, [&] { fired = true; });
    CHECK(processEventsUntil([&] { return fired; }));
}

TEST_CASE("processEventsUntil gives up at its timeout") {
    auto const before = std::chrono::steady_clock::now();
    CHECK_FALSE(processEventsUntil([] { return false; }, 20ms));
    CHECK(std::chrono::steady_clock::now() - before >= 20ms);
}

TEST_CASE("lexec runs alongside the Qt event loop") {
    auto const result = lexec::sync_wait(lexec::just(21) | lexec::then([](int const v) noexcept { return v * 2; }));
    REQUIRE(result.has_value());
    CHECK(std::get<0>(*result) == 42);
}
