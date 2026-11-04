#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include "catch.h"
#include "logger.h"
#include "network/network-manager.h"
#include "network/network-reply.h"
#include "functions.h"


TEST_CASE("NetworkManager", "[.][network]")
{
	setTestModeEnabled(false);

	/**
	 * Start a request that should take 300ms.
	 * Wait for 2s.
	 * The request should have been received and the active query count reset.
	 */
	SECTION("Basic")
	{
		NetworkManager manager;
		manager.setMaxConcurrency(1);

		REQUIRE(manager.activeQueries() == 0);

		QNetworkRequest request(QUrl("https://httpbin.org/delay/0.1"));
		NetworkReply *reply = manager.get(request);

		bool received = false;
		QObject::connect(reply, &NetworkReply::finished, [&]() {
			received = true;
		});

		QEventLoop loop;
		QTimer::singleShot(2000, &loop, &QEventLoop::quit);
		loop.exec();

		REQUIRE(received);
		REQUIRE(manager.activeQueries() == 0);
	}

	/**
	 * Start a request that should take 300ms.
	 * Abort the request after 100ms.
	 * Wait for 2s.
	 * The request should have been finished (without data) and the active query count reset.
	 */
	SECTION("Abort after starting")
	{
		NetworkManager manager;
		manager.setMaxConcurrency(1);

		REQUIRE(manager.activeQueries() == 0);

		QNetworkRequest request(QUrl("https://httpbin.org/delay/0.3"));
		NetworkReply *reply = manager.get(request);

		bool received = false;
		QObject::connect(reply, &NetworkReply::finished, [&]() {
			received = true;
		});
		QTimer::singleShot(100, [&]() {
			reply->abort();
		});

		QEventLoop loop;
		QTimer::singleShot(2000, &loop, &QEventLoop::quit);
		loop.exec();

		REQUIRE(received);
		REQUIRE(manager.activeQueries() == 0);
	}

	/**
	 * Start a request that should take 300ms and should start in 2s.
	 * Abort the request after 100ms.
	 * Wait for 5s.
	 * The request should have been finished (without data) and the active query count reset.
	 */
	SECTION("Abort before starting")
	{
		NetworkManager manager;
		manager.setMaxConcurrency(1);
		manager.setInterval(-1, 2000);

		REQUIRE(manager.activeQueries() == 0);

		// Start a random request to trigger the interval
		QNetworkRequest fakeRequest(QUrl("https://httpbin.org/status/200"));
		manager.get(fakeRequest);

		QNetworkRequest request(QUrl("https://httpbin.org/delay/0.3"));
		NetworkReply *reply = manager.get(request);

		bool received = false;
		QObject::connect(reply, &NetworkReply::finished, [&]() {
			received = true;
		});
		QTimer::singleShot(100, [&]() {
			reply->abort();
		});

		QEventLoop loop;
		QTimer::singleShot(5000, &loop, &QEventLoop::quit);
		loop.exec();

		REQUIRE(received);
		REQUIRE(manager.activeQueries() == 0);
	}

	/**
	 * Start a request against a server that accepts the connection and never answers.
	 * Wait for 3s with a 1s transfer timeout.
	 * The request should have been aborted and the active query count reset, so that later
	 * requests still get dispatched.
	 */
	SECTION("Transfer timeout on a silent server")
	{
		QTcpServer blackhole;
		REQUIRE(blackhole.listen(QHostAddress::LocalHost, 0));

		NetworkManager manager;
		manager.setMaxConcurrency(1);
		manager.setTransferTimeout(1000);

		QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/blackhole").arg(blackhole.serverPort())));

		QStringList warnings;
		QObject::connect(&Logger::getInstance(), &Logger::newLog, [&warnings](const QString &msg) {
			if (msg.contains(QLatin1String("No data received for"))) {
				warnings.append(msg);
			}
		});

		NetworkReply *reply = manager.get(request);

		bool received = false;
		QObject::connect(reply, &NetworkReply::finished, [&]() {
			received = true;
		});

		QEventLoop loop;
		QTimer::singleShot(3000, &loop, &QEventLoop::quit);
		loop.exec();

		REQUIRE(received);
		REQUIRE(manager.activeQueries() == 0);

		// Qt aborts an expired transfer timeout with TimeoutError, not the OperationCanceledError
		// that abort() gives. NetworkReply::replyFinished() keys its warning off this.
		REQUIRE(reply->error() == QNetworkReply::TimeoutError);

		// Qt closes the reply before emitting finished(), and FileDownloader still drains it there
		REQUIRE(reply->readAll().isEmpty());

		// The whole point of the timeout is not having to guess at it later in the log
		REQUIRE(warnings.size() == 1);

		// The queue has to keep moving once the dead reply is out of the way
		NetworkReply *next = manager.get(request);
		bool nextReceived = false;
		QObject::connect(next, &NetworkReply::finished, [&]() {
			nextReceived = true;
		});

		QEventLoop loop2;
		QTimer::singleShot(3000, &loop2, &QEventLoop::quit);
		loop2.exec();

		REQUIRE(nextReceived);
	}

	/**
	 * With one slot, start a request whose finished handler starts a second one and waits for it
	 * in a nested event loop, as Image::parseDetails does through an exiftool write.
	 * The second request should still be dispatched.
	 */
	SECTION("Slot is released while a finished handler runs a nested loop")
	{
		QTcpServer server;
		REQUIRE(server.listen(QHostAddress::LocalHost, 0));
		QObject::connect(&server, &QTcpServer::newConnection, [&server]() {
			QTcpSocket *socket = server.nextPendingConnection();
			QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
				socket->readAll();
				socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
				socket->disconnectFromHost();
			});
		});

		NetworkManager manager;
		manager.setMaxConcurrency(1);

		const QUrl url(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
		NetworkReply *first = manager.get(QNetworkRequest(url));

		bool innerReceived = false;
		QObject::connect(first, &NetworkReply::finished, [&]() {
			NetworkReply *inner = manager.get(QNetworkRequest(url));
			QEventLoop nested;
			QObject::connect(inner, &NetworkReply::finished, &nested, &QEventLoop::quit);
			QTimer::singleShot(3000, &nested, &QEventLoop::quit);
			nested.exec();
			innerReceived = !inner->isRunning();
		});

		QEventLoop loop;
		QObject::connect(first, &NetworkReply::finished, &loop, &QEventLoop::quit, Qt::QueuedConnection);
		QTimer::singleShot(5000, &loop, &QEventLoop::quit);
		loop.exec();

		REQUIRE(innerReceived);
	}

	setTestModeEnabled(true);
}
