#include <QEventLoop>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTimer>
#include <QUrl>
#include "catch.h"
#include "custom-network-access-manager.h"
#include "network/flaresolverr-reply.h"
#include "network/network-manager.h"
#include "network/network-reply.h"


static void waitFinished(QNetworkReply *reply)
{
	if (reply->isFinished()) {
		return;
	}
	QEventLoop loop;
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QTimer::singleShot(5000, &loop, &QEventLoop::quit);
	loop.exec();
}

TEST_CASE("FlareSolverrReply")
{
	const QUrl solver("http://localhost:8191/v1");
	const QUrl page("https://rule34.xxx/index.php?page=post&s=list&tags=akisora&pid=0");

	CustomNetworkAccessManager manager;
	manager.setFlareSolverr(solver, { "rule34.xxx" }, 60000);
	CustomNetworkAccessManager::NextFiles.clear();

	SECTION("Serves the page the solver loaded")
	{
		CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/flaresolverr/ok.json");
		QNetworkReply *reply = manager.get(QNetworkRequest(page));
		REQUIRE(qobject_cast<FlareSolverrReply*>(reply) != nullptr);
		waitFinished(reply);

		REQUIRE(reply->isFinished());
		REQUIRE(reply->error() == QNetworkReply::NoError);
		REQUIRE(reply->url() == page);
		REQUIRE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200);
		const QString body = QString::fromUtf8(reply->readAll());
		REQUIRE(body.startsWith("<html><body><ul id=\"tag-sidebar\">"));
		REQUIRE(body.contains("<li class=\"tag-type-copyright\">"));
		reply->deleteLater();
	}

	SECTION("Leaves other hosts alone")
	{
		QNetworkReply *reply = manager.get(QNetworkRequest(QUrl("https://api.rule34.xxx/index.php?page=dapi&s=post&q=index")));
		REQUIRE(qobject_cast<FlareSolverrReply*>(reply) == nullptr);
		reply->deleteLater();
	}

	SECTION("Reports the solver's error without its stack trace")
	{
		CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/flaresolverr/error.json");
		QNetworkReply *reply = manager.get(QNetworkRequest(page));
		waitFinished(reply);

		REQUIRE(reply->error() == QNetworkReply::UnknownNetworkError);
		REQUIRE(reply->errorString() == "FlareSolverr could not load the page: Error: Error solving the challenge. Message: unknown error: net::ERR_NAME_NOT_RESOLVED");
		reply->deleteLater();
	}

	SECTION("Maps the page's HTTP status")
	{
		CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/flaresolverr/status-404.json");
		QNetworkReply *reply = manager.get(QNetworkRequest(page));
		waitFinished(reply);

		REQUIRE(reply->error() == QNetworkReply::ContentNotFoundError);
		REQUIRE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404);
		REQUIRE(!reply->readAll().isEmpty());
		reply->deleteLater();
	}

	SECTION("Fails cleanly when something else answers")
	{
		CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/flaresolverr/not-json.html");
		QNetworkReply *reply = manager.get(QNetworkRequest(page));
		waitFinished(reply);

		REQUIRE(reply->error() == QNetworkReply::ProtocolFailure);
		REQUIRE(reply->errorString() == "FlareSolverr unavailable: not a FlareSolverr response");
		reply->deleteLater();
	}

	SECTION("Abort finishes once, before the solver answers")
	{
		CustomNetworkAccessManager::NextFiles.enqueue("hang");
		QNetworkReply *reply = manager.get(QNetworkRequest(page));
		QSignalSpy spy(reply, &QNetworkReply::finished);

		reply->abort();
		reply->abort();
		REQUIRE(reply->isFinished());
		REQUIRE(reply->error() == QNetworkReply::OperationCanceledError);
		REQUIRE(spy.count() == 1);
		reply->deleteLater();
	}

	SECTION("Through NetworkManager, as sites use it")
	{
		NetworkManager networkManager;
		networkManager.setFlareSolverr(solver, { "rule34.xxx" }, 60000);
		CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/flaresolverr/ok.json");

		NetworkReply *reply = networkManager.get(QNetworkRequest(page));
		QEventLoop loop;
		QObject::connect(reply, &NetworkReply::finished, &loop, &QEventLoop::quit);
		QTimer::singleShot(5000, &loop, &QEventLoop::quit);
		loop.exec();

		REQUIRE(reply->error() == QNetworkReply::NoError);
		REQUIRE(reply->readAll().contains("tag-type-copyright"));
		reply->deleteLater();
	}
}
