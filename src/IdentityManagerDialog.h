#pragma once
#include <QDialog>

class QPlainTextEdit;
class QLineEdit;
class QLabel;
class QPushButton;

class IdentityManagerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit IdentityManagerDialog(QWidget *parent = nullptr);

private slots:
    void onDerive();
    void onCopyPublic();
    void onCopyFingerprint();
    void onSavePrivate();
    void onSavePublic();

private:
    QString sha3Fingerprint(const QByteArray &pub32);

    QPlainTextEdit *m_words;
    QLineEdit *m_pass;
    QLineEdit *m_comment;
    QLabel *m_fp;
    QPlainTextEdit *m_pubOut;

    QByteArray m_pub32;
    QByteArray m_priv64;
    QByteArray m_privFile;
};
