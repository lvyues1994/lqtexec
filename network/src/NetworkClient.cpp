#include <lqtexec/NetworkClient.h>

namespace lqtexec::detail {

namespace {

int statusCodeOf(QNetworkReply const &reply) {
    return reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

} // namespace

QNetworkReply *sendNetworkRequest(QNetworkAccessManager &manager, NetworkRequestSpec const &spec) {
    switch (spec.method) {
    case HttpMethod::get:
        return manager.get(spec.request);
    case HttpMethod::head:
        return manager.head(spec.request);
    case HttpMethod::deleteResource:
        return manager.deleteResource(spec.request);
    case HttpMethod::post:
        return manager.post(spec.request, spec.body);
    case HttpMethod::put:
        return manager.put(spec.request, spec.body);
    case HttpMethod::custom:
        break;
    }
    return manager.sendCustomRequest(spec.request, spec.verb, spec.body);
}

NetworkResponse readNetworkResponse(QNetworkReply &reply) {
    auto response = NetworkResponse{};
    response.statusCode = statusCodeOf(reply);
    response.url = reply.url();
    response.headers = reply.rawHeaderPairs();
    response.body = reply.readAll();
    return response;
}

NetworkError readNetworkError(QNetworkReply &reply) {
    auto error = NetworkError{};
    error.code = reply.error();
    error.message = reply.errorString();
    error.statusCode = statusCodeOf(reply);
    error.body = reply.readAll();
    return error;
}

} // namespace lqtexec::detail
