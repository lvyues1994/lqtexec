#include <lqtexec/EventLoopContext.h>
#include <lqtexec/NetworkClient.h>

#include "network/TestHttpServer.h"
#include "support/QtWait.h"

#include <doctest/doctest.h>

#include <QNetworkAccessManager>
#include <QThread>

#include <chrono>
#include <optional>
#include <thread>

using namespace lqtexec_test;

namespace {

using Clock = std::chrono::steady_clock;

// Records the response or the error a request completed with.
struct NetworkReceiver {
    using receiver_concept = lexec::receiver_t;

    void set_value(lqtexec::NetworkResponse response_) && noexcept {
        *response = std::move(response_);
        record(Outcome::value);
    }
    void set_error(lqtexec::NetworkError error_) && noexcept {
        *error = std::move(error_);
        record(Outcome::error);
    }
    void set_stopped() && noexcept { record(Outcome::stopped); }

    auto get_env() const noexcept { return lexec::prop{lexec::get_stop_token, token}; }

    void record(Outcome const outcome) const noexcept { recordInto(*completion).record(outcome); }

    Completion *completion;
    std::optional<lqtexec::NetworkResponse> *response;
    std::optional<lqtexec::NetworkError> *error;
    lexec::inplace_stop_token token{};
};

struct Exchange {
    template <class Sndr>
    void run(Sndr &&sndr, lexec::inplace_stop_token const token = {}) {
        auto op = lexec::connect(static_cast<Sndr &&>(sndr), NetworkReceiver{&completion, &response, &error, token});
        lexec::start(op);
        REQUIRE(processEventsUntil([&] { return completion.done(); }));
    }

    Completion completion;
    std::optional<lqtexec::NetworkResponse> response;
    std::optional<lqtexec::NetworkError> error;
};

} // namespace

TEST_CASE("a GET completes with the status, headers and body, on the scheduler's thread") {
    auto context = lqtexec::EventLoopContext{};
    auto server = TestHttpServer{};
    auto manager = QNetworkAccessManager{};
    auto const client = lqtexec::NetworkClient{context.get_scheduler(), manager};
    auto exchange = Exchange{};
    exchange.run(client.get(QNetworkRequest{server.url(QStringLiteral("/hello"))}));
    REQUIRE(exchange.response.has_value());
    CHECK(exchange.response->statusCode == 200);
    CHECK(exchange.response->body == QByteArrayLiteral("hello"));
    // Qt 6.7 and later report header names in lower case.
    auto hasHeader = false;
    for (auto const &header : exchange.response->headers) {
        hasHeader = hasHeader or (header.first.toLower() == "x-test" and header.second == "yes");
    }
    CHECK(hasHeader);
    CHECK(exchange.completion.thread == QThread::currentThread());
}

TEST_CASE("an HTTP error completes with a NetworkError carrying the status and body") {
    auto context = lqtexec::EventLoopContext{};
    auto server = TestHttpServer{};
    auto manager = QNetworkAccessManager{};
    auto const client = lqtexec::NetworkClient{context.get_scheduler(), manager};
    auto exchange = Exchange{};
    exchange.run(client.get(QNetworkRequest{server.url(QStringLiteral("/missing"))}));
    REQUIRE(exchange.error.has_value());
    CHECK(exchange.error->code == QNetworkReply::ContentNotFoundError);
    CHECK(exchange.error->statusCode == 404);
    CHECK(exchange.error->body == QByteArrayLiteral("nope"));
}

TEST_CASE("POST, PUT and custom verbs send their bodies") {
    auto context = lqtexec::EventLoopContext{};
    auto server = TestHttpServer{};
    auto manager = QNetworkAccessManager{};
    auto const client = lqtexec::NetworkClient{context.get_scheduler(), manager};
    auto const url = server.url(QStringLiteral("/echo"));
    auto request = QNetworkRequest{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("text/plain"));

    auto posted = Exchange{};
    posted.run(client.post(request, QByteArrayLiteral("posted")));
    REQUIRE(posted.response.has_value());
    CHECK(posted.response->body == QByteArrayLiteral("posted"));

    auto put = Exchange{};
    put.run(client.put(request, QByteArrayLiteral("put")));
    REQUIRE(put.response.has_value());
    CHECK(put.response->body == QByteArrayLiteral("put"));

    auto patched = Exchange{};
    patched.run(client.send(QByteArrayLiteral("PATCH"), request, QByteArrayLiteral("patched")));
    REQUIRE(patched.response.has_value());
    CHECK(patched.response->body == QByteArrayLiteral("patched"));

    auto headed = Exchange{};
    headed.run(client.head(QNetworkRequest{url}));
    REQUIRE(headed.response.has_value());
    CHECK(headed.response->statusCode == 200);
    CHECK(headed.response->body.isEmpty());
}

TEST_CASE("a refused connection completes with a NetworkError") {
    auto context = lqtexec::EventLoopContext{};
    auto manager = QNetworkAccessManager{};
    auto const client = lqtexec::NetworkClient{context.get_scheduler(), manager};
    auto url = QUrl{};
    {
        auto closed = TestHttpServer{};
        url = closed.url(QStringLiteral("/hello"));
    }
    auto exchange = Exchange{};
    exchange.run(client.get(QNetworkRequest{url}));
    REQUIRE(exchange.error.has_value());
    CHECK(exchange.error->code == QNetworkReply::ConnectionRefusedError);
}

TEST_CASE("a stop request aborts the request") {
    auto context = lqtexec::EventLoopContext{};
    auto server = TestHttpServer{};
    auto manager = QNetworkAccessManager{};
    auto const client = lqtexec::NetworkClient{context.get_scheduler(), manager};
    auto source = lexec::inplace_stop_source{};
    auto completion = Completion{};
    auto response = std::optional<lqtexec::NetworkResponse>{};
    auto error = std::optional<lqtexec::NetworkError>{};
    auto op = lexec::connect(client.get(QNetworkRequest{server.url(QStringLiteral("/slow"))}),
                             NetworkReceiver{&completion, &response, &error, source.get_token()});
    lexec::start(op);
    REQUIRE(processEventsUntil([&] { return server.connections == 1; }));
    auto const before = Clock::now();
    source.request_stop();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::stopped);
    CHECK(Clock::now() - before < 1s);
}

TEST_CASE("a request started from another thread completes on the scheduler's thread") {
    auto context = lqtexec::EventLoopContext{};
    auto server = TestHttpServer{};
    auto manager = QNetworkAccessManager{};
    auto const client = lqtexec::NetworkClient{context.get_scheduler(), manager};
    auto completion = Completion{};
    auto response = std::optional<lqtexec::NetworkResponse>{};
    auto error = std::optional<lqtexec::NetworkError>{};
    auto op = lexec::connect(client.get(QNetworkRequest{server.url(QStringLiteral("/hello"))}),
                             NetworkReceiver{&completion, &response, &error});
    auto starter = std::thread{[&] { lexec::start(op); }};
    starter.join();
    REQUIRE(processEventsUntil([&] { return completion.done(); }));
    CHECK(completion.outcome == Outcome::value);
    CHECK(completion.thread == QThread::currentThread());
}
