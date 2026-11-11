// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/instance_lock.h"

#include <QTemporaryDir>
#include <QtTest>

using namespace slm;

class InstanceLockTest : public QObject
{
    Q_OBJECT
private slots:
    void secondLockIsRejected();
    void lockIsReusableAfterRelease();
};

void InstanceLockTest::secondLockIsRejected()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.path() + QStringLiteral("/instance.lock");

    InstanceLock first;
    QString error;
    QVERIFY2(first.acquire(&error, path), qPrintable(error));

    InstanceLock second;
    error.clear();
    QVERIFY(!second.acquire(&error, path));
    QVERIFY2(error.contains(QStringLiteral("already running")), qPrintable(error));
}

void InstanceLockTest::lockIsReusableAfterRelease()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.path() + QStringLiteral("/instance.lock");
    {
        InstanceLock first;
        QString error;
        QVERIFY2(first.acquire(&error, path), qPrintable(error));
    }
    InstanceLock second;
    QString error;
    QVERIFY2(second.acquire(&error, path), qPrintable(error));
}

QTEST_MAIN(InstanceLockTest)
#include "tst_instance_lock.moc"
