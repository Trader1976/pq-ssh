#include "TerminalView.h"

TerminalView::TerminalView(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setUndoRedoEnabled(false);
}

void TerminalView::keyPressEvent(QKeyEvent *e)
{
    QByteArray out;

    switch (e->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        out.append('\r');          // Enter -> CR (we translate to CRLF in worker)
        break;
    case Qt::Key_Backspace:
        out.append('\b');          // Backspace -> BS
        break;
    default:
        if (!e->text().isEmpty()) {
            out.append(e->text().toLocal8Bit());
        }
        break;
    }

    // ❌ NO local echo here
    // QPlainTextEdit::keyPressEvent(e);

    if (!out.isEmpty()) {
        emit bytesTyped(out);
    }
}

