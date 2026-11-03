// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/appmanifest.h"

#include "test_helpers.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace slm;

class AppManifestTest : public QObject
{
    Q_OBJECT
private slots:
    void loadValid();
    void flagMatrix();
    void rejectMismatchedAppId();
    void rejectMalformed();
};

void AppManifestTest::loadValid()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.path() + QStringLiteral("/appmanifest_12345.acf");
    QVERIFY(testutil::writeFile(path, testutil::manifestText(12345, "Test", "Test Game", 4, 999,
                                                              1700000000, 424242)
                                          .toUtf8()));
    const ManifestLoadResult result = loadAppManifest(path);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.manifest.appId, 12345u);
    QCOMPARE(result.manifest.name, QStringLiteral("Test"));
    QCOMPARE(result.manifest.installDir, QStringLiteral("Test Game"));
    QCOMPARE(result.manifest.sizeOnDisk, 999ull);
    QCOMPARE(result.manifest.lastPlayed, 1700000000ll);
    QCOMPARE(result.manifest.buildId, 424242ull);
    QVERIFY(result.manifest.isFullyInstalled());
    QVERIFY(!result.manifest.hasNonIdleFlags());
}

void AppManifestTest::flagMatrix()
{
    AppManifest manifest;
    manifest.stateFlags = 4;
    QVERIFY(manifest.isFullyInstalled());
    QVERIFY(!manifest.hasNonIdleFlags());
    manifest.stateFlags = 6;
    QVERIFY(manifest.isFullyInstalled());
    QVERIFY(manifest.hasNonIdleFlags());
    QVERIFY(manifest.stateFlagNames().contains(QStringLiteral("UpdateRequired")));
    manifest.stateFlags = 1026;
    QVERIFY(manifest.hasNonIdleFlags());
    QVERIFY(manifest.stateFlagNames().contains(QStringLiteral("UpdateStarted")));
}

void AppManifestTest::rejectMismatchedAppId()
{
    QTemporaryDir temp;
    const QString path = temp.path() + QStringLiteral("/appmanifest_12345.acf");
    QVERIFY(testutil::writeFile(path, testutil::manifestText(99999, "Test", "Test").toUtf8()));
    const ManifestLoadResult result = loadAppManifest(path);
    QVERIFY(!result.ok);
}

void AppManifestTest::rejectMalformed()
{
    QTemporaryDir temp;
    const QString path = temp.path() + QStringLiteral("/appmanifest_12345.acf");
    QVERIFY(testutil::writeFile(path, QByteArray("\"AppState\" { \"appid\" ")));
    const ManifestLoadResult result = loadAppManifest(path);
    QVERIFY(!result.ok);
    QVERIFY(!result.error.isEmpty());
}

QTEST_GUILESS_MAIN(AppManifestTest)
#include "tst_appmanifest.moc"
