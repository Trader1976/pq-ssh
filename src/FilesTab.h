#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "SshClient.h"

class QLabel;
class QPushButton;
class QTreeView;
class QFileSystemModel;
class QProgressDialog;
class RemoteDropTable;

class FilesTab : public QWidget
{
    Q_OBJECT
public:
    explicit FilesTab(SshClient *ssh, QWidget *parent = nullptr);

public slots:
    void onSshConnected();
    void onSshDisconnected();

private slots:
    void refreshRemote();
    void goRemoteUp();
    void goLocalUp();
    void remoteItemActivated(int row, int col);

    void uploadSelected();
    void uploadFolder();
    void downloadSelected();

    void onRemoteFilesDropped(const QStringList& localPaths);

    void onTransferProgress(quint64 done, quint64 total);

private:
    void buildUi();
    void setRemoteCwd(const QString& path);
    QString remoteParentDir(const QString& p) const;

    void runTransfer(const QString& title,
                     const std::function<bool(QString *err)> &fn);

    void startUploadPaths(const QStringList& paths);

    // Recursive download support (folders + files)
    void startDownloadPaths(const QStringList& remotePaths, const QString& destDir);
    bool collectRemoteRecursive(const QString& remoteRoot,
                                const QString& localRoot,
                                QVector<QString>& outRemoteFiles,
                                QVector<QString>& outLocalFiles,
                                QVector<quint64>& outSizes,
                                quint64* totalBytes,
                                QString* err);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    SshClient *m_ssh = nullptr;

    QString m_remoteCwd;
    QString m_localCwd;

    // UI
    QLabel *m_remotePathLabel = nullptr;
    QPushButton *m_remoteUpBtn = nullptr;
    QPushButton *m_localUpBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_uploadBtn = nullptr;
    QPushButton *m_uploadFolderBtn = nullptr;
    QPushButton *m_downloadBtn = nullptr;

    QTreeView *m_localView = nullptr;
    QFileSystemModel *m_localModel = nullptr;

    RemoteDropTable *m_remoteTable = nullptr;

    QProgressDialog *m_progressDlg = nullptr;
};
