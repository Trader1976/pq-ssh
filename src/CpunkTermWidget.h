#pragma once

#include <qtermwidget5/qtermwidget.h>
#include <QPointer>
#include <QByteArray>

/*
    CpunkTermWidget
    ----------------
    Thin wrapper around QTermWidget used by pq-ssh.

    Purpose:
    - Prevent accidental auto-start of a local shell
    - Provide a controlled terminal widget for SSH sessions only
    - Add drag & drop file support (upload direction)
    - Keep terminal behavior isolated from global application styles

    This class does NOT:
    - Perform SSH logic
    - Decide how dropped files are transferred
    - Manage remote connections

    Instead, it emits signals (fileDropped) and lets higher layers
    (MainWindow / SshClient) decide what to do.
*/

class CpunkTermWidget : public QTermWidget
{
    Q_OBJECT

public:
    /*
        Constructor

        startnow:
        - Passed to QTermWidget constructor.
        - pq-ssh always passes 0 to prevent auto-starting a local shell.
          (Security + UX: pq-ssh must never open a local shell implicitly.)

        parent:
        - Standard Qt parent widget.
    */
    explicit CpunkTermWidget(int historyLines = 2000, QWidget *parent = nullptr);

signals:
    /*
        Emitted when a local file is dropped onto the terminal.

        path:
        - Absolute local filesystem path of the dropped file.

        data:
        - Full contents of the file (read with retries to handle
          delayed/temporary files on some platforms).

        NOTE:
        - This signal is transport-agnostic.
        - The receiver decides whether to SCP, SFTP, upload via SSH,
          reject the file, etc.
    */
    void fileDropped(const QString &path, const QByteArray &data);

protected:
    /*
        Event filter used to intercept DragEnter and Drop events.

        Why eventFilter?
        - QTermWidget is composed of multiple internal child widgets.
        - Drag & drop events may be delivered to any of them.
        - Installing an event filter allows us to reliably catch
          drag/drop events across the entire widget subtree.
    */
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    /*
        Enables drag & drop handling on this widget and all child widgets.

        Called once from the constructor.
        Sets acceptDrops=true and installs the event filter recursively.
    */
    void setupDropInterceptor();
};
