// IdentityManagerDialog.h
#pragma once
#include <QDialog>
#include <QByteArray>
#include <QString>
#include <QJsonObject>
#include <QResizeEvent>

class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QPushButton;
class QListWidget;
class QListWidgetItem;
//class QSplitter;

class IdentityManagerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit IdentityManagerDialog(QWidget *parent = nullptr);


protected:
    void resizeEvent(QResizeEvent *e) override;


private slots:
    void onDerive();
    void onCopyPublic();
    void onCopyFingerprint();
    void onSavePrivate();
    void onSavePublic();

    // NEW
    void onCreateIdentity();
    void onRestoreIdentity();
    void onSaveIdentity();
    void onRemoveIdentity();
    void onSelectSaved(QListWidgetItem *item);


private:
    // NEW helpers
    void buildUi();
    void loadSaved();
    void clearDerivedUi();
    QString normalizeWords(const QString &in) const;
    QStringList parseWords24(const QString &in, QString *err) const;
    QStringList loadWordlist(QString *err) const;
    QStringList generateRandom24(QString *err) const;

    QString identitiesPath() const;
    bool readIdentitiesJson(QJsonObject *root, QString *err) const;
    bool writeIdentitiesJson(const QJsonObject &root, QString *err) const;
    void updateFingerprintUi();


private:
    // existing widgets
    QPlainTextEdit *m_words = nullptr;
    QLineEdit      *m_pass = nullptr;
    QLineEdit      *m_comment = nullptr;
    QLabel         *m_fp = nullptr;
    QPlainTextEdit *m_pubOut = nullptr;

    // existing derived material
    QByteArray m_pub32;
    QByteArray m_priv64;
    QByteArray m_privFile;

    // NEW UI
    QListWidget *m_savedList = nullptr;
    QLineEdit   *m_alias = nullptr;

    QPushButton *m_createBtn = nullptr;
    QPushButton *m_restoreBtn = nullptr;
    QPushButton *m_saveIdBtn = nullptr;
    QPushButton *m_removeIdBtn = nullptr;
    QPushButton *m_deriveBtn = nullptr;

    QString m_selectedFp; // fingerprint of selected saved identity
    QString m_fullFingerprint;
    //QSplitter *m_split = nullptr;
    //QWidget   *m_leftPane = nullptr;

};
