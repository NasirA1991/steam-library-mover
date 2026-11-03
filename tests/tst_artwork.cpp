// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/artworkprovider.h"

#include <QDir>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class ArtworkTest : public QObject
{
    Q_OBJECT
private slots:
    void cacheHitEmitsReady();
    void localArtworkEmitsReady();
    void disabledProviderDoesNothing();
};

void ArtworkTest::cacheHitEmitsReady()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_ARTWORK_DIR", temp.path().toUtf8());
    qunsetenv("SLM_DISABLE_ARTWORK");
    const QString path = temp.path() + QStringLiteral("/42_library_600x900.jpg");
    QImage image(8, 12, QImage::Format_RGB32);
    image.fill(Qt::darkBlue);
    QVERIFY(image.save(path, "JPG"));

    ArtworkProvider provider;
    QSignalSpy spy(&provider, &ArtworkProvider::artworkReady);
    provider.request(42);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toUInt(), 42u);
    QCOMPARE(spy.at(0).at(1).toString(), path);
}

void ArtworkTest::localArtworkEmitsReady()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_ARTWORK_DIR", temp.path().toUtf8());
    qunsetenv("SLM_DISABLE_ARTWORK");
    const QString local = temp.path() + QStringLiteral("/local.jpg");
    QImage image(8, 12, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QVERIFY(image.save(local, "JPG"));

    ArtworkProvider provider;
    QSignalSpy spy(&provider, &ArtworkProvider::artworkReady);
    provider.request(43, local);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toUInt(), 43u);
    QCOMPARE(spy.at(0).at(1).toString(), local);
}

void ArtworkTest::disabledProviderDoesNothing()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    qputenv("SLM_ARTWORK_DIR", temp.path().toUtf8());
    qputenv("SLM_DISABLE_ARTWORK", "1");
    ArtworkProvider provider;
    QVERIFY(!provider.isEnabled());
    QSignalSpy spy(&provider, &ArtworkProvider::artworkReady);
    provider.request(44);
    QCOMPARE(spy.count(), 0);
    qunsetenv("SLM_DISABLE_ARTWORK");
}

QTEST_GUILESS_MAIN(ArtworkTest)
#include "tst_artwork.moc"
