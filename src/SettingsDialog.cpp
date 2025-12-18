#include "SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QSettings>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Settings");
    resize(420, 160);
    buildUi();
    loadFromSettings();
}

void SettingsDialog::buildUi()
{
    auto *outer = new QVBoxLayout(this);

    auto *form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight);

    m_themeCombo = new QComboBox(this);
    // Future-proof: value is an internal id, label is user-facing
    m_themeCombo->addItem("CPUNK Dark", "cpunk-dark");

    m_logLevelCombo = new QComboBox(this);
    // 0..2, matches Logger::setLogLevel(int)
    m_logLevelCombo->addItem("Errors only", 0);
    m_logLevelCombo->addItem("Normal", 1);
    m_logLevelCombo->addItem("Debug", 2);

    form->addRow("App theme:", m_themeCombo);
    form->addRow("Logging level:", m_logLevelCombo);

    outer->addLayout(form);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::onAccepted);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    outer->addWidget(m_buttons);
    setLayout(outer);
}

void SettingsDialog::loadFromSettings()
{
    QSettings s;

    const QString theme = s.value("ui/theme", "cpunk-dark").toString();
    for (int i = 0; i < m_themeCombo->count(); ++i) {
        if (m_themeCombo->itemData(i).toString() == theme) {
            m_themeCombo->setCurrentIndex(i);
            break;
        }
    }

    const int lvl = s.value("logging/level", 1).toInt();
    for (int i = 0; i < m_logLevelCombo->count(); ++i) {
        if (m_logLevelCombo->itemData(i).toInt() == lvl) {
            m_logLevelCombo->setCurrentIndex(i);
            break;
        }
    }
}

void SettingsDialog::saveToSettings()
{
    QSettings s;
    s.setValue("ui/theme", m_themeCombo->currentData().toString());
    s.setValue("logging/level", m_logLevelCombo->currentData().toInt());
}

void SettingsDialog::onAccepted()
{
    saveToSettings();
    accept();
}
