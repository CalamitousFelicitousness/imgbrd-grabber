#ifndef FLARESOLVERR_REPLY_H
#define FLARESOLVERR_REPLY_H

#include <QByteArray>
#include <QNetworkReply>
#include <QPointer>
#include <QUrl>


class CustomNetworkAccessManager;
class QNetworkRequest;

/**
 * A GET answered by a FlareSolverr instance instead of the site, for pages behind a Cloudflare
 * wall that a plain HTTP client cannot pass. The solver's browser fetches the page and its HTML
 * is served as this reply's body.
 */
class FlareSolverrReply : public QNetworkReply
{
	Q_OBJECT

	public:
		FlareSolverrReply(const QNetworkRequest &request, const QUrl &solver, int msTimeout, CustomNetworkAccessManager *manager);
		~FlareSolverrReply() override;

		void abort() override;
		qint64 bytesAvailable() const override;
		bool isSequential() const override;

	protected:
		qint64 readData(char *data, qint64 maxSize) override;

	private:
		void solverFinished();
		void fail(NetworkError code, const QString &message);
		void finish();

		QPointer<QNetworkReply> m_solverReply;
		QByteArray m_content;
		qint64 m_offset = 0;
};

#endif // FLARESOLVERR_REPLY_H
