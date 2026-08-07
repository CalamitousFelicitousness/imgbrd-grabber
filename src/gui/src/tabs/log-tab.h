#ifndef LOG_TAB_H
#define LOG_TAB_H

#include <QWidget>


class QSettings;


namespace Ui
{
	class LogTab;
}


class LogTab : public QWidget
{
	Q_OBJECT

	public:
		explicit LogTab(QSettings *settings, QWidget *parent = nullptr);
		~LogTab() override;

	public slots:
		void write(const QString &msg);
		void clear();
		void open();
		void openDir();

	protected:
		void changeEvent(QEvent *event) override;

	private slots:
		void autoScrollToggled(bool checked);

	private:
		Ui::LogTab *ui;
		QSettings *m_settings;
};

#endif // LOG_TAB_H
