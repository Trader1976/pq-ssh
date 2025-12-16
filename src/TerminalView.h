#pragma once

#include <QPlainTextEdit>
#include <QPoint>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QByteArray>
#include <QString>
#include <QColor>

/*
 * TerminalView
 * ============
 * A lightweight, “terminal-like” text widget built on QPlainTextEdit.
 *
 * What it does:
 *  - Emits typed bytes via bytesTyped() so an SSH shell worker can send input.
 *  - Provides local echo (it still behaves like a normal text editor).
 *  - Supports drag & drop:
 *      * Drop local file into widget -> emits fileDropped(path, bytes)
 *      * Drag a selected word out -> attempts scp download of that remote filename
 *
 * What it is NOT:
 *  - A full terminal emulator. For proper terminal behavior (escape codes, arrows,
 *    function keys, etc.) pq-ssh uses qtermwidget via CpunkTermWidget.
 *
 * Design intent:
 *  - Keep TerminalView small and “glue-like”: UI event handling + signals.
 *  - Avoid direct libssh calls here; any network work should live in SSH modules.
 */

class TerminalView : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit TerminalView(QWidget *parent = nullptr);
    ~TerminalView() override;

    /*
     * setRemoteContext()
     * ------------------
     * Sets the remote identity needed for the “drag remote file to desktop” feature.
     * If m_remoteHost is empty, TerminalView will not attempt scp downloads on drag.
     */
    void setRemoteContext(const QString &user, const QString &host);

    /*
     * applyTerminalBackground()
     * ------------------------
     * Forces a consistent background color for both the widget frame and its viewport.
     * Used to prevent style/palette inheritance from causing gray flicker or mismatched
     * backgrounds (common with QPlainTextEdit under custom app themes).
     */
    void applyTerminalBackground(const QColor& bg);

signals:
    /*
     * bytesTyped()
     * ------------
     * Emitted for textual input (event->text()) so the SSH shell layer can forward it.
     * Note: special keys do not always produce text; qtermwidget handles those better.
     */
    void bytesTyped(const QByteArray &data);

    /*
     * fileDropped()
     * -------------
     * Emitted when a local file is dropped into the widget. Includes both the path
     * and the full file content (read into memory).
     *
     * Note: For very large files, you may later switch this to “path only”
     * and stream/upload from disk instead of holding bytes in RAM.
     */
    void fileDropped(const QString &path, const QByteArray &data);

protected:
    // Capture typed characters, emit bytesTyped(), then allow local echo.
    void keyPressEvent(QKeyEvent *event) override;

    // Track drag start for “drag remote filename out”.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

    // Accept OS file drops into the widget.
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    // Remote context for SCP-based “drag out” file retrieval.
    QString m_remoteUser;
    QString m_remoteHost;

    // Mouse position at start of drag gesture.
    QPoint  m_dragStartPos;

    // Helper: return word under cursor at position (used when no selection).
    QString wordAtPosition(const QPoint &pos) const;

    // Helper: scp remote fileName into temp dir and return local path (or empty on failure).
    QString downloadRemoteFileForDrag(const QString &fileName) const;
};
