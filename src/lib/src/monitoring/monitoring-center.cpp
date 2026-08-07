#include "monitoring/monitoring-center.h"
#include <QDateTime>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include "downloader/download-query-group.h"
#include "downloader/download-queue.h"
#include "downloader/image-downloader.h"
#include "loader/pack-loader.h"
#include "logger.h"
#include "models/favorite.h"
#include "models/image.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/search-query/tag-search-query.h"
#include "models/site.h"
#include "monitoring/monitor.h"
#include "monitoring/monitor-manager.h"

#define MONITOR_CHECK_TOTAL 1000


MonitoringCenter::MonitoringCenter(Profile *profile, DownloadQueue *downloadQueue, QObject *parent)
	: QObject(parent), m_profile(profile), m_downloadQueue(downloadQueue)
{
	connect(m_downloadQueue, &DownloadQueue::finished, this, &MonitoringCenter::queueEmpty);
}

void MonitoringCenter::start()
{
	log(QStringLiteral("Monitoring starting"), Logger::Info);

	m_stop = false;
	int secsDelay = m_profile->getSettings()->value("Monitoring/startupDelay", 0).toInt();
	QTimer::singleShot(secsDelay * 1000, this, SLOT(tick()));
}

void MonitoringCenter::checkMonitor(Monitor &monitor, const Favorite &favorite)
{
	bool newImages = checkMonitor(monitor, favorite.getName().split(' ', Qt::SkipEmptyParts), favorite.getPostFiltering());
	if (newImages) {
		emit m_profile->favoritesChanged();
	}
}

bool MonitoringCenter::checkMonitor(Monitor &monitor, const SearchQuery &search, const QStringList &postFiltering)
{
	const int delay = monitor.delay();
	const QDateTime limit = QDateTime::currentDateTimeUtc().addSecs(-delay);

	QStringList siteNames;
	for (Site *site : monitor.sites()) {
		siteNames.append(site->name());
	}

	emit statusChanged(monitor, MonitoringStatus::Checking);
	log(QStringLiteral("Monitoring new images for '%1' on '%2'").arg(search.toString(), siteNames.join(", ")), Logger::Info);

	int count = 0;
	int newImages = 0;
	QList<QSharedPointer<Image>> newImagesList;
	QString state = "";
	bool success = true;
	const int perPage = m_profile->getSettings()->value("limit", 20).toInt();

	for (Site *site : monitor.sites()) {
		// Create a pack loader
		DownloadQueryGroup query(m_profile->getSettings(), search, 1, perPage, MONITOR_CHECK_TOTAL, postFiltering, site);
		PackLoader loader(m_profile, query, perPage, this);

		// A site that simply has no posts for this tag is NOT a failure - only a real page
		// error is. Counting "no results" as failure froze lastSuccess permanently for any
		// monitor with a site that never matches (measured here: 113 of 666 monitors, the
		// worst stuck 108 days). Since lastSuccess is the watermark for "what counts as new",
		// those monitors re-discovered their entire history on every single check, re-queued
		// all of it, and halted the monitoring sweep each time while the queue drained.
		bool siteError = false;
		connect(&loader, &PackLoader::finishedPage, this, [&siteError](Page *page) {
			if (!page->errors().isEmpty()) {
				siteError = true;
			}
		});

		const bool started = loader.start();

		// Load all images
		bool firstRun = true;
		int countRun = 0;
		int newImagesRun = 0;
		while ((firstRun || monitor.download()) && loader.hasNext() && newImagesRun == countRun) {
			// Load the next page
			QList<QSharedPointer<Image>> allImages = loader.next();
			countRun += allImages.count();

			// Filter out old images
			for (const QSharedPointer<Image> &img : allImages) {
				if (img->createdAt() > monitor.lastSuccess() && (delay <= 0 || img->createdAt() <= limit)) {
					newImagesList.append(img);
					newImagesRun++;
				}
			}
		}

		count += countRun;
		newImages += newImagesRun;

		if (countRun == 0) {
			log(QStringLiteral("No results for monitor '%1' on site '%2'").arg(search.toString(), site->name()), Logger::Warning);
		}

		const QString newState = countRun == 0 ? "empty" : (newImagesRun == 0 ? "finished" : "ok");
		state = state.isEmpty() || state == newState ? newState : "mixed";
		success = success && started && !siteError;
	}

	emit statusChanged(monitor, MonitoringStatus::Performing);

	// Send notification
	if (monitor.notify() && newImages > 0) {
		QString msg;
		if (count == 1) {
			msg = tr("New images found for tag '%1' on '%2'");
		} else if (newImages < count) {
			msg = tr("%n new image(s) found for tag '%1' on '%2'", "", newImages);
		} else {
			msg = tr("More than %n new image(s) found for tag '%1' on '%2'", "", newImages);
		}
		emit notify(monitor, msg.arg(search.toString(), siteNames.join(", ")));
	}

	// Add images to download queue
	if (monitor.download() && newImages > 0) {
		QString path = monitor.pathOverride();
		if (path.isEmpty()) {
			path = m_profile->getSettings()->value("Save/path").toString();
		}
		QString filename = monitor.filenameOverride();
		if (filename.isEmpty()) {
			filename = m_profile->getSettings()->value("Save/filename").toString();
		}

		for (const QSharedPointer<Image> &img : newImagesList) {
			auto *downloader = new ImageDownloader(m_profile, img, filename, path, 0, true, false, this);
			if (!monitor.getBlacklisted()) {
				downloader->setBlacklist(&m_profile->getBlacklist());
			}
			m_downloadQueue->add(DownloadQueue::Background, downloader);
		}

		log(QStringLiteral("Added %1 images to the download queue for monitor '%2' on '%3'").arg(QString::number(newImages), search.toString(), siteNames.join(", ")), Logger::Info);
		m_waitingForQueue = true;
	}

	// Update monitor
	monitor.setLastCheck(limit);
	if (success) {
		monitor.setLastSuccess(limit);
	}
	monitor.setLastState(state);
	monitor.setCumulated(monitor.cumulated() + newImages, count != 1 && newImages < count);
	m_changed = true;

	if (!m_waitingForQueue) {
		sync();
	}

	emit statusChanged(monitor, MonitoringStatus::Waiting);

	return newImages > 0;
}

