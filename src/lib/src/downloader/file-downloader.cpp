#include "downloader/file-downloader.h"
#include <QFile>
#include "functions.h"
#include "logger.h"
#include "network/network-reply.h"

#define WRITE_BUFFER_SIZE (200 * 1024)


FileDownloader::FileDownloader(bool allowHtmlResponses, QObject *parent)
	: QObject(parent), m_allowHtmlResponses(allowHtmlResponses), m_reply(nullptr), m_readSize(0), m_writeError(false)
{}

void FileDownloader::setExpectedMd5(const QString &md5)
{
	m_expectedMd5 = md5;
}

bool FileDownloader::start(NetworkReply *reply, const QString &path)
{
	m_file.setFileName(path);
	const bool ok = m_file.open(QFile::WriteOnly | QFile::Truncate);

	m_readSize = 0;
	m_writeError = false;
	m_reply = reply;
	m_hash.reset();

	if (ok) {
		connect(reply, &NetworkReply::readyRead, this, &FileDownloader::replyReadyRead);
		connect(reply, &NetworkReply::finished, this, &FileDownloader::replyFinished);
	} else {
		log(QStringLiteral("Unable to open file '%1': %2 (%3)").arg(path, m_file.errorString(), QString::number(m_file.error())), Logger::Error);
	}

	return ok;
}


void FileDownloader::replyReadyRead()
{
	if (m_reply->bytesAvailable() < WRITE_BUFFER_SIZE) {
		return;
	}

	const QByteArray data = m_reply->readAll();
	m_readSize += data.size();
	m_hash.addData(data);

	if (m_file.write(data) < 0) {
		m_writeError = true;
		m_reply->abort();
	} else {
		m_file.flush();
	}
}

void FileDownloader::replyFinished()
{
	const QByteArray data = m_reply->readAll();
	m_readSize += data.size();
	m_hash.addData(data);

	const qint64 written = m_file.write(data);
	m_file.close();

	const auto error = m_reply->error();
	const auto msg = m_reply->errorString();
	const QUrl redirectUrl = m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
	const bool failedLastWrite = data.length() > 0 && written < 0;
	const bool invalidHtml = !m_allowHtmlResponses && isHtml(data) && redirectUrl.isEmpty();
	const bool emptyFile = m_readSize == 0 && redirectUrl.isEmpty();

	// A redirect nobody followed means the body we just wrote is the redirect page, not the
	// file. Sankaku answers expired signed links with a 302 whose 138-byte nginx body was
	// being saved as the image, because `invalidHtml` above deliberately excludes redirects.
	const bool unfollowedRedirect = !m_allowHtmlResponses && !redirectUrl.isEmpty();

	// Sankaku redirects an expired signed link to a 14,802-byte "Expired link" banner served
	// with 200 OK, so it cannot be recognised by status code - only by where it landed.
	const bool expiredLink = !m_allowHtmlResponses
		&& (m_reply->url().path().endsWith(QLatin1String("/expired.png"))
			|| redirectUrl.path().endsWith(QLatin1String("/expired.png")));

	// Opt-in integrity check: the bytes must hash to the md5 the API reported for this post.
	const bool md5Mismatch = !m_expectedMd5.isEmpty()
		&& QString::fromLatin1(m_hash.result().toHex()).compare(m_expectedMd5, Qt::CaseInsensitive) != 0;

	if (error != NetworkReply::NetworkError::NoError || failedLastWrite || invalidHtml || emptyFile || unfollowedRedirect || expiredLink || md5Mismatch) {
		// Ignore those errors as they are caused by a bug in Qt
		if (error != NetworkReply::NetworkError::NoError && msg.contains("140E0197")) {
			log(QStringLiteral("Ignored network error '140E0197' for the image: `%1`: %2 (%3)").arg(m_reply->url().toString()).arg(error).arg(msg), Logger::Info);
			emit success();
			return;
		}

		m_file.remove();
		if (failedLastWrite || m_writeError) {
			log(QStringLiteral("Unable to write file '%1': %2 (%3)").arg(m_file.fileName(), m_file.errorString(), QString::number(m_file.error())), Logger::Error);
			emit writeError();
		} else if (invalidHtml && error == NetworkReply::NetworkError::NoError) {
			log(QString("Invalid HTML content returned for url '%1'").arg(m_reply->url().toString()), Logger::Info);
			emit networkError(NetworkReply::NetworkError::ContentNotFoundError, "Invalid HTML content returned");
		} else if (emptyFile && error == NetworkReply::NetworkError::NoError) {
			log(QString("Empty file returned for url '%1'").arg(m_reply->url().toString()), Logger::Info);
			emit networkError(NetworkReply::NetworkError::ContentNotFoundError, "Empty file returned");
		} else if (expiredLink && error == NetworkReply::NetworkError::NoError) {
			log(QString("Expired link placeholder returned for url '%1'").arg(m_reply->url().toString()), Logger::Warning);
			emit networkError(NetworkReply::NetworkError::ContentNotFoundError, "Expired link placeholder returned");
		} else if (unfollowedRedirect && error == NetworkReply::NetworkError::NoError) {
			log(QString("Unfollowed redirect to '%1' for url '%2'").arg(redirectUrl.toString(), m_reply->url().toString()), Logger::Warning);
			emit networkError(NetworkReply::NetworkError::ContentNotFoundError, "Unfollowed redirect returned instead of the file");
		} else if (md5Mismatch && error == NetworkReply::NetworkError::NoError) {
			log(QString("MD5 mismatch for url '%1': expected %2, got %3").arg(m_reply->url().toString(), m_expectedMd5, QString::fromLatin1(m_hash.result().toHex())), Logger::Warning);
			emit networkError(NetworkReply::NetworkError::ContentNotFoundError, "Downloaded file does not match the expected MD5");
		} else {
			emit networkError(error, msg);
		}
		return;
	}

	emit success();
}
