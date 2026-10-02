#pragma once

#include <lqtexec/EventLoopScheduler.h>
#include <lqtexec/detail/AffineOperation.h>
#include <lqtexec/detail/Lexec.h>

#include <QByteArray>
#include <QList>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace lqtexec {

struct NetworkResponse {
    // Zero when the protocol has none.
    int statusCode = 0;
    // After redirections.
    QUrl url;
    // As Qt reports them: Qt 6.7 and later give header names in lower case.
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
};

// A failed request, HTTP errors included: their status code and body come along.
struct NetworkError {
    QNetworkReply::NetworkError code = QNetworkReply::UnknownNetworkError;
    QString message;
    int statusCode = 0;
    QByteArray body;
};

namespace detail {

enum class HttpMethod : std::uint8_t { get, head, deleteResource, post, put, custom };

struct NetworkRequestSpec {
    HttpMethod method;
    QNetworkRequest request;
    QByteArray body;
    QByteArray verb;
};

QNetworkReply *sendNetworkRequest(QNetworkAccessManager &manager, NetworkRequestSpec const &spec);
NetworkResponse readNetworkResponse(QNetworkReply &reply);
NetworkError readNetworkError(QNetworkReply &reply);

template <class Rcvr>
struct NetworkOperation final : AffineOperation<NetworkOperation<Rcvr>, Rcvr> {
    using Base = AffineOperation<NetworkOperation<Rcvr>, Rcvr>;

    NetworkOperation(EventLoopScheduler const scheduler, QNetworkAccessManager *const manager_,
                     NetworkRequestSpec &&spec_, Rcvr &&rcvr_)
        : Base(scheduler, static_cast<Rcvr &&>(rcvr_)), manager(manager_),
          spec(static_cast<NetworkRequestSpec &&>(spec_)) {}

private:
    friend Base;

    struct ReplyDeleter {
        void operator()(QNetworkReply *const target) const noexcept { target->deleteLater(); }
    };

    // A reply may have finished before the connection was made.
    void begin() noexcept {
        reply.reset(sendNetworkRequest(*manager, spec));
        connection = QObject::connect(reply.get(), &QNetworkReply::finished, reply.get(), [this] { onFinished(); });
        if (reply->isFinished()) {
            onFinished();
        }
    }

    void onFinished() {
        if (this->cancelling()) {
            release();
            this->finishStopped();
            return;
        }
        if (reply->error() == QNetworkReply::NoError) {
            response.emplace(readNetworkResponse(*reply));
        } else {
            error.emplace(readNetworkError(*reply));
        }
        release();
        this->finish();
    }

    // abort() emits finished() before it returns.
    void cancel() noexcept { reply->abort(); }

    void abandon() noexcept {
        QObject::disconnect(connection);
        delete reply.release();
        this->finishStopped();
    }

    void deliver() noexcept {
        if (response) {
            lexec::set_value(static_cast<Rcvr &&>(this->rcvr), static_cast<NetworkResponse &&>(*response));
        } else {
            lexec::set_error(static_cast<Rcvr &&>(this->rcvr), static_cast<NetworkError &&>(*error));
        }
    }

    void release() noexcept {
        QObject::disconnect(connection);
        reply.reset();
    }

    QNetworkAccessManager *manager;
    NetworkRequestSpec spec;
    std::unique_ptr<QNetworkReply, ReplyDeleter> reply;
    QMetaObject::Connection connection;
    std::optional<NetworkResponse> response;
    std::optional<NetworkError> error;
};

struct NetworkAttrs {
    template <class Tag, class... Env,
              std::enable_if_t<std::is_same_v<Tag, lexec::set_value_t> or std::is_same_v<Tag, lexec::set_error_t> or
                                   std::is_same_v<Tag, lexec::set_stopped_t>,
                               int> = 0>
    EventLoopScheduler query(lexec::get_completion_scheduler_t<Tag>, Env const &...) const noexcept {
        return scheduler;
    }

    EventLoopScheduler scheduler;
};

struct NetworkSender {
    using sender_concept = lexec::sender_t;
    using completion_signatures =
        lexec::completion_signatures<lexec::set_value_t(NetworkResponse), lexec::set_error_t(NetworkError),
                                     lexec::set_stopped_t()>;

    template <class Rcvr>
    NetworkOperation<Rcvr> connect(Rcvr rcvr) const & {
        return {scheduler, manager, NetworkRequestSpec{spec}, static_cast<Rcvr &&>(rcvr)};
    }

    template <class Rcvr>
    NetworkOperation<Rcvr> connect(Rcvr rcvr) && {
        return {scheduler, manager, static_cast<NetworkRequestSpec &&>(spec), static_cast<Rcvr &&>(rcvr)};
    }

    NetworkAttrs get_env() const noexcept { return {scheduler}; }

    EventLoopScheduler scheduler;
    QNetworkAccessManager *manager;
    NetworkRequestSpec spec;
};

} // namespace detail

// Sends requests through a QNetworkAccessManager from the thread it lives on, which the
// scheduler must serve. Each sender sends its request when started and completes on that
// thread with the whole response, or a NetworkError, HTTP errors included; a stop request
// aborts the reply and completes with set_stopped.
struct NetworkClient {
    NetworkClient(EventLoopScheduler const scheduler_, QNetworkAccessManager &manager_)
        : scheduler(scheduler_), manager(&manager_) {
        Q_ASSERT_X(manager_.thread() == scheduler_.thread(), "lqtexec::NetworkClient",
                   "the scheduler must run on the manager's thread");
    }

    detail::NetworkSender get(QNetworkRequest request) const {
        return make(detail::HttpMethod::get, std::move(request), {}, {});
    }
    detail::NetworkSender head(QNetworkRequest request) const {
        return make(detail::HttpMethod::head, std::move(request), {}, {});
    }
    detail::NetworkSender deleteResource(QNetworkRequest request) const {
        return make(detail::HttpMethod::deleteResource, std::move(request), {}, {});
    }
    detail::NetworkSender post(QNetworkRequest request, QByteArray body) const {
        return make(detail::HttpMethod::post, std::move(request), std::move(body), {});
    }
    detail::NetworkSender put(QNetworkRequest request, QByteArray body) const {
        return make(detail::HttpMethod::put, std::move(request), std::move(body), {});
    }
    detail::NetworkSender send(QByteArray verb, QNetworkRequest request, QByteArray body = {}) const {
        return make(detail::HttpMethod::custom, std::move(request), std::move(body), std::move(verb));
    }

private:
    detail::NetworkSender make(detail::HttpMethod const method, QNetworkRequest request, QByteArray body,
                               QByteArray verb) const {
        return {scheduler, manager, {method, std::move(request), std::move(body), std::move(verb)}};
    }

    EventLoopScheduler scheduler;
    QNetworkAccessManager *manager;
};

} // namespace lqtexec
