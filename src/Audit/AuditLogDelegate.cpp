// src/Audit/AuditLogDelegate.cpp
#include "AuditLogDelegate.h"
#include <QPainter>

AuditLogDelegate::AuditLogDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void AuditLogDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const
{
    QStyleOptionViewItem o(opt);
    initStyleOption(&o, idx);

    const int sev = idx.data(Qt::UserRole).toInt();

    // Subtle row tint (works with your dark theme too)
    QColor tint;
    switch (sev) {
        case 1: tint = QColor(0, 255, 153, 28); break;   // OK
        case 2: tint = QColor(255, 193, 7,  28); break;   // WARN
        case 3: tint = QColor(255, 82,  82, 28); break;   // ERROR
        case 4: tint = QColor(180, 120, 255, 26); break;  // SECURITY
        default: tint = QColor(100, 160, 255, 16); break; // INFO
    }

    // Paint tinted background behind selected highlight (keep selection visible)
    p->save();
    if (!(o.state & QStyle::State_Selected)) {
        p->fillRect(o.rect, tint);
    }
    p->restore();

    QStyledItemDelegate::paint(p, o, idx);
}
