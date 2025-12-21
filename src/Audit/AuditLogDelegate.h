// src/Audit/AuditLogDelegate.h
#pragma once
#include <QStyledItemDelegate>

class AuditLogDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit AuditLogDelegate(QObject* parent=nullptr);

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
};
