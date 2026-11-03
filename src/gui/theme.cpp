// SPDX-License-Identifier: GPL-3.0-or-later
#include "theme.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QIcon>
#include <QStyle>
#include <QStyleFactory>
#include <QPalette>
#include <QSettings>
#include <QString>

namespace ui {

namespace {

QPalette makeLightPalette()
{
    QPalette p;
    const QColor text(QStringLiteral("#1b1f23"));
    const QColor window(QStringLiteral("#eef1f4"));
    const QColor base(QStringLiteral("#ffffff"));
    const QColor button(QStringLiteral("#e3e7eb"));
    const QColor mid(QStringLiteral("#b8c0c9"));
    const QColor highlight(QStringLiteral("#2d6cdf"));
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#f4f6f8")));
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(QStringLiteral("#c0392b")));
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#6b7480")));
    p.setColor(QPalette::Light, QColor(QStringLiteral("#ffffff")));
    p.setColor(QPalette::Midlight, QColor(QStringLiteral("#d8dee4")));
    p.setColor(QPalette::Mid, mid);
    p.setColor(QPalette::Dark, QColor(QStringLiteral("#8b95a1")));
    p.setColor(QPalette::Shadow, QColor(QStringLiteral("#6b7480")));

    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#8b95a1")));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#8b95a1")));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#8b95a1")));
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#c8d1db")));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, Qt::white);
    return p;
}

QPalette makeDarkPalette()
{
    QPalette p;
    const QColor text(QStringLiteral("#e6e8eb"));
    const QColor window(QStringLiteral("#1b1d21"));
    const QColor base(QStringLiteral("#23262b"));
    const QColor button(QStringLiteral("#2b2f35"));
    const QColor mid(QStringLiteral("#454b54"));
    const QColor highlight(QStringLiteral("#3b82f6"));
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#282c32")));
    p.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#2d3137")));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(QStringLiteral("#ff6b6b")));
    p.setColor(QPalette::Link, QColor(QStringLiteral("#79b8ff")));
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#9aa4af")));
    p.setColor(QPalette::Light, QColor(QStringLiteral("#343941")));
    p.setColor(QPalette::Midlight, QColor(QStringLiteral("#2f343a")));
    p.setColor(QPalette::Mid, mid);
    p.setColor(QPalette::Dark, QColor(QStringLiteral("#141619")));
    p.setColor(QPalette::Shadow, Qt::black);

    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(QStringLiteral("#7b838d")));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#7b838d")));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#7b838d")));
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(QStringLiteral("#3c434c")));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(QStringLiteral("#9aa4af")));
    return p;
}

bool g_capturedSystem = false;
bool g_explicitThemeApplied = false;
QPalette g_systemPalette;
QString g_originalStyleName;
QString g_originalIconTheme;

void captureSystemPalette()
{
    if (g_capturedSystem)
        return;
    g_systemPalette = QApplication::palette();
    g_originalStyleName = QApplication::style()->objectName();
    if (g_originalStyleName.isEmpty())
        g_originalStyleName = QStringLiteral("Fusion");
    g_originalIconTheme = QIcon::themeName();
    g_capturedSystem = true;
}

void setStyleByName(const QString &name)
{
    if (QApplication::style()
        && QApplication::style()->objectName().compare(name, Qt::CaseInsensitive) == 0) {
        return;
    }
    QStyle *style = QStyleFactory::create(name);
    if (style)
        QApplication::setStyle(style);
}

void setIconThemeIfAvailable(const QString &name)
{
    if (name.isEmpty() || name == QIcon::themeName())
        return;
    for (const QString &root : QIcon::themeSearchPaths()) {
        if (QDir(root + QLatin1Char('/') + name).exists()) {
            QIcon::setThemeName(name);
            return;
        }
    }
}

} // namespace

Theme themeFromName(const QString &name)
{
    if (name.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0)
        return Theme::Light;
    if (name.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0)
        return Theme::Dark;
    return Theme::System;
}

Theme themeFromSettings()
{
    const QString env = qEnvironmentVariable("SLM_THEME");
    if (!env.isEmpty())
        return themeFromName(env);
    QSettings settings;
    return themeFromName(settings.value(QStringLiteral("appearance/theme"), QStringLiteral("system"))
                             .toString());
}

QString themeName(Theme theme)
{
    switch (theme) {
    case Theme::Light:
        return QStringLiteral("light");
    case Theme::Dark:
        return QStringLiteral("dark");
    case Theme::System:
        break;
    }
    return QStringLiteral("system");
}

void applyTheme(Theme theme)
{
    captureSystemPalette();
    if (theme == Theme::System) {
        if (!g_explicitThemeApplied)
            return;
        setStyleByName(g_originalStyleName);
        setIconThemeIfAvailable(g_originalIconTheme);
        QApplication::setPalette(g_systemPalette);
        g_explicitThemeApplied = false;
        return;
    }

    // Breeze's menu bar and toolbar read KDE's global colour scheme directly,
    // which is why an application palette alone left the window chrome dark
    // even in Light mode. Use the palette-driven Fusion style for explicit
    // Light/Dark themes and restore the native style for "Follow system".
    setStyleByName(QStringLiteral("Fusion"));
    setIconThemeIfAvailable(theme == Theme::Light ? QStringLiteral("breeze")
                                                   : QStringLiteral("breeze-dark"));
    QApplication::setPalette(theme == Theme::Light ? makeLightPalette() : makeDarkPalette());
    g_explicitThemeApplied = true;
}

bool isDarkTheme()
{
    const QColor window = QApplication::palette().color(QPalette::Window);
    return window.lightness() < 128;
}

} // namespace ui
