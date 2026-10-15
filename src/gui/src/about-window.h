#ifndef ABOUT_WINDOW_H
#define ABOUT_WINDOW_H

#include <QDialog>
#include <QString>
#include <QWidget>


namespace Ui
{
	class AboutWindow;
}


class AboutWindow : public QDialog
{
	Q_OBJECT

	public:
		explicit AboutWindow(const QString &version, QWidget *parent = nullptr);
		~AboutWindow() override;

	private:
		Ui::AboutWindow *ui;
};

#endif // ABOUT_WINDOW_H
