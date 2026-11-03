// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusUnixFileDescriptor>
#include <QString>

class SleepInhibitor
{
public:
    SleepInhibitor() = default;
    ~SleepInhibitor();

    bool inhibit(const QString &reason);
    void release();
    bool isActive() const { return m_active; }
    QString error() const { return m_error; }

private:
    QDBusUnixFileDescriptor m_loginFd;
    unsigned int m_screenSaverCookie = 0;
    bool m_active = false;
    QString m_error;
};
