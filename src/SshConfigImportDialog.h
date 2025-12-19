#pragma once
#include <QDialog>
#include "SshConfigParser.h"

class QLabel;
class QTableWidget;
class QPushButton;

class SshConfigImportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SshConfigImportDialog(const QString& configPath, QWidget* parent = nullptr);

public slots:
    void reload();

private:
    void buildUi();
    void populate();

    QString m_path;
    SshConfigParseResult m_result;

    QLabel* m_title = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_warnings = nullptr;
    QTableWidget* m_table = nullptr;

    QPushButton* m_reloadBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
};
