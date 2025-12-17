#pragma once

#include <QTableWidget>
#include <QStringList>

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QMimeData;

class RemoteDropTable : public QTableWidget
{
    Q_OBJECT
public:
    explicit RemoteDropTable(QWidget *parent = nullptr);

    signals:
        void filesDropped(const QStringList& localPaths);

protected:
    // Accept local file drops (upload)
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

    // Provide remote-path mime when user drags OUT (download)
    QMimeData* mimeData(const QList<QTableWidgetItem*> items) const override;
};
