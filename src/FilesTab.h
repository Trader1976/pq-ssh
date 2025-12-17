#pragma once

#include <QWidget>
#include <QString>
#include <QVector>
#include <functional>

#include "SshClient.h"

class QLabel;
class QPushButton;
class QTreeView;
class QFileSystemModel;
class QProgressDialog;

class RemoteDropTable; // <-- real class (in RemoteDropTable.h)
class QProgressDialog;

class FilesTab : public QWidget
{
    Q_OBJECT
public:
    explicit FilesTab(SshClient *ssh, QWidget *parent = nullptr);

public slots:
    void onSshConnected();      // call after successful connect
    void onSshDisconnected();   // call on disconnect

private slots:
    void refreshRemote();
    void goRemoteUp();
    void remoteItemActivated(int row, int col);

    void uploadSelected();
    void downloadSelected();

    void onRemoteFilesDropped(const QStringList& localPaths); // <-- drag→upload entry

    void onTransferProgress(quint64 done, quint64 total);
    void uploadFolder();
    void goLocalUp();

private:
    void buildUi();
    void setRemoteCwd(const QString& path);
    QString remoteParentDir(const QString& p) const;

    void runTransfer(const QString& title,
                     const std::function<bool(QString *err)> &fn);
    void startUploadPaths(const QStringList& paths);

private:
    SshClient *m_ssh = nullptr;
    QString m_remoteCwd;

    // UI
    QLabel *m_remotePathLabel = nullptr;
    QPushButton *m_upBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_uploadBtn = nullptr;
    QPushButton *m_downloadBtn = nullptr;

    QTreeView *m_localView = nullptr;
    QFileSystemModel *m_localModel = nullptr;

    RemoteDropTable *m_remoteTable = nullptr; // <-- was QTableWidget

    // Progress (one at a time for MVP)
    QProgressDialog *m_progressDlg = nullptr;
    QPushButton *m_uploadFolderBtn = nullptr;
    QPushButton *m_localUpBtn = nullptr;
    QString m_localCwd;

};
