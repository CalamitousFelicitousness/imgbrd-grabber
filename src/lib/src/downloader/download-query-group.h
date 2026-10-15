#ifndef DOWNLOAD_QUERY_GROUP_H
#define DOWNLOAD_QUERY_GROUP_H

#include <QMetaType>
#include <QString>
#include <QStringList>
#include "downloader/download-query.h"
#include "models/search-query/search-query.h"


class Profile;
class QJsonObject;
class QSettings;
class Site;

class DownloadQueryGroup : public DownloadQuery
{
	public:
		// Constructors
		DownloadQueryGroup() = default;
		explicit DownloadQueryGroup(QSettings *settings, SearchQuery query, int page, int perPage, int total, QStringList postFiltering, Site *site);
		explicit DownloadQueryGroup(SearchQuery query, int page, int perPage, int total, QStringList postFiltering, bool getBlacklisted, Site *site, const QString &filename, const QString &path);

		// Serialization
		void write(QJsonObject &json) const override;
		void write(QJsonObject &json, bool saveProgress) const;
		bool read(const QJsonObject &json, Profile *profile) override;

		// Public members
		SearchQuery query;
		int page;
		int perpage;
		int total;
		QStringList postFiltering;
		bool getBlacklisted;
		bool galleriesCountAsOne = true;
		int progressVal = 0;
		bool progressFinished = false;

		// Where the download's row stands. Unknown leaves the table to work it out from
		// progressVal, which is all it can do for a batch that has not run this session.
		enum class Status
		{
			Unknown,
			Pending,
			Downloading,
			Done,
		};
		Status status = Status::Unknown;

		// What this run made of each image, shown on the download's row. Deliberately not
		// serialized: it describes a run, not the query.
		struct Counters
		{
			int downloaded = 0;
			int failed = 0;
			int notFound = 0;
			int alreadyExists = 0;
			int ignored = 0;
		};
		Counters counters;
};

bool operator==(const DownloadQueryGroup &lhs, const DownloadQueryGroup &rhs);
bool operator!=(const DownloadQueryGroup &lhs, const DownloadQueryGroup &rhs);

Q_DECLARE_METATYPE(DownloadQueryGroup)

#endif // DOWNLOAD_QUERY_GROUP_H
