#include "exiftool.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QProcess>
#include <QStorageInfo>
#include <QUuid>
#include "logger.h"
#ifdef Q_OS_WIN
	#include <Windows.h>
#endif


/**
 * Owns the exiftool process and every blocking call made against it.
 *
 * Lives on Exiftool's worker thread, so the process is only ever touched from there and the
 * queued calls that reach it are already serialised.
 */
class ExiftoolWorker : public QObject
{
	public:
		explicit ExiftoolWorker(QObject *parent = nullptr)
			: QObject(parent)
		{}

		~ExiftoolWorker() override
		{
			#ifdef Q_OS_WIN
				if (m_job != nullptr) {
					CloseHandle(m_job);
					m_job = nullptr;
				}
			#endif
		}

		// Created here rather than in the constructor so the process belongs to the worker thread
		bool start(int msecs)
		{
			if (m_process == nullptr) {
				m_process = new QProcess(this);
				connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
					const QString output = QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed();
					const Logger::LogLevel logLevel = output.startsWith("Warning:") ? Logger::Warning : Logger::Error;
					log(QString("[Exiftool] %1").arg(output), logLevel);
				});
			}

			if (m_process->state() != QProcess::NotRunning) {
				return true;
			}

			m_process->start("exiftool", { "-stay_open", "True", "-@", "-" });
			const bool ok = m_process->waitForStarted(msecs);

			if (!ok) {
				log(QStringLiteral("Error starting exiftool: %1 (%2)").arg(m_process->errorString()).arg(m_process->error()));
				return false;
			}

			adoptIntoJob();
			return true;
		}

		/**
		 * Tie the exiftool process to this one so the kernel reaps it.
		 *
		 * `-stay_open True` makes exiftool ignore its stdin closing, so it only exits on an explicit
		 * `-stay_open False` from ~Profile. Every path that skips that - a crash, or the user killing
		 * a frozen Grabber - leaves it running and holding its accumulated memory.
		 */
		void adoptIntoJob()
		{
			#ifdef Q_OS_WIN
				if (m_job == nullptr) {
					m_job = CreateJobObjectW(nullptr, nullptr);
					if (m_job == nullptr) {
						log(QStringLiteral("[Exiftool] Could not create a job object (%1)").arg(static_cast<qulonglong>(GetLastError())), Logger::Debug);
						return;
					}

					JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
					limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
					if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
						log(QStringLiteral("[Exiftool] Could not configure the job object (%1)").arg(static_cast<qulonglong>(GetLastError())), Logger::Debug);
						CloseHandle(m_job);
						m_job = nullptr;
						return;
					}
				}

				HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(m_process->processId()));
				if (process == nullptr) {
					log(QStringLiteral("[Exiftool] Could not open the exiftool process (%1)").arg(static_cast<qulonglong>(GetLastError())), Logger::Debug);
					return;
				}
				if (!AssignProcessToJobObject(m_job, process)) {
					log(QStringLiteral("[Exiftool] Could not adopt exiftool into the job object (%1)").arg(static_cast<qulonglong>(GetLastError())), Logger::Debug);
				}
				CloseHandle(process);
			#endif
		}

		bool stop(int msecs)
		{
			if (m_process == nullptr || m_process->state() == QProcess::NotRunning) {
				return true;
			}

			m_process->write("-stay_open\nFalse\n");
			m_process->closeWriteChannel();
			return m_process->waitForFinished(msecs);
		}

		bool execute(const QString &file, const QString &command, int msecs)
		{
			if (m_process == nullptr || m_process->state() != QProcess::Running) {
				log(QStringLiteral("Cannot execute command since Exiftool is not running"));
				return false;
			}

			const QString toWrite = (command.isEmpty() ? "" : command + "\n") + file + "\n-execute\n";
			m_process->write(toWrite.toUtf8());

			m_process->setReadChannel(QProcess::StandardOutput);

			while (m_process->waitForReadyRead(msecs)) {
				const QString output = QString::fromLocal8Bit(m_process->readAllStandardOutput()).trimmed();
				log(QString("[Exiftool] %1").arg(output), Logger::Debug);
				if (output.endsWith("{ready}")) {
					return !output.contains("due to errors");
				}
			}

			return false;
		}

		/**
		 * Run a command against a local copy of the file, then put the result back.
		 *
		 * `-overwrite_original` rewrites the whole file in many small reads and writes, and on a
		 * network volume each one costs a round trip: a 71 MB video measured 37.6s on SMB against
		 * 0.85s on local disk, while a bulk copy of the same file took 0.2s. Staging also keeps the
		 * operation inside the `execute` timeout, past which the write is abandoned and only a
		 * sidecar is left behind.
		 */
		bool executeOnCopy(const QString &file, const QString &command, int msecs)
		{
			const QFileInfo info(file);
			const QString local = QDir::tempPath() + QDir::separator()
				+ QUuid::createUuid().toString(QUuid::WithoutBraces) + "." + info.suffix();

			if (!QFile::copy(file, local)) {
				log(QStringLiteral("[Exiftool] Could not stage `%1` locally, writing over the network instead").arg(file), Logger::Debug);
				return execute(file, command, msecs);
			}

			QElapsedTimer timer;
			timer.start();
			if (!execute(local, command, msecs)) {
				QFile::remove(local);
				return false;
			}

			// Carried onto the final file below, so staging leaves the timestamp an in-place run would
			const QDateTime stagedTime = QFileInfo(local).lastModified();

			// Swap the staged file in via renames so the original survives every failure
			const QString incoming = file + ".exifnew";
			const QString outgoing = file + ".exifold";
			QFile::remove(incoming);
			QFile::remove(outgoing);

			if (!QFile::copy(local, incoming)) {
				log(QStringLiteral("[Exiftool] Could not copy `%1` back to `%2`").arg(local, incoming), Logger::Error);
				QFile::remove(local);
				return false;
			}
			QFile::remove(local);

			if (!QFile::rename(file, outgoing)) {
				log(QStringLiteral("[Exiftool] Could not move `%1` aside").arg(file), Logger::Error);
				QFile::remove(incoming);
				return false;
			}
			if (!QFile::rename(incoming, file)) {
				log(QStringLiteral("[Exiftool] Could not put `%1` in place, restoring the original").arg(file), Logger::Error);
				QFile::rename(outgoing, file);
				QFile::remove(incoming);
				return false;
			}
			QFile::remove(outgoing);

			QFile placed(file);
			if (stagedTime.isValid() && placed.open(QIODevice::ReadWrite)) {
				placed.setFileTime(stagedTime, QFileDevice::FileModificationTime);
				placed.close();
			}

			log(QStringLiteral("[Exiftool] Wrote metadata to `%1` via a local copy in %2 ms").arg(file).arg(timer.elapsed()), Logger::Debug);
			return true;
		}

		bool setMetadata(const QString &file, const QMap<QString, QString> &metadata, bool clear, bool keepColorProfile, Exiftool::SidecarFile sidecarFile, bool sidecarFileNoExtension, int msecs)
		{
			QStringList commands;
			commands.append({ "-charset", "filename=utf8" });
			commands.append({ "-sep", ";" });
			for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) {
				commands.append("-" + it.key() + "=" + it.value());
			}
			commands.append("-m"); // Ignore minor errors

			// Try to write metadata to the file first, unless we only want to write to the sidecar
			if (sidecarFile != Exiftool::SidecarFile::Only) {
				QStringList originalFileCommands(commands);
				if (clear) {
					originalFileCommands.insert(2, "-all=");
					if (keepColorProfile) {
						originalFileCommands.insert(3, QStringList{ "--icc_profile:all", "-tagsfromfile", "@", "-colorspacetags" }.join("\n"));
					}
				}
				originalFileCommands.append("-overwrite_original");

				const QString originalFileCommand = originalFileCommands.join("\n");
				const bool ok = Exiftool::isOnRemoteVolume(file)
					? executeOnCopy(file, originalFileCommand, msecs)
					: execute(file, originalFileCommand, msecs);

				// Stop here if we only want to write to the original file
				if (sidecarFile == Exiftool::SidecarFile::No) {
					return ok;
				}

				// Stop here on success if we only want to write sidecar files on error
				if (ok && sidecarFile == Exiftool::SidecarFile::OnError) {
					return ok;
				}
			}

			// If we're here, that means we either only want to write sidecar files, always want both, or an error occurred and we only write sidecars on error
			const QFileInfo info(file);
			const QString sidecarFilename = (sidecarFileNoExtension ? info.path() + QDir::separator() + info.completeBaseName() : file) + ".xmp";
			return execute(sidecarFilename, commands.join("\n"), msecs);
		}

	private:
		QProcess *m_process = nullptr;
		#ifdef Q_OS_WIN
			HANDLE m_job = nullptr;
		#endif
};


