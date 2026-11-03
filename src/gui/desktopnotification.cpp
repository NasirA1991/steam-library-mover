// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktopnotification.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QStringList>
#include <QVariantMap>

namespace ui {

bool sendDesktopNotification(const QString &title, const QString &body, QString *error)
{
    if (!QDBusConnection::sessionBus().isConnected()) {
        if (error)
            *error = QStringLiteral("Session D-Bus is not available");
        return false;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"), QStringLiteral("Notify"));
    message << QStringLiteral("Steam Library Mover") << 0u
            << QStringLiteral("steam-library-mover") << title << body << QStringList()
            << QVariantMap() << 8000;
    const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::NoBlock);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (error)
            *error = reply.errorMessage();
        return false;
    }
    if (error)
        error->clear();
    return true;
}

} // namespace ui
