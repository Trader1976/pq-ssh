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
//
// SettingsDialog is the preferences UI for PQ-SSH.
// It is responsible for:
// - Presenting editable settings (theme, language, log/audit paths, app lock)
// - Reading/writing those values through QSettings
// - Emitting a signal when settings are applied so the caller can react
//
// It is NOT responsible for:
// - Applying themes globally (caller/MainWindow does that)
// - Starting/stopping logging/audit subsystems directly (caller does that)
// - Implementing the app unlock flow (MainWindow owns startup gating)
//
// Notes:
// - Language changes typically require app restart to reload translators cleanly.
// - Empty path fields intentionally mean “use default location”.

// Apply current UI state into QSettings and return whether language changed.
// This is used by Apply/OK handlers to decide whether to show a restart notice.
bool SettingsDialog::applySettings()
{
    QSettings s;

    const QString oldLang  = s.value("ui/language", "en").toString().trimmed();
    saveToSettings();
    s.sync();

    const QString newLang  = s.value("ui/language", "en").toString().trimmed();
    return (newLang != oldLang);
}

// Construct the dialog, build widgets, and initialize UI state from QSettings.
SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent)
{
    buildUi();
    loadFromSettings();
}

// Create a small toolbutton with an icon and tooltip.
// Used for browse/open-directory buttons next to path edits.
static QToolButton* makeIconBtn(QWidget* parent, const QIcon& icon, const QString& tooltip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon);
    b->setToolTip(tooltip); // caller should pass already-translated text
    b->setAutoRaise(true);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

// Build all widgets and wire signals.
// The dialog supports Apply (non-modal updates) as well as OK/Cancel.
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
    m_languageCombo->addItem("Chinese 中文（简体）", "zh");
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

    // Log file path row: free-form path (empty = default), plus browse + open-dir.
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

    // Audit directory row: directory picker (empty = default), plus browse + open-dir.
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
    // Stores:
    // - appLock/enabled (bool)
    // - appLock/hash    (libsodium crypto_pwhash_str output)
    //
    // The checkbox only toggles enabled state. Hash is managed by Set/Disable buttons.
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
        // Status label is informational only; keep it subtle.
        m_appLockStatus->setStyleSheet("color:#888;");

        h->addWidget(m_appLockCheck, 1);
        h->addWidget(m_setAppPassBtn, 0);
        h->addWidget(m_disableAppLockBtn, 0);
        h->addWidget(m_appLockStatus, 0);

        form->addRow(tr("App lock:"), row);

        connect(m_setAppPassBtn, &QPushButton::clicked, this, &SettingsDialog::onSetAppPasswordClicked);
        connect(m_disableAppLockBtn, &QPushButton::clicked, this, &SettingsDialog::onDisableAppLockClicked);

        // Disable button only makes sense if lock is enabled.
        connect(m_appLockCheck, &QCheckBox::toggled, this, [this](bool on) {
            if (m_setAppPassBtn) m_setAppPassBtn->setEnabled(true);        // always allow setting
            if (m_disableAppLockBtn) m_disableAppLockBtn->setEnabled(on);  // only makes sense if enabled
        });
    }

    outer->addLayout(form);

    // Buttons: OK applies + closes, Apply applies without closing, Cancel closes.
    m_buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel,
        this
    );
    outer->addWidget(m_buttons);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Apply button handler: write settings, emit signal, and optionally show restart info.
    if (auto* applyBtn = m_buttons->button(QDialogButtonBox::Apply)) {
        connect(applyBtn, &QPushButton::clicked, this, [this]() {
            const bool langChanged = applySettings();

            emit settingsApplied(langChanged);

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

// Populate UI widgets from QSettings.
// This is called once after buildUi(), and can be reused if you ever add “Reset”.
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

// Write current UI values into QSettings.
// This does not call sync(); callers decide when to commit (Apply/OK).
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

// OK button handler: apply settings and close dialog.
// Emits settingsApplied(langChanged) so the caller can re-apply theme/logging immediately.
void SettingsDialog::onAccepted()
{
    const bool langChanged = applySettings();

    emit settingsApplied(langChanged);

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

// Utility: return the directory for a given file/dir path.
// - If the input is a directory path, returns it.
// - If the input is a file path, returns its parent directory.
// - Empty input returns empty.
QString SettingsDialog::dirOfPathOrEmpty(const QString& path) const
{
    const QString p = path.trimmed();
    if (p.isEmpty()) return QString();
    const QFileInfo fi(p);
    return fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
}

// ---- Buttons ----

// Browse for a log file path (save file dialog).
// Choosing a path overrides the default log directory; empty means revert to default.
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

// Open the directory containing the currently configured log file.
// If no custom log file is set, open the default logs directory.
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

// Browse for an audit directory.
// Empty means revert to default AppLocalDataLocation/audit.
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

// Open the configured audit directory, or fall back to default audit dir if empty.
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

// Set/change the application startup password.
// Stores a libsodium password hash into QSettings and enables appLock/enabled.
//
// Security notes:
// - We never store the plaintext password.
// - crypto_pwhash_str embeds salt and parameters in the output string.
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

    // Reflect immediately in UI
    if (m_appLockCheck) m_appLockCheck->setChecked(true);
    if (m_appLockStatus) m_appLockStatus->setText(tr("Password set"));
    if (m_disableAppLockBtn) m_disableAppLockBtn->setEnabled(true);

    QMessageBox::information(
        this,
        tr("App lock enabled"),
        tr("Startup password has been set.")
    );
}

// Disable app lock and remove the stored password hash.
// This is a destructive operation; confirm with the user first.
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

    // Reflect immediately in UI
    if (m_appLockCheck) m_appLockCheck->setChecked(false);
    if (m_appLockStatus) m_appLockStatus->setText(tr("Off"));
    if (m_disableAppLockBtn) m_disableAppLockBtn->setEnabled(false);

    QMessageBox::information(
        this,
        tr("App lock disabled"),
        tr("App startup password has been removed.")
    );
}
