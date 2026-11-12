// SPDX-License-Identifier: GPL-3.0-or-later
#include "settingsdialog.h"

#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QSettings>
#include <QVBoxLayout>

namespace {

void setMutedLabel(QLabel *label)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText,
                     ui::isDarkTheme() ? QColor(QStringLiteral("#9aa4af"))
                                       : QColor(QStringLiteral("#6b7480")));
    label->setPalette(palette);
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    QSettings settings;

    setWindowTitle(QStringLiteral("Settings"));
    auto *layout = new QVBoxLayout(this);

    m_theme = new QComboBox(this);
    m_theme->addItem(QStringLiteral("Follow system theme"), QStringLiteral("system"));
    m_theme->addItem(QStringLiteral("Light"), QStringLiteral("light"));
    m_theme->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
    const QString currentTheme =
        settings.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
    const int themeIndex = m_theme->findData(currentTheme);
    if (themeIndex >= 0)
        m_theme->setCurrentIndex(themeIndex);

    m_steamRoot = new QLineEdit(this);
    m_steamRoot->setPlaceholderText(QStringLiteral("Automatic detection"));
    m_steamRoot->setText(settings.value(QStringLiteral("steam/root")).toString());

    auto *rootForm = new QFormLayout();
    rootForm->addRow(QStringLiteral("Theme:"), m_theme);
    auto *themeNote = new QLabel(
        QStringLiteral("The window title bar is drawn by KDE and follows your system window "
                       "decoration. The app's menus, toolbar and content follow the selected "
                       "theme."),
        this);
    themeNote->setWordWrap(true);
    setMutedLabel(themeNote);
    rootForm->addRow(QString(), themeNote);
    rootForm->addRow(QStringLiteral("Steam root override:"), m_steamRoot);
    layout->addLayout(rootForm);

    m_compact = new QCheckBox(
        QStringLiteral("Compact game rows (smaller artwork and title, more games visible)"), this);
    m_compact->setChecked(settings.value(QStringLiteral("appearance/compact"), true).toBool());
    layout->addWidget(m_compact);

    m_verify = new QCheckBox(
        QStringLiteral("Use full SHA-256 verification by default (recommended)"), this);
    m_verify->setChecked(settings.value(QStringLiteral("verify/full"), true).toBool());

    m_shader = new QCheckBox(QStringLiteral("Move shader cache when present"), this);
    m_shader->setChecked(settings.value(QStringLiteral("components/shadercache"), true).toBool());

    m_workshop = new QCheckBox(QStringLiteral("Move Workshop content and manifest when present"),
                               this);
    m_workshop->setChecked(settings.value(QStringLiteral("components/workshop"), true).toBool());

    m_artwork = new QCheckBox(
        QStringLiteral("Download missing box art from Steam's public CDN (cached locally)"), this);
    m_artwork->setChecked(settings.value(QStringLiteral("artwork/download"), true).toBool());

    layout->addWidget(m_verify);
    layout->addWidget(m_shader);
    layout->addWidget(m_workshop);
    layout->addWidget(m_artwork);

    auto *note = new QLabel(QStringLiteral(
                                "Steam must be closed for every move. The mover never runs Steam's own "
                                "move operation and never deletes source data before verification."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString SettingsDialog::theme() const { return m_theme->currentData().toString(); }
QString SettingsDialog::steamRoot() const { return m_steamRoot->text().trimmed(); }
bool SettingsDialog::compactRows() const { return m_compact->isChecked(); }
bool SettingsDialog::fullVerify() const { return m_verify->isChecked(); }
bool SettingsDialog::moveShaderCache() const { return m_shader->isChecked(); }
bool SettingsDialog::moveWorkshop() const { return m_workshop->isChecked(); }
bool SettingsDialog::downloadArtwork() const { return m_artwork->isChecked(); }
