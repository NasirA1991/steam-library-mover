// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;

// Application preferences. The dialog only collects values; MainWindow applies
// the theme and density changes after the dialog is accepted.
class SettingsDialog : public QDialog
{
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    QString theme() const;
    QString steamRoot() const;
    bool compactRows() const;
    bool fullVerify() const;
    bool moveShaderCache() const;
    bool moveWorkshop() const;
    bool downloadArtwork() const;

private:
    QComboBox *m_theme = nullptr;
    QLineEdit *m_steamRoot = nullptr;
    QCheckBox *m_compact = nullptr;
    QCheckBox *m_verify = nullptr;
    QCheckBox *m_shader = nullptr;
    QCheckBox *m_workshop = nullptr;
    QCheckBox *m_artwork = nullptr;
};
