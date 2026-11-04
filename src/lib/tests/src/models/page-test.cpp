#include <QScopedPointer>
#include <QSignalSpy>
#include "custom-network-access-manager.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/site.h"
#include "models/source.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("Page")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");

	setupSource("Gelbooru (0.2)");
	setupSite("Gelbooru (0.2)", "gelbooru.com");

	const QScopedPointer<Profile> pProfile(makeProfile());
	auto *profile = pProfile.data();

	QList<Site*> sites { profile->getSites().value("danbooru.donmai.us") };
	Site *site = profile->getSites().value("gelbooru.com");

	REQUIRE(site != nullptr);
	REQUIRE(sites[0] != nullptr);

	SECTION("IncompatibleModifiers")
	{
		Page page(profile, site, sites, QStringList() << "test" << "status:deleted");

		REQUIRE(page.search().count() == 1);
		REQUIRE(page.search().first() == QString("test"));
	}

	SECTION("LoadAbort")
	{
		Page page(profile, site, sites, QStringList() << "test" << "status:deleted");

		QSignalSpy spy(&page, SIGNAL(finishedLoading(Page*)));
		page.load();
		page.abort();
		REQUIRE(!spy.wait(1000));
	}

	SECTION("LoadAbortDoesNotFallBack")
	{
		CustomNetworkAccessManager::NextFiles.enqueue("hang");
		Page page(profile, site, sites, QStringList() << "test");

		QSignalSpy failed(&page, SIGNAL(failedLoading(Page*)));
		QSignalSpy finished(&page, SIGNAL(finishedLoading(Page*)));
		page.load();
		page.abort();

		REQUIRE(failed.count() == 1);
		REQUIRE(page.errors().contains("Loading aborted."));
		REQUIRE(!finished.wait(500));
		REQUIRE(failed.count() == 1);
		CustomNetworkAccessManager::NextFiles.clear();
	}

	SECTION("LoadTagsAbort")
	{
		Page page(profile, site, sites, QStringList() << "test" << "status:deleted");

		QSignalSpy spy(&page, SIGNAL(finishedLoadingTags(Page*)));
		page.loadTags();
		page.abortTags();
		REQUIRE(!spy.wait(1000));
	}
}
