#include "tabs/log-tab.h"
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>
#include <QScrollBar>
#include <ui_log-tab.h>
#include "functions.h"
#include "helpers.h"
#include "logger.h"
#include "utils/logging.h"


LogTab::LogTab(QSettings *settings, QWidget *parent)
	: QWidget(parent), ui(new Ui::LogTab), m_settings(settings)
{
	ui->setupUi(this);

	ui->checkAutoScroll->setChecked(m_settings->value("Log/autoScroll", true).toBool());
	connect(ui->checkAutoScroll, &QCheckBox::toggled, this, &LogTab::autoScrollToggled);

	// Load already written log
	QFile logFile(Logger::getInstance().logFile());
	if (logFile.open(QFile::ReadOnly | QFile::Text)) {
		while (!logFile.atEnd()) {
			write(logFile.readLine());
		}
		logFile.close();
	}

	connect(&Logger::getInstance(), &Logger::newLog, this, &LogTab::write);
}

LogTab::~LogTab()
{
	close();
	delete ui;
}

void LogTab::write(const QString &msg)
{
	auto *scrollBar = ui->labelLog->verticalScrollBar();
	const int previous = scrollBar->value();

	ui->labelLog->appendHtml(logToHtml(msg));

	// Appending moves the cursor to the end, which drags the view with it. Only follow the
	// newest line when the user asked for it - otherwise restore where they were reading, so
	// a busy log does not yank the view away mid-sentence.
	if (ui->checkAutoScroll->isChecked()) {
		scrollBar->setValue(scrollBar->maximum());
	} else {
		scrollBar->setValue(previous);
	}
}

void LogTab::autoScrollToggled(bool checked)
{
	m_settings->setValue("Log/autoScroll", checked);

	// Re-enabling should jump straight to the newest line rather than waiting for one to arrive.
	if (checked) {
		auto *scrollBar = ui->labelLog->verticalScrollBar();
		scrollBar->setValue(scrollBar->maximum());
	}
}

void LogTab::clear()
{
	QFile logFile(Logger::getInstance().logFile());
	if (logFile.open(QFile::WriteOnly | QFile::Text)) {
		logFile.resize(0);
		logFile.close();
	}

	ui->labelLog->clear();
}

void LogTab::open()
{
	QDesktopServices::openUrl("file:///" + Logger::getInstance().logFile());
}

void LogTab::openDir()
{
	showInGraphicalShell(Logger::getInstance().logDirectory());
}

void LogTab::changeEvent(QEvent *event)
{
	// Automatically re-translate this tab on language change
	if (event->type() == QEvent::LanguageChange) {
		ui->retranslateUi(this);
	}

	QWidget::changeEvent(event);
}
