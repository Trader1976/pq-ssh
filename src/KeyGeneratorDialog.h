#pragma once

#include <QDialog>
#include <QMap>
#include <QJsonObject>
#include <Qt>

class QLineEdit;
class QComboBox;
class QCheckBox;
class QDateTimeEdit;
class QSpinBox;
class QLabel;
class QPushButton;
class QTabWidget;
class QTableWidget;

class KeyGeneratorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit KeyGeneratorDialog(QWidget *parent = nullptr);

private slots:
    void onGenerate();

    // Keys tab actions
    void refreshKeysTable();
    void onKeySelectionChanged();
    void onCopyFingerprint();
    void onCopyPublicKey();
    void onExportPublicKey();
    void onMarkRevoked();
    void onDeleteKey();
    void onEditMetadata();

private:
    struct KeyRow {
        QString fingerprint;
        QString label;
        QString owner;
        QString created;
        QString expires;   // empty if none
        QString purpose;
        QString algorithm;
        int rotationDays = 0;
        QString status;

        QString pubPath;
        QString privPath;
        QString comment;   // from .pub line if available

        bool hasMetadata = false;
        bool hasFiles = false;
    };

    QString keysDir() const;
    QString metadataPath() const;

    bool ensureKeysDir(QString *errOut);
    bool runSshKeygen(const QString &algo, const QString &privPath,
                      const QString &comment, const QString &passphrase,
                      QString *errOut);
    bool computeFingerprint(const QString &pubPath, QString *fpOut, QString *errOut);

    // Metadata
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

    // Scanning keys on disk
    QMap<QString, KeyRow> buildInventory(QString *errOut);
    static QString readPublicKeyLine(const QString &pubPath);

    KeyRow selectedRow() const;
    int selectedTableRow() const;

    bool updateMetadataFields(const QString &fingerprint,
                          const QString &label,
                          const QString &owner,
                          const QString &purpose,
                          int rotationDays,
                          const QString &expiresIsoOrEmpty,
                          QString *errOut);

private:
    // UI - tabs
    QTabWidget *m_tabs{};

    // Generate tab UI
    QComboBox *m_algoCombo{};
    QLineEdit *m_keyNameEdit{};
    QLineEdit *m_labelEdit{};
    QLineEdit *m_ownerEdit{};
    QLineEdit *m_purposeEdit{};
    QSpinBox  *m_rotationSpin{};
    QComboBox *m_statusCombo{};
    QCheckBox *m_expireCheck{};
    QDateTimeEdit *m_expireDate{};
    QLineEdit *m_pass1Edit{};
    QLineEdit *m_pass2Edit{};
    QLabel *m_resultLabel{};
    QPushButton *m_generateBtn{};

    // Keys tab UI
    QTableWidget *m_table{};
    QPushButton *m_refreshBtn{};
    QPushButton *m_copyFpBtn{};
    QPushButton *m_copyPubBtn{};
    QPushButton *m_exportPubBtn{};
    QPushButton *m_revokeBtn{};
    QPushButton *m_deleteBtn{};
    QCheckBox *m_deleteFilesCheck{};
    QLabel *m_keysHintLabel{};
    QPushButton *m_editMetaBtn{};
    // data cache
    QMap<QString, KeyRow> m_inventory; // fingerprint -> info
    // --- persist table sorting ---
    int m_sortColumn = 0;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
};
