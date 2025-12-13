#pragma once

#include <qtermwidget5/qtermwidget.h>
#include <QPointer>
#include <QByteArray>

class CpunkTermWidget : public QTermWidget
{
    Q_OBJECT
public:
    explicit CpunkTermWidget(int startnow = 0, QWidget *parent = nullptr);

    signals:
        void fileDropped(const QString &path, const QByteArray &data);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void setupDropInterceptor();
};
