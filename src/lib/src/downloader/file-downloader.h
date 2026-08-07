#ifndef FILE_DOWNLOADER_H
#define FILE_DOWNLOADER_H

#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QString>
#include "network/network-reply.h"

class FileDownloader : public QObject
{
	Q_OBJECT

	public:
		explicit FileDownloader(bool allowHtmlResponses, QObject *parent = nullptr);
		bool start(NetworkReply *reply, const QString &path);

		// When set, the downloaded bytes must hash to this md5 or the file is rejected.
		// Pass an empty string to disable the check for the next download.
		void setExpectedMd5(const QString &md5);

	signals:
		void writeError();
		void networkError(NetworkReply::NetworkError error, const QString &errorString);
		void success();

	private slots:
		void replyReadyRead();
		void replyFinished();

	private:
		bool m_allowHtmlResponses;
		NetworkReply *m_reply;
		QFile m_file;
		int m_readSize;
		bool m_writeError;
		QString m_expectedMd5;
		QCryptographicHash m_hash { QCryptographicHash::Md5 };
};

#endif // FILE_DOWNLOADER_H
