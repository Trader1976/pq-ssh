#pragma once

#include <QPlainTextEdit>
#include <QPoint>
#include <QTextEdit>
#include <QDragEnterEvent>
#include <QDropEvent>

class TerminalView : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit TerminalView(QWidget *parent = nullptr);
    ~TerminalView() override;

    void setRemoteContext(const QString &user, const QString &host);

    signals:
        void bytesTyped(const QByteArray &data);
        void fileDropped(const QString &path, const QByteArray &data);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    // 🔹 NEW: for drag & drop support
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QString m_remoteUser;
    QString m_remoteHost;
    QPoint  m_dragStartPos;

    QString wordAtPosition(const QPoint &pos) const;
    QString downloadRemoteFileForDrag(const QString &fileName) const;
};