Exiftool::Exiftool(QObject *parent)
	: QObject(parent), m_worker(new ExiftoolWorker())
{
	m_worker->moveToThread(&m_thread);
	connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
	m_thread.start();
}

Exiftool::~Exiftool()
{
	stop();
	m_thread.quit();
	m_thread.wait();
}


QString Exiftool::version(int msecs)
{
	QProcess process;
	process.start("exiftool", { "-ver" });

	if (!process.waitForStarted(msecs)) {
		return "";
	}
	if (!process.waitForFinished(msecs)) {
		process.kill();
		return "";
	}
	if (process.exitCode() != 0) {
		return "";
	}

	return QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
}

/**
 * Whether a path lives on a network volume, where per-operation latency dominates.
 */
bool Exiftool::isOnRemoteVolume(const QString &path)
{
	if (path.startsWith(QLatin1String("//")) || path.startsWith(QLatin1String("\\\\"))) {
		return true;
	}

	const QStorageInfo storage(QFileInfo(path).absolutePath());
	if (!storage.isValid()) {
		return false;
	}

	// Windows names local volumes `\\?\Volume{GUID}`, so only a UNC device is actually remote
	const QString device = QString::fromUtf8(storage.device());
	if (device.startsWith(QLatin1String("\\\\?\\")) || device.startsWith(QLatin1String("//?/"))) {
		return false;
	}
	return device.startsWith(QLatin1String("//")) || device.startsWith(QLatin1String("\\\\"));
}

