#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <memory>

namespace lqtexec_test {

// A minimal HTTP/1.1 server on the loopback interface, driven by the event loop of the
// thread that creates it:
//   /hello   200, body "hello" and header X-Test: yes
//   /missing 404, body "nope"
//   /echo    200, the request's body; the method in header X-Method
//   /slow    never answers
struct TestHttpServer {
    TestHttpServer() {
        server.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] { accept(); });
    }

    QUrl url(QString const &path) const {
        return QUrl{QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path)};
    }

    int connections = 0;

private:
    void accept() {
        while (auto *const socket = server.nextPendingConnection()) {
            ++connections;
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, buffer] {
                buffer->append(socket->readAll());
                respondWhenComplete(*socket, *buffer);
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    }

    static void respondWhenComplete(QTcpSocket &socket, QByteArray const &buffer) {
        auto const headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        auto const lines = buffer.left(headerEnd).split('\n');
        auto const requestLine = lines.front().trimmed().split(' ');
        auto contentLength = 0;
        for (auto const &line : lines) {
            if (line.toLower().startsWith("content-length:")) {
                contentLength = line.mid(15).trimmed().toInt();
            }
        }
        if (buffer.size() - (headerEnd + 4) < contentLength) {
            return;
        }
        auto const method = requestLine.value(0);
        auto const path = requestLine.value(1);
        auto const body = buffer.mid(headerEnd + 4, contentLength);
        if (path == "/hello") {
            reply(socket, "200 OK", "hello", "X-Test: yes\r\n");
        } else if (path == "/missing") {
            reply(socket, "404 Not Found", "nope", "");
        } else if (path == "/echo") {
            reply(socket, "200 OK", method == "HEAD" ? QByteArray{} : body, "X-Method: " + method + "\r\n");
        }
    }

    static void reply(QTcpSocket &socket, QByteArray const &status, QByteArray const &body, QByteArray const &headers) {
        socket.write("HTTP/1.1 " + status + "\r\nContent-Type: text/plain\r\nContent-Length: " +
                     QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + headers + "\r\n" + body);
        socket.disconnectFromHost();
    }

    QTcpServer server;
};

} // namespace lqtexec_test
