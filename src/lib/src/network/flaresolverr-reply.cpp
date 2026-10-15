#include "network/flaresolverr-reply.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkRequest>
#include <QTimer>
#include <chrono>
#include "custom-network-access-manager.h"
#include "logger.h"


FlareSolverrReply::FlareSolverrReply(const QNetworkRequest &request, const QUrl &solver, int msTimeout, CustomNetworkAccessManager *manager)
	: QNetworkReply(manager)
{
	setRequest(request);
	setUrl(request.url());
	setOperation(QNetworkAccessManager::GetOperation);
	open(QIODevice::ReadOnly | QIODevice::Unbuffered);

	// The solver's browser sends its own User-Agent and headers; only the site's cookies carry over
	QJsonArray cookies;
	if (manager->cookieJar() != nullptr) {
		for (const QNetworkCookie &cookie : manager->cookieJar()->cookiesForUrl(request.url())) {
			cookies.append(QJsonObject {
				{ "name", QString::fromUtf8(cookie.name()) },
				{ "value", QString::fromUtf8(cookie.value()) },
			});
		}
	}

	const QJsonObject body {
		{ "cmd", "request.get" },
		{ "url", request.url().toString(QUrl::FullyEncoded) },
		{ "maxTimeout", msTimeout },
		{ "cookies", cookies },
	};

	// The solver answers only once the page has loaded, so silence is expected for up to maxTimeout
	QNetworkRequest solverRequest(solver);
	solverRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	solverRequest.setTransferTimeout(std::chrono::milliseconds(msTimeout + 15000));

	log(QStringLiteral("Loading `%1` through FlareSolverr").arg(request.url().toString()), Logger::Debug);
	m_solverReply = manager->post(solverRequest, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(m_solverReply, &QNetworkReply::finished, this, &FlareSolverrReply::solverFinished);

	// Not a child: ~QNetworkAccessManager deletes every QNetworkReply found by a recursive
	// findChildren(), so a reply nested under this one would be deleted twice
	m_solverReply->setParent(nullptr);
}

FlareSolverrReply::~FlareSolverrReply()
{
	delete m_solverReply.data();
}

void FlareSolverrReply::solverFinished()
{
	if (isFinished()) {
		return;
	}

	// An error status still carries a JSON body explaining it, so parse before looking at the transport error
	const QByteArray data = m_solverReply->readAll();
	QJsonParseError parseError;
	const QJsonObject json = QJsonDocument::fromJson(data, &parseError).object();
	if (parseError.error != QJsonParseError::NoError) {
		const NetworkError code = m_solverReply->error() != NoError ? m_solverReply->error() : ProtocolFailure;
		const QString reason = m_solverReply->error() != NoError ? m_solverReply->errorString() : QStringLiteral("not a FlareSolverr response");
		fail(code, QStringLiteral("FlareSolverr unavailable: %1").arg(reason));
		return;
	}

	if (json["status"].toString() != QLatin1String("ok")) {
		// Messages end in a browser stack trace, its line breaks escaped as a literal "\n"
		const QString message = json["message"].toString().replace(QStringLiteral("\\n"), QStringLiteral("\n")).section('\n', 0, 0).trimmed();
		fail(UnknownNetworkError, QStringLiteral("FlareSolverr could not load the page: %1").arg(message));
		return;
	}

	const QJsonObject solution = json["solution"].toObject();
	const int status = solution["status"].toInt();
	setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
	setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/html; charset=utf-8"));

	m_content = solution["response"].toString().toUtf8();
	setHeader(QNetworkRequest::ContentLengthHeader, m_content.size());

	if (status >= 400) {
		const NetworkError code = status == 401 ? AuthenticationRequiredError
			: status == 403 ? ContentAccessDenied
			: status == 404 ? ContentNotFoundError
			: status >= 500 ? InternalServerError
			: UnknownContentError;
		setError(code, QStringLiteral("HTTP %1 through FlareSolverr").arg(status));
	}

	emit readyRead();
	finish();
}

void FlareSolverrReply::fail(NetworkError code, const QString &message)
{
	log(QStringLiteral("%1 (`%2`)").arg(message, url().toString()), Logger::Warning);
	setError(code, message);
	emit errorOccurred(code);
	finish();
}

void FlareSolverrReply::finish()
{
	setFinished(true);
	emit finished();
}

void FlareSolverrReply::abort()
{
	if (isFinished()) {
		return;
	}

	if (!m_solverReply.isNull()) {
		m_solverReply->disconnect(this);
		m_solverReply->abort();
	}
	setError(OperationCanceledError, QStringLiteral("Operation canceled"));
	finish();
}

qint64 FlareSolverrReply::bytesAvailable() const
{
	return m_content.size() - m_offset + QNetworkReply::bytesAvailable();
}

bool FlareSolverrReply::isSequential() const
{
	return true;
}

qint64 FlareSolverrReply::readData(char *data, qint64 maxSize)
{
	if (m_offset >= m_content.size()) {
		return isFinished() ? -1 : 0;
	}

	const qint64 count = qMin(maxSize, m_content.size() - m_offset);
	memcpy(data, m_content.constData() + m_offset, count);
	m_offset += count;
	return count;
}
