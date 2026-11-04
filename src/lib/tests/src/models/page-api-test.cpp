#include <QEventLoop>
#include <QScopedPointer>
#include <QSettings>
#include <QTimer>
#include "custom-network-access-manager.h"
#include "models/page.h"
#include "models/page-api.h"
#include "models/profile.h"
#include "models/site.h"
#include "models/source.h"
#include "tags/tag.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("PageApi")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");

	setupSource("Gelbooru (0.2)");
	setupSite("Gelbooru (0.2)", "gelbooru.com");

	QString path = "tests/resources/sites/Danbooru (2.0)/danbooru.donmai.us/defaults.ini";
	QSettings settings(path, QSettings::IniFormat);
	settings.setValue("auth/pseudo", "user");
	settings.setValue("auth/apiKey", "test-api-key");
	settings.sync();

	const QScopedPointer<Profile> pProfile(makeProfile());
	auto *profile = pProfile.data();

	QList<Site*> sites { profile->getSites().value("danbooru.donmai.us") };
	REQUIRE(sites[0] != nullptr);

	SECTION("ParseUrlBasic")
	{
		Site *site = profile->getSites().value("gelbooru.com");
		REQUIRE(site != nullptr);

		QStringList tags = QStringList() << "test" << "tag";
		Page page(profile, site, sites, tags);
		PageApi pageApi(&page, profile, site, site->getApis().first(), tags);

		REQUIRE(pageApi.url().toString() == QString("https://gelbooru.com/index.php?page=dapi&s=post&q=index&limit=25&pid=0&tags=test tag"));
	}

	SECTION("ParseUrlLogin")
	{
		Site *site = sites.first();

		QStringList tags = QStringList() << "test" << "tag";
		Page page(profile, site, sites, tags);
		PageApi pageApi(&page, profile, site, site->getApis().first(), tags);

		REQUIRE(pageApi.url().toString() == QString("https://danbooru.donmai.us/posts.xml?limit=25&page=1&tags=test tag&login=user&api_key=test-api-key"));
	}

	SECTION("ParseUrlAltPage")
	{
		Site *site = sites.first();

		QStringList tags = QStringList() << "test" << "tag";
		Page prevPage(profile, site, sites, tags, 1000);
		Page page(profile, site, sites, tags, 1001);
		PageApi pageApi(&page, profile, site, site->getApis().first(), tags, 1001);
		pageApi.setLastPage(prevPage.pageInformation());

		REQUIRE(pageApi.url().toString() == QString("https://danbooru.donmai.us/posts.xml?limit=25&page=b0&tags=test tag&login=user&api_key=test-api-key"));
	}

	SECTION("Rate limit retries are capped")
	{
		Site *site = sites.first();
		site->setSetting("download/rate_limit_retries", 1, 5);
		site->setSetting("download/throttle_retry", 0, 60);
		site->loadConfig();

		CustomNetworkAccessManager::NextFiles.enqueue("503");
		CustomNetworkAccessManager::NextFiles.enqueue("503");

		QStringList tags = QStringList() << "test";
		Page page(profile, site, sites, tags);
		PageApi pageApi(&page, profile, site, site->getApis().first(), tags);

		bool finished = false;
		PageApi::LoadResult result = PageApi::LoadResult::Ok;
		QEventLoop loop;
		QObject::connect(&pageApi, &PageApi::finishedLoading, &loop, [&](PageApi *, PageApi::LoadResult status) {
			finished = true;
			result = status;
			loop.quit();
		});
		QTimer::singleShot(5000, &loop, &QEventLoop::quit);
		pageApi.load();
		loop.exec();

		site->setSetting("download/rate_limit_retries", 5, 5);
		site->setSetting("download/throttle_retry", 60, 60);
		site->loadConfig();

		REQUIRE(finished);
		REQUIRE(result == PageApi::LoadResult::Error);
		REQUIRE(CustomNetworkAccessManager::NextFiles.isEmpty());
		REQUIRE(pageApi.errors().count() == 1);
		REQUIRE(pageApi.errors().first().contains("giving up after 1 retries"));
	}
}
