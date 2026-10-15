#include "monitoring/monitoring-center.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QPointer>
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

	// QCoreApplication::exit() emits this before it makes every running and future event loop return
	// at once, which would otherwise let a check in progress record pages that never loaded as empty
	if (QCoreApplication::instance() != nullptr) {
		connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &MonitoringCenter::stop);
	}
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

bool MonitoringCenter::checkMonitor(Monitor &monitor, SearchQuery search, QStringList postFiltering)
{
	// The logins and page loads below wait in nested event loops, which run UI events: the monitor
	// lists can be edited (reallocating `monitor`) and the main window can close. So `monitor` is
	// only read before the first wait, and looked up again before the results are recorded.
	const Monitor *address = &monitor;
	const QString searchKey = search.toString();
	const QList<Site*> sites = monitor.sites();
	const int delay = monitor.delay();
	const QDateTime lastSuccess = monitor.lastSuccess();
	const QDateTime previousCheck = monitor.lastCheck();
	const bool download = monitor.download();
	const QDateTime limit = QDateTime::currentDateTimeUtc().addSecs(-delay);

	QPointer<MonitoringCenter> alive(this);

	QStringList siteNames;
	for (Site *site : sites) {
		siteNames.append(site->name());
	}

	emit statusChanged(monitor, MonitoringStatus::Checking);
	log(QStringLiteral("Monitoring new images for '%1' on '%2'").arg(searchKey, siteNames.join(", ")), Logger::Info);

	int count = 0;
	int newImages = 0;
	QList<QSharedPointer<Image>> newImagesList;
	QString state = "";
	bool success = true;
	const int perPage = m_profile->getSettings()->value("limit", 20).toInt();

	// A stopped check returns without recording anything: an interrupted wait looks like an empty
	// page, and recording it would move lastSuccess past posts that were never fetched
	auto interrupted = [&]() {
		if (!alive.isNull() && !alive->m_stop) {
			return false;
		}
		log(QStringLiteral("Monitoring check for '%1' interrupted, the monitor is left unchanged").arg(searchKey), Logger::Info);
		if (!alive.isNull()) {
			if (Monitor *current = alive->findMonitor(address, searchKey, sites)) {
				emit alive->statusChanged(*current, MonitoringStatus::Waiting);
			}
		}
		return true;
	};

	for (Site *site : sites) {
		// Create a pack loader, without a parent: the parent would delete this stack object if it went away during a wait
		DownloadQueryGroup query(m_profile->getSettings(), search, 1, perPage, MONITOR_CHECK_TOTAL, postFiltering, site);
		PackLoader loader(m_profile, query, perPage);

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
		if (interrupted()) {
			return false;
		}

		// Load all images
		bool firstRun = true;
		int countRun = 0;
		int newImagesRun = 0;
		while ((firstRun || download) && loader.hasNext() && newImagesRun == countRun) {
			// Load the next page
			QList<QSharedPointer<Image>> allImages = loader.next();
			if (interrupted()) {
				return false;
			}
			countRun += allImages.count();

			// Filter out old images
			for (const QSharedPointer<Image> &img : allImages) {
				if (img->createdAt() > lastSuccess && (delay <= 0 || img->createdAt() <= limit)) {
					newImagesList.append(img);
					newImagesRun++;
				}
			}
		}

		count += countRun;
		newImages += newImagesRun;

		if (countRun == 0) {
			log(QStringLiteral("No results for monitor '%1' on site '%2'").arg(searchKey, site->name()), Logger::Warning);
		}

		const QString newState = countRun == 0 ? "empty" : (newImagesRun == 0 ? "finished" : "ok");
		state = state.isEmpty() || state == newState ? newState : "mixed";
		success = success && started && !siteError;
	}

	// Not found means it was edited or removed during the waits; its unchanged lastSuccess re-finds these posts
	Monitor *target = findMonitor(address, searchKey, sites);
	if (target == nullptr) {
		log(QStringLiteral("Monitor '%1' changed while it was being checked, its results are discarded").arg(searchKey), Logger::Warning);
		return false;
	}

	emit statusChanged(*target, MonitoringStatus::Performing);

	// Send notification
	if (target->notify() && newImages > 0) {
		QString msg;
		if (count == 1) {
			msg = tr("New images found for tag '%1' on '%2'");
		} else if (newImages < count) {
			msg = tr("%n new image(s) found for tag '%1' on '%2'", "", newImages);
		} else {
			msg = tr("More than %n new image(s) found for tag '%1' on '%2'", "", newImages);
		}
		emit notify(*target, msg.arg(searchKey, siteNames.join(", ")));
	}

	// Add images to download queue
	QSharedPointer<PendingSuccess> pending;
	if (target->download() && newImages > 0) {
		QString path = target->pathOverride();
		if (path.isEmpty()) {
			path = m_profile->getSettings()->value("Save/path").toString();
		}
		QString filename = target->filenameOverride();
		if (filename.isEmpty()) {
			filename = m_profile->getSettings()->value("Save/filename").toString();
		}

		// lastSuccess is the "what counts as new" watermark, so it waits for these downloads:
		// moving it now would make an image that fails to download old before it ever arrived
		if (success) {
			pending.reset(new PendingSuccess { target, searchKey, sites, limit, previousCheck, {} });
			m_pendingSuccesses.append(pending);
		}

		for (const QSharedPointer<Image> &img : newImagesList) {
			auto *downloader = new ImageDownloader(m_profile, img, filename, path, 0, true, false, this);
			if (!target->getBlacklisted()) {
				downloader->setBlacklist(&m_profile->getBlacklist());
			}
			if (!pending.isNull()) {
				const QDateTime createdAt = img->createdAt();
				connect(downloader, &ImageDownloader::saved, this, [pending, createdAt](QSharedPointer<Image>, const QList<ImageSaveResult> &results) {
					for (const ImageSaveResult &res : results) {
						if (res.result == Image::SaveResult::NetworkError || res.result == Image::SaveResult::DetailsLoadError || res.result == Image::SaveResult::Error) {
							pending->failures.append(createdAt);
							return;
						}
					}
				});
			}
			m_downloadQueue->add(DownloadQueue::Background, downloader);
		}

		log(QStringLiteral("Added %1 images to the download queue for monitor '%2' on '%3'").arg(QString::number(newImages), searchKey, siteNames.join(", ")), Logger::Info);
		m_waitingForQueue = true;
	}

	// Update monitor
	target->setLastCheck(limit);
	if (success && pending.isNull()) {
		target->setLastSuccess(limit);
	}
	target->setLastState(state);
	target->setCumulated(target->cumulated() + newImages, count != 1 && newImages < count);
	m_changed = true;

	if (!m_waitingForQueue) {
		sync();
	}

	emit statusChanged(*target, MonitoringStatus::Waiting);

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

	// A check waits in nested event loops, so after one the lists may have been edited (indices
	// shifted or out of range, references dangling) and this object may be gone
	QPointer<MonitoringCenter> alive(this);
	auto monitorAt = [&favs, &monitors](const PendingMonitor &pm) -> Monitor * {
		if (pm.favIndex < 0) {
			return pm.monitorIndex < monitors.count() ? &monitors[pm.monitorIndex] : nullptr;
		}
		if (pm.favIndex >= favs.count()) {
			return nullptr;
		}
		QList<Monitor> &favMonitors = favs[pm.favIndex].getMonitors();
		return pm.monitorIndex < favMonitors.count() ? &favMonitors[pm.monitorIndex] : nullptr;
	};

	for (const PendingMonitor &pm : pending) {
		Monitor *monitor = monitorAt(pm);
		if (monitor == nullptr) {
			continue;
		}

		// If this monitor's interval expired, we check it for updates
		qint64 next = monitor->secsToNextCheck();
		if (next <= 0) {
			if (pm.favIndex < 0) {
				checkMonitor(*monitor, monitor->query(), monitor->postFilters());
			} else {
				checkMonitor(*monitor, favs[pm.favIndex]);
			}
			if (alive.isNull() || m_stop) {
				return;
			}
			monitor = monitorAt(pm);
			if (monitor == nullptr) {
				continue;
			}
			next = monitor->secsToNextCheck();
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
		applyPendingSuccesses();
		sync();
		tick();
	}
}

QDateTime MonitoringCenter::nextLastSuccess(const QDateTime &limit, const QList<QDateTime> &failures, const QDateTime &giveUpBefore)
{
	QDateTime oldest;
	for (const QDateTime &createdAt : failures) {
		const bool retry = createdAt.isValid() && (!giveUpBefore.isValid() || createdAt >= giveUpBefore);
		if (retry && (!oldest.isValid() || createdAt < oldest)) {
			oldest = createdAt;
		}
	}
	return oldest.isValid() ? qMin(limit, oldest.addMSecs(-1)) : limit;
}

Monitor *MonitoringCenter::findMonitor(const Monitor *address, const QString &search, const QList<Site*> &sites) const
{
	// Address alone could be reused after the user edits the lists, so the search and sites must match too
	auto matches = [&](const Monitor &monitor, const QString &monitorSearch) {
		return &monitor == address && monitor.sites() == sites && monitorSearch == search;
	};

	for (Favorite &favorite : m_profile->getFavorites()) {
		const QString search = SearchQuery(favorite.getName().split(' ', Qt::SkipEmptyParts)).toString();
		for (Monitor &monitor : favorite.getMonitors()) {
			if (matches(monitor, search)) {
				return &monitor;
			}
		}
	}
	for (Monitor &monitor : m_profile->monitorManager()->monitors()) {
		if (matches(monitor, monitor.query().toString())) {
			return &monitor;
		}
	}
	return nullptr;
}

void MonitoringCenter::applyPendingSuccesses()
{
	for (const QSharedPointer<PendingSuccess> &pending : qAsConst(m_pendingSuccesses)) {
		// Not found means the monitor was edited or removed meanwhile; its old lastSuccess re-finds these images
		Monitor *monitor = findMonitor(pending->monitor, pending->search, pending->sites);
		if (monitor == nullptr) {
			continue;
		}

		// Give up on a post only once an earlier check already tried it and it is past three days
		// (three intervals for slow monitors), so a first attempt after Grabber was off is never dropped
		const qint64 retryWindow = qMax<qint64>(3 * 24 * 60 * 60, 3 * static_cast<qint64>(monitor->interval()));
		QDateTime giveUpBefore = pending->limit.addSecs(-retryWindow);
		if (!pending->previousCheck.isValid()) {
			giveUpBefore = QDateTime();
		} else if (pending->previousCheck < giveUpBefore) {
			giveUpBefore = pending->previousCheck;
		}
		const QDateTime lastSuccess = nextLastSuccess(pending->limit, pending->failures, giveUpBefore);

		int retried = 0;
		for (const QDateTime &createdAt : pending->failures) {
			if (createdAt.isValid() && (!giveUpBefore.isValid() || createdAt >= giveUpBefore)) {
				retried++;
			}
		}
		if (retried > 0) {
			log(QStringLiteral("%1 image(s) failed to download for monitor '%2', its next check will retry them").arg(retried).arg(pending->search), Logger::Warning);
		}
		if (retried < pending->failures.count()) {
			log(QStringLiteral("%1 image(s) for monitor '%2' kept failing to download, giving up on them").arg(pending->failures.count() - retried).arg(pending->search), Logger::Warning);
		}

		if (lastSuccess > monitor->lastSuccess()) {
			monitor->setLastSuccess(lastSuccess);
			m_changed = true;
		}
	}
	m_pendingSuccesses.clear();
}

void MonitoringCenter::stop()
{
	// Closing the window and aboutToQuit both stop it
	if (m_stop) {
		return;
	}
	m_stop = true;

	log(QStringLiteral("Monitoring stopped"), Logger::Info);
}

bool MonitoringCenter::isRunning() const
{
	return !m_stop;
}
