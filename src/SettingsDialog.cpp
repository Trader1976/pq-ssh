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
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <sodium.h>

// SettingsDialog.cpp
bool SettingsDialog::applySettings()
{
    QSettings s;

    const QString oldLang  = s.value("ui/language", "en").toString().trimmed();
    saveToSettings();
    s.sync();

    const QString newLang  = s.value("ui/language", "en").toString().trimmed();
    return (newLang != oldLang);
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
    buildUi();
    loadFromSettings();
}

static QToolButton* makeIconBtn(QWidget* parent, const QIcon& icon, const QString& tooltip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setToolTip(tooltip); // caller should pass already-translated text
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

void SettingsDialog::buildUi()
{
    setWindowTitle(tr("Settings"));
    resize(720, 320);

    auto* outer = new QVBoxLayout(this);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // Language
    m_languageCombo = new QComboBox(this);

    // Visible name, stored value = locale code you’ll use for qm lookup
    m_languageCombo->addItem(tr("English"), "en");
    m_languageCombo->addItem("Español", "es");
    m_languageCombo->addItem(tr("Suomi"),   "fi");
    // Later: add more
    // m_languageCombo->addItem(tr("Deutsch"), "de");
    // m_languageCombo->addItem(tr("Español"), "es");

    form->addRow(tr("Language:"), m_languageCombo);

    // Theme
    m_themeCombo = new QComboBox(this);
    m_themeCombo->addItem(tr("CPUNK Dark"), "cpunk-dark");
    m_themeCombo->addItem(tr("CPUNK Orange"), "cpunk-orange");
    m_themeCombo->addItem(tr("CPUNK Neo"), "cpunk-neo");
    m_themeCombo->addItem(tr("Windows Basic"), "windows-basic");
    form->addRow(tr("Theme:"), m_themeCombo);

    // Log level
    m_logLevelCombo = new QComboBox(this);
    m_logLevelCombo->addItem(tr("Errors only"), 0);
    m_logLevelCombo->addItem(tr("Normal"), 1);
    m_logLevelCombo->addItem(tr("Debug"), 2);
    form->addRow(tr("Log level:"), m_logLevelCombo);

    // Icons (“disk”)
    const QIcon browseIcon  = style()->standardIcon(QStyle::SP_DialogOpenButton);
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
        m_logFileEdit->setPlaceholderText(
            tr("Empty = default (%1/<app>.log)").arg(defaultLogDir)
        );

        m_logBrowseBtn  = makeIconBtn(row, browseIcon,  tr("Choose log file…"));
        m_logOpenDirBtn = makeIconBtn(row, openDirIcon, tr("Open log file directory"));

        h->addWidget(m_logFileEdit, 1);
        h->addWidget(m_logBrowseBtn);
        h->addWidget(m_logOpenDirBtn);

        form->addRow(tr("Log file:"), row);

        connect(m_logBrowseBtn,  &QToolButton::clicked, this, &SettingsDialog::onBrowseLogFile);
        connect(m_logOpenDirBtn, &QToolButton::clicked, this, &SettingsDialog::onOpenLogDir);
    }

    // Audit dir path row
    {
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(6);

        m_auditDirEdit = new QLineEdit(row);
        m_auditDirEdit->setPlaceholderText(tr("Empty = default (AppLocalDataLocation/audit)"));

        m_auditBrowseBtn  = makeIconBtn(row, browseIcon,  tr("Choose audit directory…"));
        m_auditOpenDirBtn = makeIconBtn(row, openDirIcon, tr("Open audit directory"));

        h->addWidget(m_auditDirEdit, 1);
        h->addWidget(m_auditBrowseBtn);
        h->addWidget(m_auditOpenDirBtn);

        form->addRow(tr("Audit dir:"), row);

        connect(m_auditBrowseBtn,  &QToolButton::clicked, this, &SettingsDialog::onBrowseAuditDir);
        connect(m_auditOpenDirBtn, &QToolButton::clicked, this, &SettingsDialog::onOpenAuditDir);
    }

    // -------------------------
    // App lock (startup password)
    // -------------------------
    {
        auto* row = new QWidget(this);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0,0,0,0);
        h->setSpacing(8);

        m_appLockCheck = new QCheckBox(tr("Require password on startup"), row);

        m_setAppPassBtn = new QPushButton(tr("Set / change…"), row);
        m_setAppPassBtn->setToolTip(tr("Set or change the application startup password"));

        m_disableAppLockBtn = new QPushButton(tr("Disable"), row);
        m_disableAppLockBtn->setToolTip(tr("Disable app lock and remove stored password hash"));

        m_appLockStatus = new QLabel(row);
        m_appLockStatus->setStyleSheet("color:#888;");

        h->addWidget(m_appLockCheck, 1);
        h->addWidget(m_setAppPassBtn, 0);
        h->addWidget(m_disableAppLockBtn, 0);
        h->addWidget(m_appLockStatus, 0);

        form->addRow(tr("App lock:"), row);

        connect(m_setAppPassBtn, &QPushButton::clicked, this, &SettingsDialog::onSetAppPasswordClicked);
        connect(m_disableAppLockBtn, &QPushButton::clicked, this, &SettingsDialog::onDisableAppLockClicked);

        connect(m_appLockCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_setAppPassBtn) m_setAppPassBtn->setEnabled(true);        // always allow setting
            if (m_disableAppLockBtn) m_disableAppLockBtn->setEnabled(on);  // only makes sense if enabled
        });
    }

    outer->addLayout(form);

    // Buttons
    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel,
        this
    );
    outer->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (auto* applyBtn = m_buttons->button(QDialogButtonBox::Apply)) {
        connect(applyBtn, &QPushButton::clicked, this, [this]() {
            const bool langChanged = applySettings();

            emit settingsApplied(langChanged); // <-- NEW

            if (langChanged && !m_restartWarned) {
                m_restartWarned = true;
                QMessageBox::information(
                    this,
                    tr("Restart required"),
                    tr("Language change will take effect after restarting the application.")
                );
            }
        });
    }
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

    // Language
    const QString lang = s.value("ui/language", "en").toString();
    for (int i = 0; i < m_languageCombo->count(); ++i) {
        if (m_languageCombo->itemData(i).toString() == lang) {
            m_languageCombo->setCurrentIndex(i);
            break;
        }
    }

    // Paths
    if (m_logFileEdit)
        m_logFileEdit->setText(s.value("logging/filePath", "").toString());

    if (m_auditDirEdit)
        m_auditDirEdit->setText(s.value("audit/dirPath", "").toString());

    // App lock
    const bool enabled = s.value("appLock/enabled", false).toBool();
    const QString hash = s.value("appLock/hash", "").toString();

    if (m_appLockCheck) m_appLockCheck->setChecked(enabled);

    const bool hasHash = !hash.trimmed().isEmpty();
    if (m_appLockStatus) {
        if (enabled && hasHash) m_appLockStatus->setText(tr("Password set"));
        else if (enabled && !hasHash) m_appLockStatus->setText(tr("Enabled, but no password set!"));
        else m_appLockStatus->setText(tr("Off"));
    }

    if (m_disableAppLockBtn)
        m_disableAppLockBtn->setEnabled(enabled);
}

