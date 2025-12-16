#pragma once

#include <QDialog>
#include <QMap>
#include <QJsonObject>
#include <QStringList>

class QTabWidget;
class QTableWidget;
class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QDateTimeEdit;
class QSpinBox;

class KeyGeneratorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit KeyGeneratorDialog(const QStringList& profileNames, QWidget *parent = nullptr);

signals:
    void installPublicKeyRequested(const QString& pubKeyLine, int profileIndex);

private slots:
    void onGenerate();
    void refreshKeysTable();
    void onKeySelectionChanged();

    void onCopyFingerprint();
    void onCopyPublicKey();
    void onExportPublicKey();

    void onEditMetadata();
    void onMarkRevoked();
    void onDeleteKey();

    void onKeysContextMenuRequested(const QPoint& pos);
    void onInstallSelectedKey();

private:
    struct KeyRow {
        QString fingerprint;

        QString label;
        QString owner;
        QString algorithm;
        QString status;

        QString created;
        QString expires;

        QString purpose;
        int rotationDays = 0;

        QString pubPath;
        QString privPath;

        QString comment;

        bool hasMetadata = false;
        bool hasFiles = false;
    };

    // Paths / helpers
    QString keysDir() const;
    QString metadataPath() const;

    bool ensureKeysDir(QString *errOut);
    bool runSshKeygen(const QString &algo, const QString &privPath,
                      const QString &comment, const QString &passphrase,
                      QString *errOut);

    bool computeFingerprint(const QString &pubPath, QString *fpOut, QString *errOut);

    bool loadMetadata(QMap<QString, QJsonObject> *out, QString *errOut);
    bool writeMetadata(const QJsonObject &root, QString *errOut);

    bool saveMetadata(const QString &fingerprint,
                      const QString &label,
                      const QString &owner,
                      const QString &createdIso,
                      const QString &expiresIsoOrEmpty,
                      const QString &purpose,
                      const QString &algorithm,
                      int rotationDays,
                      const QString &status,
                      const QString &privPath,
                      const QString &pubPath,
                      QString *errOut);

    QString readPublicKeyLine(const QString &pubPath);

    QMap<QString, KeyRow> buildInventory(QString *errOut);

    int selectedTableRow() const;
    KeyRow selectedRow() const;

    bool updateMetadataFields(const QString &fingerprint,
                              const QString &label,
                              const QString &owner,
                              const QString &purpose,
                              int rotationDays,
                              const QString &expiresIsoOrEmpty,
                              QString *errOut);

private:
    QStringList m_profileNames;

    // UI
    QTabWidget *m_tabs{};
    QLabel *m_resultLabel{};
    QLabel *m_keysHintLabel{};

    // Generate tab controls
    QComboBox *m_algoCombo{};
    QLineEdit *m_keyNameEdit{};
    QLineEdit *m_labelEdit{};
    QLineEdit *m_ownerEdit{};
    QLineEdit *m_purposeEdit{};
    QSpinBox *m_rotationSpin{};
    QComboBox *m_statusCombo{};
    QCheckBox *m_expireCheck{};
    QDateTimeEdit *m_expireDate{};
    QLineEdit *m_pass1Edit{};
    QLineEdit *m_pass2Edit{};
    QPushButton *m_generateBtn{};

    // Keys tab controls
    QTableWidget *m_table{};
    QPushButton *m_refreshBtn{};
    QPushButton *m_copyFpBtn{};
    QPushButton *m_copyPubBtn{};
    QPushButton *m_exportPubBtn{};
    QPushButton *m_installBtn{};
    QPushButton *m_editMetaBtn{};
    QPushButton *m_revokeBtn{};
    QPushButton *m_deleteBtn{};
    QCheckBox *m_deleteFilesCheck{};

    // Sorting + inventory
    int m_sortColumn = 0;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    QMap<QString, KeyRow> m_inventory;
};
