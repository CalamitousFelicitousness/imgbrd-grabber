#include "network-reply.h"
#include <chrono>
#include <utility>
#include "custom-network-access-manager.h"
#include "logger.h"


NetworkReply::NetworkReply(QNetworkRequest request, CustomNetworkAccessManager *manager, QObject *parent)
	: QObject(parent), m_request(std::move(request)), m_manager(manager)
{
	init();
}

NetworkReply::NetworkReply(QNetworkRequest request, QByteArray data, CustomNetworkAccessManager *manager, QObject *parent)
	: QObject(parent), m_request(std::move(request)), m_data(std::move(data)), m_manager(manager), m_post(true)
{
	init();
}

NetworkReply::~NetworkReply()
{
	abort();

	if (m_reply != nullptr) {
		m_reply->deleteLater();
		m_reply = nullptr;
	}
}


void NetworkReply::init()
{
	timer.setSingleShot(true);
	connect(&timer, &QTimer::timeout, this, &NetworkReply::startNow);
}


QUrl NetworkReply::url() const
{
	if (m_reply != nullptr) {
		return m_reply->url();
	}
	return m_request.url();
}

QVariant NetworkReply::attribute(QNetworkRequest::Attribute code) const
{
	if (m_reply != nullptr) {
		return m_reply->attribute(code);
	}
	return QVariant();
}

QByteArray NetworkReply::readAll()
{
	// A reply Qt aborted is closed before it emits finished(), and FileDownloader still drains it
	// there. Reading a closed QIODevice only warns and returns nothing.
	if (m_reply != nullptr && m_reply->isOpen()) {
		return m_reply->readAll();
	}
	return {};
}

qint64 NetworkReply::bytesAvailable() const
{
	if (m_reply != nullptr) {
		return m_reply->bytesAvailable();
	}
	return 0;
}

QNetworkReply::NetworkError NetworkReply::error() const
{
	if (m_reply != nullptr) {
		return m_reply->error();
	}
	if (m_aborted) {
		return QNetworkReply::NetworkError::OperationCanceledError;
	}
	return QNetworkReply::NetworkError::NoError;
}

QString NetworkReply::errorString() const
{
	if (m_reply != nullptr) {
		return m_reply->errorString();
	}
	return QString();
}

QNetworkReply *NetworkReply::networkReply() const
{
	return m_reply;
}

QByteArray NetworkReply::rawHeader(const QByteArray &headerName) const
{
	if (m_reply != nullptr) {
		return m_reply->rawHeader(headerName);
	}
	return {};
}


bool NetworkReply::isRunning() const
{
	// A reply that hasn't been actually started or aborted yet is considered as running
	if (!m_started && !m_aborted) {
		return true;
	}

	return m_reply != nullptr && m_reply->isRunning();
}


void NetworkReply::start(int msDelay)
{
	if (m_started) {
		return;
	}
	if (m_aborted) {
		m_aborted = false;
		return;
	}
	m_started = true;

	timer.setInterval(msDelay);
	timer.start();
}

void NetworkReply::setTransferTimeout(int msTimeout)
{
	m_transferTimeout = msTimeout;
}

void NetworkReply::startNow()
{
	// Qt waits forever by default, so a connection dropped without a close leaves a reply that
	// never emits finished(). NetworkManager only advances its queue on that signal, so a few of
	// them permanently deadlock every later request for the site. The clock resets on each chunk
	// received, so this bounds silence rather than total transfer time.
	if (m_transferTimeout > 0) {
		m_request.setTransferTimeout(std::chrono::milliseconds(m_transferTimeout));
	}

	if (m_post) {
		m_reply = m_manager->post(m_request, m_data);
	} else {
		m_reply = m_manager->get(m_request);
	}

	connect(m_reply, &QNetworkReply::readyRead, this, &NetworkReply::readyRead);
	connect(m_reply, &QNetworkReply::downloadProgress, this, &NetworkReply::downloadProgress);
	connect(m_reply, &QNetworkReply::redirected, this, &NetworkReply::redirected);
	connect(m_reply, &QNetworkReply::finished, this, &NetworkReply::replyFinished);

	m_reply->setParent(this);
}

void NetworkReply::replyFinished()
{
	// Qt aborts an expired transfer timeout with TimeoutError, not the OperationCanceledError
	// that abort() gives (qnetworkreplyhttpimpl.cpp `_q_transferTimedOut`).
	if (m_reply != nullptr && m_reply->error() == QNetworkReply::TimeoutError) {
		log(QStringLiteral("No data received for %1s, timed out `%2`").arg(QString::number(m_transferTimeout / 1000), m_reply->url().toString()), Logger::Warning);
	}

	emit finished();
}

void NetworkReply::abort()
{
	// Do nothing if we are already aborted or already running
	if (m_aborted || (m_reply != nullptr && !m_reply->isRunning())) {
		return;
	}

	m_aborted = true;
	if (m_reply != nullptr) {
		m_reply->abort();
	} else {
		emit finished();
	}
	if (timer.isActive()) {
		timer.stop();
	}
}
