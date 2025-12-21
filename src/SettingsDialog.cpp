#include "SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>
#include <QStyle>
#include <QSettings>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QStandardPaths>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setModal(true);
    buildUi();
    loadFromSettings();
}

static QToolButton* makeIconBtn(QWidget* parent, const QIcon& icon, const QString& tooltip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setToolTip(tooltip);
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

void SettingsDialog::buildUi()
{
    setWindowTitle("Settings");
    resize(720, 260);

    auto* outer = new QVBoxLayout(this);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // Theme
    m_themeCombo = new QComboBox(this);
    m_themeCombo->addItem("CPUNK Dark", "cpunk-dark");
    m_themeCombo->addItem("CPUNK Orange", "cpunk-orange");
    m_themeCombo->addItem("CPUNK Neo", "cpunk-neo");
    m_themeCombo->addItem("Windows Basic", "windows-basic");
    form->addRow("Theme:", m_themeCombo);

    // Log level
    m_logLevelCombo = new QComboBox(this);
    m_logLevelCombo->addItem("Errors only", 0);
    m_logLevelCombo->addItem("Normal", 1);
    m_logLevelCombo->addItem("Debug", 2);
    form->addRow("Log level:", m_logLevelCombo);

    // Icons (“disk”)
    const QIcon browseIcon = style()->standardIcon(QStyle::SP_DialogOpenButton);
    const QIcon openDirIcon = style()->standardIcon(QStyle::SP_DirOpenIcon);

    // Log file path row
    {
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(6);

        m_logFileEdit = new QLineEdit(row);
        const QString defaultLogDir =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs";
        m_logFileEdit->setPlaceholderText(QString("Empty = default (%1/<app>.log)").arg(defaultLogDir));


        m_logBrowseBtn = makeIconBtn(row, browseIcon, "Choose log file…");
        m_logOpenDirBtn = makeIconBtn(row, openDirIcon, "Open log file directory");

        h->addWidget(m_logFileEdit, 1);
        h->addWidget(m_logBrowseBtn);
        h->addWidget(m_logOpenDirBtn);

        form->addRow("Log file:", row);

        connect(m_logBrowseBtn, &QToolButton::clicked, this, &SettingsDialog::onBrowseLogFile);
        connect(m_logOpenDirBtn, &QToolButton::clicked, this, &SettingsDialog::onOpenLogDir);
    }

    // Audit dir path row
    {
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(6);

        m_auditDirEdit = new QLineEdit(row);
        m_auditDirEdit->setPlaceholderText("Empty = default (AppLocalDataLocation/audit)");

        m_auditBrowseBtn = makeIconBtn(row, browseIcon, "Choose audit directory…");
        m_auditOpenDirBtn = makeIconBtn(row, openDirIcon, "Open audit directory");

        h->addWidget(m_auditDirEdit, 1);
        h->addWidget(m_auditBrowseBtn);
        h->addWidget(m_auditOpenDirBtn);

        form->addRow("Audit dir:", row);

        connect(m_auditBrowseBtn, &QToolButton::clicked, this, &SettingsDialog::onBrowseAuditDir);
        connect(m_auditOpenDirBtn, &QToolButton::clicked, this, &SettingsDialog::onOpenAuditDir);
    }

    outer->addLayout(form);

    // Buttons
    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    outer->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void SettingsDialog::loadFromSettings()
{
    QSettings s;

    // Theme
    const QString themeId = s.value("ui/theme", "cpunk-dark").toString();
    for (int i = 0; i < m_themeCombo->count(); ++i) {
        if (m_themeCombo->itemData(i).toString() == themeId) {
            m_themeCombo->setCurrentIndex(i);
            break;
        }
    }

    // Log level
    const int lvl = s.value("logging/level", 1).toInt();
    for (int i = 0; i < m_logLevelCombo->count(); ++i) {
        if (m_logLevelCombo->itemData(i).toInt() == lvl) {
            m_logLevelCombo->setCurrentIndex(i);
            break;
        }
    }

    // Paths
    if (m_logFileEdit)
        m_logFileEdit->setText(s.value("logging/filePath", "").toString());

    if (m_auditDirEdit)
        m_auditDirEdit->setText(s.value("audit/dirPath", "").toString());
}

void SettingsDialog::saveToSettings()
{
    QSettings s;

    s.setValue("ui/theme", m_themeCombo ? m_themeCombo->currentData().toString() : "cpunk-dark");
    s.setValue("logging/level", m_logLevelCombo ? m_logLevelCombo->currentData().toInt() : 1);

    // IMPORTANT: allow empty = revert to default
    s.setValue("logging/filePath", m_logFileEdit ? m_logFileEdit->text().trimmed() : QString());
    s.setValue("audit/dirPath",    m_auditDirEdit ? m_auditDirEdit->text().trimmed() : QString());
}

void SettingsDialog::onAccepted()
{
    saveToSettings();
    accept();
}

QString SettingsDialog::dirOfPathOrEmpty(const QString& path) const
{
    const QString p = path.trimmed();
    if (p.isEmpty()) return QString();
    const QFileInfo fi(p);
    return fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
}

// ---- Buttons ----

void SettingsDialog::onBrowseLogFile()
{
    const QString cur = m_logFileEdit ? m_logFileEdit->text().trimmed() : QString();
    const QString startDir = dirOfPathOrEmpty(cur).isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        : dirOfPathOrEmpty(cur);

    const QString chosen = QFileDialog::getSaveFileName(
        this,
        "Choose log file",
        QDir(startDir).filePath("pq-ssh.log"),
        "Log files (*.log);;All files (*)"
    );

    if (!chosen.isEmpty() && m_logFileEdit)
        m_logFileEdit->setText(QDir::cleanPath(chosen));
}

void SettingsDialog::onOpenLogDir()
{
    if (!m_logFileEdit) return;

    const QString dir = dirOfPathOrEmpty(m_logFileEdit->text());
    if (!dir.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/logs"
        ));
}

void SettingsDialog::onBrowseAuditDir()
{
    const QString cur = m_auditDirEdit ? m_auditDirEdit->text().trimmed() : QString();
    const QString startDir = cur.isEmpty()
        ? (QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/audit")
        : cur;

    const QString chosen = QFileDialog::getExistingDirectory(
        this,
        "Choose audit directory",
        startDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (!chosen.isEmpty() && m_auditDirEdit)
        m_auditDirEdit->setText(QDir::cleanPath(chosen));
}

void SettingsDialog::onOpenAuditDir()
{
    if (!m_auditDirEdit) return;

    const QString dir = m_auditDirEdit->text().trimmed();
    if (!dir.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/audit"
        ));
}
