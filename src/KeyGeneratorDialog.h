#pragma once

#include <QDialog>
#include <QMap>
#include <QJsonObject>
#include <QStringList>

/*
 * Forward declarations for Qt widgets used by the dialog.
 * Keeps compile times lower and avoids heavy includes in the header.
 */
class QTabWidget;
class QTableWidget;
class QLabel;
class QPushButton;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QDateTimeEdit;
class QSpinBox;

/*
 * KeyGeneratorDialog
 * ------------------
 * UI dialog responsible for:
 *  - Generating SSH keys (OpenSSH + Dilithium5)
 *  - Encrypting Dilithium private keys at rest
 *  - Managing key metadata (label, owner, purpose, expiry, rotation)
 *  - Displaying a key inventory table
 *  - Installing selected public keys to remote servers via profiles
 *
 * This dialog is UI-centric:
 *  - Cryptography lives in DilithiumKeyCrypto.*
 *  - SSH operations are delegated to MainWindow / SshClient
 */
class KeyGeneratorDialog : public QDialog
{
    Q_OBJECT

public:
    /*
     * @param profileNames  List of available SSH profile names.
     *                      Used when installing a public key to a remote host.
     */
    explicit KeyGeneratorDialog(const QStringList& profileNames,
                                QWidget *parent = nullptr);

signals:
    /*
     * Emitted when the user confirms installing a selected public key.
     *
     * @param pubKeyLine     Full OpenSSH-compatible public key line
     * @param profileIndex  Index into profileNames (resolved by MainWindow)
     *
     * MainWindow owns the actual SSH connection and remote key installation.
     */
    void installPublicKeyRequested(const QString& pubKeyLine, int profileIndex);

private slots:
    // Key generation / inventory refresh
    void onGenerate();
    void refreshKeysTable();
    void onKeySelectionChanged();

    // Clipboard / export actions
    void onCopyFingerprint();
    void onCopyPublicKey();
    void onExportPublicKey();

    // Metadata & lifecycle actions
    void onEditMetadata();
    void onMarkRevoked();
    void onDeleteKey();

    // Context menu + install flow
    void onKeysContextMenuRequested(const QPoint& pos);
    void onInstallSelectedKey();

private:
    /*
     * KeyRow
     * ------
     * Aggregated view-model for one key entry in the inventory table.
     * Combines:
     *  - metadata.json content
     *  - filesystem-discovered key files
     */
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

    // ---------------------------------------------------------------------
    // Paths & helpers
    // ---------------------------------------------------------------------

    // ~/.pq-ssh/keys
    QString keysDir() const;

    // ~/.pq-ssh/keys/metadata.json
    QString metadataPath() const;

    bool ensureKeysDir(QString *errOut);

    /*
     * Runs ssh-keygen (OpenSSH) or Dilithium5 stub generator.
     * Writes raw key material to disk; encryption handled elsewhere.
     */
    bool runSshKeygen(const QString &algo,
                      const QString &privPath,
                      const QString &comment,
                      const QString &passphrase,
                      QString *errOut);

    // Computes SHA256 fingerprint for OpenSSH or PQSSH public keys
    bool computeFingerprint(const QString &pubPath,
                            QString *fpOut,
                            QString *errOut);

    // ---------------------------------------------------------------------
    // Metadata persistence
    // ---------------------------------------------------------------------

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

    // Reads first line of a .pub file
    QString readPublicKeyLine(const QString &pubPath);

    /*
     * Builds the inventory by merging:
     *  - *.pub files on disk
     *  - entries from metadata.json
     */
    QMap<QString, KeyRow> buildInventory(QString *errOut);

    // Helpers for table selection
    int selectedTableRow() const;
    KeyRow selectedRow() const;

    // Updates editable metadata fields for an existing key
    bool updateMetadataFields(const QString &fingerprint,
                              const QString &label,
                              const QString &owner,
                              const QString &purpose,
                              int rotationDays,
                              const QString &expiresIsoOrEmpty,
                              QString *errOut);

private:
    // Names of SSH profiles (owned by MainWindow)
    QStringList m_profileNames;

    // ---------------------------------------------------------------------
    // UI widgets
    // ---------------------------------------------------------------------

    QTabWidget *m_tabs{};
    QLabel *m_resultLabel{};
    QLabel *m_keysHintLabel{};

    // Generate tab
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

    // Keys tab
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

    // Sorting + inventory state
    int m_sortColumn = 0;
    Qt::SortOrder m_sortOrder = Qt::AscendingOrder;
    QMap<QString, KeyRow> m_inventory;
};
