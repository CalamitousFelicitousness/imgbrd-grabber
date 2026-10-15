#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include "catch.h"
#include "external/exiftool.h"


static QString readTag(const QString &file, const QString &tag)
{
	QProcess p;
	p.start("exiftool", { "-s3", "-" + tag, file });
	if (!p.waitForFinished(30000)) {
		return {};
	}
	return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

static QStringList readList(const QString &file, const QString &tag)
{
	QProcess p;
	p.start("exiftool", { "-s3", "-charset", "UTF8", "-sep", "#SEP#", "-" + tag, file });
	if (!p.waitForFinished(30000)) {
		return {};
	}
	return QString::fromUtf8(p.readAllStandardOutput()).trimmed().split("#SEP#", Qt::SkipEmptyParts);
}

static bool stage(const QString &source, const QString &target)
{
	QFile::remove(target);
	if (!QFile::copy(source, target)) {
		return false;
	}
	return QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
}

TEST_CASE("Exiftool", "[.][exiftool]")
{
	const QString remoteDir = QStringLiteral("Z:/_exiftool-test");

	SECTION("Remote volume detection")
	{
		REQUIRE(!Exiftool::isOnRemoteVolume(QDir::tempPath() + "/probe.png"));
		REQUIRE(Exiftool::isOnRemoteVolume("//server/share/probe.png"));
		REQUIRE(Exiftool::isOnRemoteVolume("\\\\server\\share\\probe.png"));
	}

	SECTION("Writes a list into the sidecar beside existing metadata")
	{
		const QString dir = QDir::tempPath() + "/_exiftool-sidecar-test";
		REQUIRE(QDir().mkpath(dir));
		const QString target = dir + "/probe.png";
		REQUIRE(stage("tests/resources/image_200x200.png", target));
		const QString sidecar = target + ".xmp";
		QFile::remove(sidecar);
		QFile::remove(sidecar + "_original");
		const qint64 sizeBefore = QFileInfo(target).size();

		const QStringList values {
			QStringLiteral("Booru|copyright|steins;gate"),
			QStringLiteral("Booru|copyright|fate/grand order"),
			QStringLiteral("Booru|character|pokémon trainer"),
		};

		Exiftool exiftool;
		REQUIRE(exiftool.start());
		REQUIRE(exiftool.setSidecarList(target, "XMP-lr:HierarchicalSubject", values));
		REQUIRE(exiftool.setMetadata(target, {{ "DateTimeOriginal", "2020:01:02 03:04:05" }}, false, true, Exiftool::SidecarFile::Only));
		REQUIRE(exiftool.setSidecarList(target, "XMP-lr:HierarchicalSubject", values));
		exiftool.stop();

		REQUIRE(readTag(sidecar, "DateTimeOriginal") == "2020:01:02 03:04:05");
		REQUIRE(readList(sidecar, "HierarchicalSubject") == values);
		REQUIRE(!QFile::exists(sidecar + "_original"));
		REQUIRE(QFileInfo(target).size() == sizeBefore);

		QFile::remove(target);
		QFile::remove(sidecar);
		QDir().rmdir(dir);
	}

	SECTION("Writes metadata to a file on a remote volume")
	{
		if (!Exiftool::isOnRemoteVolume(remoteDir)) {
			WARN("Z: is not a remote volume on this machine, skipping");
			return;
		}
		REQUIRE(QDir().mkpath(remoteDir));
		const QString target = remoteDir + "/probe.png";
		REQUIRE(stage("tests/resources/image_200x200.png", target));
		const qint64 sizeBefore = QFileInfo(target).size();

		Exiftool exiftool;
		REQUIRE(exiftool.start());
		const bool ok = exiftool.setMetadata(target, {{ "DateTimeOriginal", "2020:01:02 03:04:05" }}, false, true, Exiftool::SidecarFile::No);
		exiftool.stop();

		REQUIRE(ok);
		REQUIRE(readTag(target, "DateTimeOriginal") == "2020:01:02 03:04:05");
		REQUIRE(QFileInfo(target).size() >= sizeBefore);

		// The swap must not leave anything behind
		REQUIRE(!QFile::exists(target + ".exifnew"));
		REQUIRE(!QFile::exists(target + ".exifold"));

		QFile::remove(target);
		QDir().rmdir(remoteDir);
	}

	SECTION("Keeps the caller's event loop running while writing")
	{
		if (!Exiftool::isOnRemoteVolume(remoteDir)) {
			WARN("Z: is not a remote volume on this machine, skipping");
			return;
		}
		REQUIRE(QDir().mkpath(remoteDir));
		const QString target = remoteDir + "/spin.png";
		REQUIRE(stage("tests/resources/image_200x200.png", target));

		// A blocking implementation would starve this timer for the whole write
		int ticks = 0;
		QTimer ticker;
		ticker.setInterval(5);
		QObject::connect(&ticker, &QTimer::timeout, [&ticks]() { ticks++; });
		ticker.start();

		Exiftool exiftool;
		REQUIRE(exiftool.start());
		REQUIRE(exiftool.setMetadata(target, {{ "DateTimeOriginal", "2020:01:02 03:04:05" }}, false, true, Exiftool::SidecarFile::No));
		exiftool.stop();
		ticker.stop();

		REQUIRE(ticks > 0);

		QFile::remove(target);
		QDir().rmdir(remoteDir);
	}

	SECTION("Leaves the original untouched when the write fails")
	{
		if (!Exiftool::isOnRemoteVolume(remoteDir)) {
			WARN("Z: is not a remote volume on this machine, skipping");
			return;
		}
		REQUIRE(QDir().mkpath(remoteDir));

		// exiftool cannot write tags into a text file, so this exercises the failure path
		const QString target = remoteDir + "/probe.txt";
		QFile::remove(target);
		QFile f(target);
		REQUIRE(f.open(QFile::WriteOnly));
		f.write("not an image");
		f.close();
		const qint64 sizeBefore = QFileInfo(target).size();

		Exiftool exiftool;
		REQUIRE(exiftool.start());
		const bool ok = exiftool.setMetadata(target, {{ "DateTimeOriginal", "2020:01:02 03:04:05" }}, false, true, Exiftool::SidecarFile::No);
		exiftool.stop();

		REQUIRE(!ok);
		REQUIRE(QFile::exists(target));
		REQUIRE(QFileInfo(target).size() == sizeBefore);
		REQUIRE(!QFile::exists(target + ".exifnew"));
		REQUIRE(!QFile::exists(target + ".exifold"));

		QFile::remove(target);
		QDir().rmdir(remoteDir);
	}
}