/**
 * The caller keeps processing events while the worker blocks, so a slow metadata write no longer
 * freezes the interface. Queued calls run one at a time, keeping the single exiftool process
 * single-threaded.
 */
bool Exiftool::runOnWorker(const std::function<bool()> &work)
{
	// Without a running thread there is nothing to hand the work to
	if (!m_thread.isRunning()) {
		return work();
	}

	bool result = false;
	QEventLoop loop;
	QMetaObject::invokeMethod(m_worker, [&]() {
		result = work();
		QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
	}, Qt::QueuedConnection);
	loop.exec();

	return result;
}


bool Exiftool::start(int msecs)
{
	return runOnWorker([this, msecs]() { return m_worker->start(msecs); });
}

bool Exiftool::setMetadata(const QString &file, const QMap<QString, QString> &metadata, bool clear, bool keepColorProfile, SidecarFile sidecarFile, bool sidecarFileNoExtension, int msecs)
{
	return runOnWorker([=]() { return m_worker->setMetadata(file, metadata, clear, keepColorProfile, sidecarFile, sidecarFileNoExtension, msecs); });
}

bool Exiftool::execute(const QString &file, const QString &command, int msecs)
{
	return runOnWorker([=]() { return m_worker->execute(file, command, msecs); });
}

bool Exiftool::stop(int msecs)
{
	return runOnWorker([this, msecs]() { return m_worker->stop(msecs); });
}
