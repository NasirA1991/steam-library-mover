// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

namespace slm {

// Machine-wide advisory lock shared by the GUI and CLI. The lock is held for the
// lifetime of the object; the kernel releases it if the process exits or crashes.
class InstanceLock
{
public:
    InstanceLock() = default;
    ~InstanceLock();
    InstanceLock(const InstanceLock &) = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;

    // An empty path uses the machine-wide default location. A custom path is
    // used by tests.
    bool acquire(QString *error = nullptr, const QString &path = {});
    bool isLocked() const { return m_fd >= 0; }
    QString path() const { return m_path; }

private:
    bool openAndLock(const QString &path, QString *error);
    void release();

    int m_fd = -1;
    QString m_path;
};

} // namespace slm