void SettingsDialog::saveToSettings()
{
    QSettings s;

    s.setValue("ui/theme", m_themeCombo ? m_themeCombo->currentData().toString() : "cpunk-dark");
    s.setValue("logging/level", m_logLevelCombo ? m_logLevelCombo->currentData().toInt() : 1);

    // IMPORTANT: allow empty = revert to default
    s.setValue("logging/filePath", m_logFileEdit ? m_logFileEdit->text().trimmed() : QString());
    s.setValue("audit/dirPath",    m_auditDirEdit ? m_auditDirEdit->text().trimmed() : QString());

    s.setValue("ui/language", m_languageCombo ? m_languageCombo->currentData().toString() : "en");

    // App lock enabled flag only (hash is written by Set/Disable buttons)
    const bool enabled = (m_appLockCheck && m_appLockCheck->isChecked());
    s.setValue("appLock/enabled", enabled);
}

void SettingsDialog::onAccepted()
{
    const bool langChanged = applySettings();

    emit settingsApplied(langChanged); // <-- NEW (optional but nice)

    if (langChanged && !m_restartWarned) {
        m_restartWarned = true;
        QMessageBox::information(
            this,
            tr("Restart required"),
            tr("Language change will take effect after restarting the application.")
        );
    }

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
        tr("Choose log file"),
        QDir(startDir).filePath("pq-ssh.log"),
        tr("Log files (*.log);;All files (*)")
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
        tr("Choose audit directory"),
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

void SettingsDialog::onSetAppPasswordClicked()
{
    // Prompt new password twice
    bool ok1 = false;
    const QString pass1 = QInputDialog::getText(
        this,
        tr("Set application password"),
        tr("Enter new password:"),
        QLineEdit::Password,
        QString(), &ok1
    );

    if (!ok1 || pass1.isEmpty())
        return;

    bool ok2 = false;
    const QString pass2 = QInputDialog::getText(
        this,
        tr("Confirm password"),
        tr("Re-enter password:"),
        QLineEdit::Password,
        QString(), &ok2
    );

    if (!ok2)
        return;

    if (pass1 != pass2) {
        QMessageBox::warning(
            this,
            tr("Password mismatch"),
            tr("Passwords do not match.")
        );
        return;
    }

    // Hash with libsodium
    char out[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(
            out,
            pass1.toUtf8().constData(),
            (unsigned long long)pass1.toUtf8().size(),
            crypto_pwhash_OPSLIMIT_MODERATE,
            crypto_pwhash_MEMLIMIT_MODERATE) != 0) {
        QMessageBox::critical(
            this,
            tr("Error"),
            tr("Failed to generate password hash (out of memory?).")
        );
        return;
    }

    QSettings s;
    s.setValue("appLock/hash", QString::fromLatin1(out));
    s.setValue("appLock/enabled", true);

    if (m_appLockCheck) m_appLockCheck->setChecked(true);
    if (m_appLockStatus) m_appLockStatus->setText(tr("Password set"));
    if (m_disableAppLockBtn) m_disableAppLockBtn->setEnabled(true);

    QMessageBox::information(
        this,
        tr("App lock enabled"),
        tr("Startup password has been set.")
    );
}

void SettingsDialog::onDisableAppLockClicked()
{
    const auto ans = QMessageBox::question(
        this,
        tr("Disable app lock"),
        tr("Disable app lock and remove the stored password hash?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel
    );

    if (ans != QMessageBox::Yes)
        return;

    QSettings s;
    s.setValue("appLock/enabled", false);
    s.remove("appLock/hash");

    if (m_appLockCheck) m_appLockCheck->setChecked(false);
    if (m_appLockStatus) m_appLockStatus->setText(tr("Off"));
    if (m_disableAppLockBtn) m_disableAppLockBtn->setEnabled(false);

    QMessageBox::information(
        this,
        tr("App lock disabled"),
        tr("App startup password has been removed.")
    );
}