void MonitoringCenter::sync()
{
	// Save only if there were changes to the monitors
	if (m_changed) {
		m_profile->syncFavorites();
		m_profile->monitorManager()->save();
		m_changed = false;
	}
}

void MonitoringCenter::tick()
{
	if (m_stop) {
		return;
	}

	qint64 minNextMonitoring = -1;
	log(QStringLiteral("Monitoring tick"), Logger::Info);

	// Build the combined work list (favourite monitors + standalone monitors) and walk it
	// most-overdue-first.
	//
	// Both lists used to be walked in file order. Because a monitor that finds new images
	// aborts the whole sweep (see the m_waitingForQueue return below) and the next tick
	// restarts from the top, a monitor low in the file was only ever reached when every
	// earlier one happened to have nothing new - so with a large monitor set and sessions
	// shorter than a full sweep, the tail could go days without being checked at all.
	// Ordering by due time makes position in the file irrelevant: whatever is most overdue
	// goes first, and restarting the sweep from the top is then always the correct thing.
	struct PendingMonitor
	{
		int favIndex;      // -1 for a standalone monitor
		int monitorIndex;
		bool forced;       // user pressed "Start now" on it
		qint64 next;       // cached: a comparator calling secsToNextCheck() live would be
		                   // unstable as the clock moves, breaking strict weak ordering
	};

	// secsToNextCheck() returns a flat -1 for force-run monitors, which would sort them AFTER
	// naturally overdue ones (-1 > -79200 for a monitor 22h late) and, being all equal, make
	// them fall back to file order. So rank by real staleness here and keep "forced" separate.
	const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
	auto staleness = [&nowUtc](const Monitor &m) {
		return nowUtc.secsTo(m.lastCheck().addSecs(m.interval()));
	};

	QList<Favorite> &favs = m_profile->getFavorites();
	QList<Monitor> &monitors = m_profile->monitorManager()->monitors();

	QList<PendingMonitor> pending;
	for (int j = 0; j < favs.count(); ++j) {
		QList<Monitor> &favMonitors = favs[j].getMonitors();
		for (int k = 0; k < favMonitors.count(); ++k) {
			pending.append({ j, k, favMonitors[k].forceRun(), staleness(favMonitors[k]) });
		}
	}
	for (int k = 0; k < monitors.count(); ++k) {
		pending.append({ -1, k, monitors[k].forceRun(), staleness(monitors[k]) });
	}

	std::stable_sort(pending.begin(), pending.end(), [](const PendingMonitor &a, const PendingMonitor &b) {
		// An explicit "Start now" outranks the schedule, but within either group the most
		// overdue still goes first.
		if (a.forced != b.forced) {
			return a.forced;
		}
		return a.next < b.next;
	});

	for (const PendingMonitor &pm : pending) {
		Monitor &monitor = pm.favIndex < 0
			? monitors[pm.monitorIndex]
			: favs[pm.favIndex].getMonitors()[pm.monitorIndex];

		// If this monitor's interval expired, we check it for updates
		qint64 next = monitor.secsToNextCheck();
		if (next <= 0) {
			if (pm.favIndex < 0) {
				checkMonitor(monitor, monitor.query(), monitor.postFilters());
			} else {
				checkMonitor(monitor, favs[pm.favIndex]);
			}
			next = monitor.secsToNextCheck();
		}

		if (m_waitingForQueue) {
			return;
		}

		// Only keep the soonest expiring timeout
		if (next < minNextMonitoring || minNextMonitoring == -1) {
			minNextMonitoring = next;
		}
	}

	// Re-run this method as soon as one of the monitoring timeout expires
	if (minNextMonitoring > 0) {
		log(QStringLiteral("Next monitoring will be in %1 seconds").arg(minNextMonitoring), Logger::Info);
		QTimer::singleShot(minNextMonitoring * 1000, this, SLOT(tick()));
	} else {
		log(QStringLiteral("Monitoring finished"), Logger::Info);
	}
}

void MonitoringCenter::queueEmpty()
{
	if (m_waitingForQueue) {
		m_waitingForQueue = false;
		sync();
		tick();
	}
}

void MonitoringCenter::stop()
{
	m_stop = true;

	log(QStringLiteral("Monitoring stopped"), Logger::Info);
}

bool MonitoringCenter::isRunning() const
{
	return !m_stop;
}
