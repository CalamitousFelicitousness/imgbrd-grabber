#ifndef EXIFTOOL_H
#define EXIFTOOL_H

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <functional>


class ExiftoolWorker;

class Exiftool : public QObject
{
	Q_OBJECT

	public:
		enum SidecarFile
		{
			No,
			OnError,
			Both,
			Only
		};
		Q_ENUM(SidecarFile)

		explicit Exiftool(QObject *parent = nullptr);
		~Exiftool() override;

		static QString version(int msecs = 30000);
		static bool isOnRemoteVolume(const QString &path);
		static QString sidecarPath(const QString &file, bool noExtension);

	public slots:
		bool start(int msecs = 30000);
		bool setMetadata(const QString &file, const QMap<QString, QString> &metadata, bool clear = false, bool keepColorProfile = true, SidecarFile sidecarFile = SidecarFile::OnError, bool sidecarFileNoExtension = false, int msecs = 30000);
		bool setSidecarList(const QString &file, const QString &tag, const QStringList &values, bool sidecarFileNoExtension = false, int msecs = 30000);
		bool execute(const QString &file, const QString &command, int msecs = 30000);
		bool stop(int msecs = 30000);

	private:
		// Runs `work` on the worker thread while the caller keeps its event loop spinning
		bool runOnWorker(const std::function<bool()> &work);

		QThread m_thread;
		ExiftoolWorker *m_worker;
};

#endif // EXIFTOOL_H
