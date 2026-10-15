#include <QIcon>
#include <QList>
#include <QModelIndex>
#include "download-group-table-model.h"
#include "downloader/download-query-group.h"
#include "catch.h"


static QImage iconImage(const QVariant &value)
{
	return value.value<QIcon>().pixmap(16, 16).toImage();
}

static QImage resourceImage(const QString &path)
{
	return QIcon(path).pixmap(16, 16).toImage();
}


TEST_CASE("DownloadGroupTableModel")
{
	QList<DownloadQueryGroup> downloads;

	DownloadQueryGroup group;
	group.page = 1;
	group.perpage = 20;
	group.total = 10;
	group.getBlacklisted = false;
	group.progressVal = 10; // Finished
	downloads.append(group);

	DownloadGroupTableModel model(nullptr, downloads);
	const QModelIndex status = model.index(0, 0);

	SECTION("A finished row without failures keeps the tick")
	{
		downloads[0].counters.downloaded = 10;

		REQUIRE(iconImage(model.data(status, Qt::DecorationRole)) == resourceImage(":/images/status/ok.png"));
	}

	SECTION("A finished row with failures is marked as an error")
	{
		downloads[0].counters.downloaded = 8;
		downloads[0].counters.failed = 2;

		REQUIRE(iconImage(model.data(status, Qt::DecorationRole)) == resourceImage(":/images/status/error.png"));
	}

	SECTION("Images that were never going to arrive are not failures")
	{
		downloads[0].counters.downloaded = 7;
		downloads[0].counters.notFound = 2;
		downloads[0].counters.ignored = 1;

		REQUIRE(iconImage(model.data(status, Qt::DecorationRole)) == resourceImage(":/images/status/ok.png"));
	}

	SECTION("An unfinished row keeps showing its progress")
	{
		downloads[0].progressVal = 4;
		downloads[0].counters.downloaded = 3;
		downloads[0].counters.failed = 1;

		REQUIRE(iconImage(model.data(status, Qt::DecorationRole)) == resourceImage(":/images/status/downloading.png"));
	}

	SECTION("The tooltip breaks the run down")
	{
		downloads[0].counters.downloaded = 6;
		downloads[0].counters.failed = 2;
		downloads[0].counters.notFound = 1;
		downloads[0].counters.alreadyExists = 1;

		const QString tooltip = model.data(status, Qt::ToolTipRole).toString();
		REQUIRE(tooltip == QStringLiteral("6 downloaded\n2 failed\n1 not found\n1 already on disk"));
	}

	SECTION("A row that has not run yet has nothing to say")
	{
		downloads[0].progressVal = 0;

		REQUIRE(!model.data(status, Qt::ToolTipRole).isValid());
	}

	SECTION("Status and counters stay with their own row")
	{
		// Three rows, each in a state no other row shares
		downloads.append(group);
		downloads.append(group);
		REQUIRE(model.setStatus(0, DownloadQueryGroup::Status::Pending));
		REQUIRE(model.setStatus(1, DownloadQueryGroup::Status::Done));
		REQUIRE(model.setStatus(2, DownloadQueryGroup::Status::Downloading));
		downloads[1].counters.failed = 3;

		SECTION("after the row above them is removed")
		{
			REQUIRE(model.removeRows(0, 1, QModelIndex()));

			REQUIRE(iconImage(model.data(model.index(0, 0), Qt::DecorationRole)) == resourceImage(":/images/status/error.png"));
			REQUIRE(model.data(model.index(0, 0), Qt::ToolTipRole).toString() == QStringLiteral("3 failed"));
			REQUIRE(iconImage(model.data(model.index(1, 0), Qt::DecorationRole)) == resourceImage(":/images/status/downloading.png"));
		}

		SECTION("after they are dragged into a new order")
		{
			REQUIRE(model.moveRows(QModelIndex(), 1, 1, QModelIndex(), 0));

			REQUIRE(iconImage(model.data(model.index(0, 0), Qt::DecorationRole)) == resourceImage(":/images/status/error.png"));
			REQUIRE(model.data(model.index(0, 0), Qt::ToolTipRole).toString() == QStringLiteral("3 failed"));
			REQUIRE(iconImage(model.data(model.index(1, 0), Qt::DecorationRole)) == resourceImage(":/images/status/pending.png"));
			REQUIRE(iconImage(model.data(model.index(2, 0), Qt::DecorationRole)) == resourceImage(":/images/status/downloading.png"));
		}
	}
}
