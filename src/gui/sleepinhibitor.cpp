// SPDX-License-Identifier: GPL-3.0-or-later
#include "sleepinhibitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

SleepInhibitor::~SleepInhibitor()
{
    release();
}

bool SleepInhibitor::inhibit(const QString &reason)
{
    release();
    m_error.clear();

    if (QDBusConnection::systemBus().isConnected()) {
        QDBusInterface login1(QStringLiteral("org.freedesktop.login1"),
                              QStringLiteral("/org/freedesktop/login1"),
                              QStringLiteral("org.freedesktop.login1.Manager"),
                              QDBusConnection::systemBus());
        if (login1.isValid()) {
            const QDBusReply<QDBusUnixFileDescriptor> reply =
                login1.call(QStringLiteral("Inhibit"), QStringLiteral("sleep"),
                            QStringLiteral("Steam Library Mover"), reason,
                            QStringLiteral("block"));
            if (reply.isValid() && reply.value().isValid()) {
                m_loginFd = reply.value();
                m_active = true;
                return true;
            }
        }
    }

    // Fallback for environments where the login1 system bus is unavailable
    // (notably some sandboxes): inhibit the desktop screensaver. It is not a
    // full sleep inhibitor, but still helps on a desktop session.
    if (QDBusConnection::sessionBus().isConnected()) {
        QDBusInterface screensaver(QStringLiteral("org.freedesktop.ScreenSaver"),
                                   QStringLiteral("/org/freedesktop/ScreenSaver"),
                                   QStringLiteral("org.freedesktop.ScreenSaver"),
                                   QDBusConnection::sessionBus());
        if (screensaver.isValid()) {
            const QDBusReply<unsigned int> reply = screensaver.call(
                QStringLiteral("Inhibit"), QStringLiteral("Steam Library Mover"), reason);
            if (reply.isValid()) {
                m_screenSaverCookie = reply.value();
                m_active = true;
                return true;
            }
        }
    }

    m_error = QStringLiteral("Could not acquire a sleep inhibitor");
    return false;
}

void SleepInhibitor::release()
{
    if (m_loginFd.isValid())
        m_loginFd = QDBusUnixFileDescriptor();
    if (m_screenSaverCookie != 0) {
        QDBusInterface screensaver(QStringLiteral("org.freedesktop.ScreenSaver"),
                                   QStringLiteral("/org/freedesktop/ScreenSaver"),
                                   QStringLiteral("org.freedesktop.ScreenSaver"),
                                   QDBusConnection::sessionBus());
        if (screensaver.isValid())
            screensaver.call(QStringLiteral("UnInhibit"), m_screenSaverCookie);
        m_screenSaverCookie = 0;
    }
    m_active = false;
}
