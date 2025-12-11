#pragma once

#include <QPlainTextEdit>
#include <QByteArray>
#include <QKeyEvent>

class TerminalView : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit TerminalView(QWidget *parent = nullptr);

signals:
    void bytesTyped(const QByteArray &data);

protected:
    void keyPressEvent(QKeyEvent *e) override;
};
