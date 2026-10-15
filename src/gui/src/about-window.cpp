#include "about-window.h"
#include "ui_about-window.h"


AboutWindow::AboutWindow(const QString &version, QWidget *parent)
	: QDialog(parent), ui(new Ui::AboutWindow)
{
	setAttribute(Qt::WA_DeleteOnClose);
	ui->setupUi(this);

	// Set window title according to the current build
	QString labelVersion = version;
	#ifdef NIGHTLY
		QString commit(NIGHTLY_COMMIT);
		if (!commit.isEmpty()) {
			labelVersion += QString(" - nightly (%1)").arg(commit.left(8));
		} else {
			labelVersion += " - nightly";
		}
	#endif
	labelVersion += QString(" (%1)").arg(VERSION_PLATFORM);
	ui->labelCurrent->setText(labelVersion);

	ui->labelCreator->setText(ui->labelCreator->text().replace("{website}", PROJECT_WEBSITE_URL));

	// List contributors
	const QString contributors = QString(CONTRIBUTORS).replace('\t', ", ");
	ui->labelContributors->setText(ui->labelContributors->text().replace("{contributors}", contributors));

	setFixedSize(600, heightForWidth(600));
}

AboutWindow::~AboutWindow()
{
	delete ui;
}
