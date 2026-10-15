#ifndef MONITORING_CENTER_H
#define MONITORING_CENTER_H

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QSharedPointer>
#include <QString>


class DownloadQueue;
class Favorite;
class ImageDownloader;
class Monitor;
class Profile;
class SearchQuery;
class Site;

class MonitoringCenter : public QObject
{
	Q_OBJECT
	Q_ENUMS(LoadResult)

	public:
		enum MonitoringStatus
		{
			Waiting,
			Checking,
			Performing,
		};

		explicit MonitoringCenter(Profile *profile, DownloadQueue *downloadQueue, QObject *parent = nullptr);
		bool isRunning() const;

		// lastSuccess once a check's downloads are done: just before the oldest failed post not older than `giveUpBefore` (invalid: none given up)
		static QDateTime nextLastSuccess(const QDateTime &limit, const QList<QDateTime> &failures, const QDateTime &giveUpBefore);

	public slots:
		void start();
		void stop();
		void tick();

	protected slots:
		void queueEmpty();

	protected:
		void checkMonitor(Monitor &monitor, const Favorite &favorite);
		bool checkMonitor(Monitor &monitor, SearchQuery search, QStringList postFiltering);
		void sync();
		void applyPendingSuccesses();

	signals:
		void statusChanged(const Monitor &monitor, MonitoringCenter::MonitoringStatus status);
		void notify(const Monitor &monitor, const QString &message);

	private:
		// A check whose lastSuccess waits for its queued downloads
		struct PendingSuccess
		{
			const Monitor *monitor; // Identity only; findMonitor() checks it still exists
			QString search;
			QList<Site*> sites;
			QDateTime limit;
			QDateTime previousCheck; // Posts older than this were already queued by an earlier check
			QList<QDateTime> failures; // createdAt of each image that failed to download
		};
		Monitor *findMonitor(const Monitor *address, const QString &search, const QList<Site*> &sites) const;

		Profile *m_profile;
		DownloadQueue *m_downloadQueue;
		QList<QSharedPointer<PendingSuccess>> m_pendingSuccesses;
		bool m_waitingForQueue = false;
		bool m_changed = false;
		bool m_stop = false;
};

#endif // MONITORING_CENTER_H
