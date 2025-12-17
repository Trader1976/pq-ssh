#pragma once
#include <QTableWidget>
#include <QStringList>

class RemoteDropTable : public QTableWidget
{
    Q_OBJECT
public:
    explicit RemoteDropTable(QWidget *parent = nullptr)
        : QTableWidget(parent)
    {
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DropOnly);
        viewport()->setAcceptDrops(true);
        setDropIndicatorShown(true);
    }

    signals:
        void filesDropped(const QStringList& localPaths);

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
};
