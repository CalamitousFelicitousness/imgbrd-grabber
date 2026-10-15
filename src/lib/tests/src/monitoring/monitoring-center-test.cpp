#include <QDir>
#include <QFile>
#include <QPointer>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTimer>
#include <QTimeZone>
#include "custom-network-access-manager.h"
#include "downloader/download-queue.h"
#include "models/profile.h"
#include "models/site.h"
#include "models/source.h"
#include "monitoring/monitor.h"
#include "monitoring/monitor-manager.h"
#include "monitoring/monitoring-center.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("MonitoringCenter")
{
	SECTION("NextLastSuccess")
	{
		const QDateTime limit(QDate(2024, 6, 10), QTime(12, 0), QTimeZone::UTC);
		const QDateTime older(QDate(2024, 6, 9), QTime(8, 0), QTimeZone::UTC);
		const QDateTime newer(QDate(2024, 6, 9), QTime(20, 0), QTimeZone::UTC);
		const QDateTime giveUpBefore(QDate(2024, 6, 9), QTime(0, 0), QTimeZone::UTC);
		const QDateTime stale(QDate(2024, 6, 1), QTime(0, 0), QTimeZone::UTC);

		REQUIRE(MonitoringCenter::nextLastSuccess(limit, {}, giveUpBefore) == limit);
		REQUIRE(MonitoringCenter::nextLastSuccess(limit, { newer }, giveUpBefore) == newer.addMSecs(-1));
		REQUIRE(MonitoringCenter::nextLastSuccess(limit, { newer, older }, giveUpBefore) == older.addMSecs(-1));
		REQUIRE(MonitoringCenter::nextLastSuccess(limit, { stale }, giveUpBefore) == limit);
		REQUIRE(MonitoringCenter::nextLastSuccess(limit, { stale, newer }, giveUpBefore) == newer.addMSecs(-1));
		REQUIRE(MonitoringCenter::nextLastSuccess(limit, { stale }, QDateTime()) == stale.addMSecs(-1));
		REQUIRE(MonitoringCenter::nextLastSuccess(limit, { QDateTime() }, giveUpBefore) == limit);
	}

	SECTION("Downloads")
	{
		// MonitoringCenter::sync() writes monitors.json, which makeProfile() does not clean up
		QFile::remove("tests/resources/monitors.json");
		const QScopedPointer<Profile> pProfile(makeProfile());
		auto *profile = pProfile.data();

		setupSource("Danbooru (2.0)");
		setupSite("Danbooru (2.0)", "danbooru.donmai.us");
		Source source("tests/resources/sites/Danbooru (2.0)");
		Site site("danbooru.donmai.us", &source, profile);

		QSignalSpy loginSpy(&site, SIGNAL(loggedIn(Site*, Site::LoginResult)));
		QTimer::singleShot(0, &site, SLOT(login()));
		REQUIRE(loginSpy.wait());

		// Only the newer of the two posts in pack-loader-2-1.xml is new, so the check stops at page 1
		const QDateTime createdAt = QDateTime::fromString("2018-10-11T12:33:35-04:00", Qt::ISODate);
		const QDateTime lastSuccess = QDateTime::fromString("2018-10-11T12:20:00-04:00", Qt::ISODate);
		const QDateTime lastCheck = QDateTime::fromString("2018-10-11T12:25:00-04:00", Qt::ISODate);
		profile->monitorManager()->add(Monitor({ &site }, 3600, lastSuccess, lastCheck, true, "tests/resources/tmp", "%md5%.%ext%", 0, true, SearchQuery(QStringList { "tag" })));
		Monitor &monitor = profile->monitorManager()->monitors().first();

		DownloadQueue queue(1);
		MonitoringCenter center(profile, &queue);
		QSignalSpy queueSpy(&queue, SIGNAL(finished()));

		SECTION("Failed image stays new")
		{
			CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/pages/danbooru.donmai.us/pack-loader-2-1.xml");
			CustomNetworkAccessManager::NextFiles.enqueue("500");

			center.tick();
			REQUIRE(queueSpy.wait());

			REQUIRE(monitor.lastSuccess() == createdAt.addMSecs(-1));
			REQUIRE(monitor.lastCheck() > lastCheck);
		}

		SECTION("Image an earlier check already failed on is given up once old")
		{
			monitor.setLastCheck(QDateTime::currentDateTimeUtc().addDays(-1));
			CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/pages/danbooru.donmai.us/pack-loader-2-1.xml");
			CustomNetworkAccessManager::NextFiles.enqueue("500");

			center.tick();
			REQUIRE(queueSpy.wait());

			REQUIRE(monitor.lastSuccess() > createdAt);
		}

		SECTION("Stopping during a check leaves the monitor unchanged")
		{
			CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/pages/danbooru.donmai.us/pack-loader-2-1.xml");

			// Runs inside the check's first wait (the login)
			QTimer::singleShot(0, &center, &MonitoringCenter::stop);
			center.tick();

			REQUIRE(monitor.lastCheck() == lastCheck);
			REQUIRE(monitor.lastSuccess() == lastSuccess);
		}

		SECTION("Deleting the center during a check")
		{
			CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/pages/danbooru.donmai.us/pack-loader-2-1.xml");

			// Like the main window's WA_DeleteOnClose: processed by the first nested event loop the check enters
			auto *doomed = new MonitoringCenter(profile, &queue);
			QPointer<MonitoringCenter> guard(doomed);
			doomed->deleteLater();
			doomed->tick();

			REQUIRE(guard.isNull());
			REQUIRE(monitor.lastCheck() == lastCheck);
			REQUIRE(monitor.lastSuccess() == lastSuccess);
		}

		SECTION("Downloaded image moves lastSuccess")
		{
			CustomNetworkAccessManager::NextFiles.enqueue("tests/resources/pages/danbooru.donmai.us/pack-loader-2-1.xml");

			center.tick();
			REQUIRE(queueSpy.wait());

			REQUIRE(monitor.lastSuccess() > createdAt);
			QFile::remove("tests/resources/tmp/d598b15312f7949f6ee722ddadd24524.jpg");
			QFile::remove("tests/resources/tmp/d598b15312f7949f6ee722ddadd24524.png");
		}

		CustomNetworkAccessManager::NextFiles.clear();
		QFile::remove("tests/resources/monitors.json");
	}
}
