// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QtTest>

class ThemeTest : public QObject
{
    Q_OBJECT
private slots:
    void darkAndLightContrast();
    void names();
};

void ThemeTest::darkAndLightContrast()
{
    ui::applyTheme(ui::Theme::Dark);
    const QPalette dark = QApplication::palette();
    QVERIFY(dark.color(QPalette::Window).lightness() < 80);
    QVERIFY(dark.color(QPalette::Text).lightness() > 180);
    QVERIFY(ui::isDarkTheme());
    QCOMPARE(QApplication::style()->objectName().toLower(), QStringLiteral("fusion"));

    ui::applyTheme(ui::Theme::Light);
    const QPalette light = QApplication::palette();
    QVERIFY(light.color(QPalette::Window).lightness() > 180);
    QVERIFY(light.color(QPalette::Text).lightness() < 80);
    QVERIFY(!ui::isDarkTheme());
    QCOMPARE(QApplication::style()->objectName().toLower(), QStringLiteral("fusion"));

    ui::applyTheme(ui::Theme::System);
}

void ThemeTest::names()
{
    QCOMPARE(ui::themeFromName(QStringLiteral("light")), ui::Theme::Light);
    QCOMPARE(ui::themeFromName(QStringLiteral("DARK")), ui::Theme::Dark);
    QCOMPARE(ui::themeFromName(QStringLiteral("anything-else")), ui::Theme::System);
    QCOMPARE(ui::themeName(ui::Theme::Dark), QStringLiteral("dark"));
}

QTEST_MAIN(ThemeTest)
#include "tst_theme.moc"
