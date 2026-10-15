#ifndef CUSTOMNETWORKACCESSMANAGER_H
#define CUSTOMNETWORKACCESSMANAGER_H

#include <QNetworkAccessManager>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QUrl>


class QNetworkReply;
class QNetworkRequest;
class QSslError;

class CustomNetworkAccessManager : public QNetworkAccessManager
{
	Q_OBJECT

	public:
		explicit CustomNetworkAccessManager(QObject *parent = nullptr);
		QNetworkReply *get(const QNetworkRequest &request);
		QNetworkReply *post(const QNetworkRequest &request, const QByteArray &data);
		void setIgnoreSslErrors(bool ignore);
		void setFlareSolverr(const QUrl &solver, const QStringList &hosts, int msTimeout);
		void sslErrorHandler(QNetworkReply *reply, const QList<QSslError> &errors);

		static QQueue<QString> NextFiles;

	protected:
		QNetworkReply *makeErrorReply(const QNetworkRequest &request, const QString &code = QString());
		QNetworkReply *makeTestReply(const QNetworkRequest &request);

	private:
		bool m_ignoreSslErrors = false;
		QUrl m_flareSolverr;
		QStringList m_flareSolverrHosts;
		int m_flareSolverrTimeout = 60000;
};

#endif // CUSTOMNETWORKACCESSMANAGER_H
