// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QFile>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace slm {

namespace JournalStore {
QString stateDir();
QString logsDir();
QString backupsDir();
QString operationsDir();
bool ensure(QString *error = nullptr);
QString newOperationId();
bool isValidOperationId(const QString &opId);
QString operationDir(const QString &opId);
QString journalPath(const QString &opId);
QString logPath(const QString &opId);
QString backupDir(const QString &opId);
bool writeObject(const QString &path, const QJsonObject &object, QString *error = nullptr);
QJsonObject readObject(const QString &path, QString *error = nullptr);
QList<QJsonObject> listOperations();
bool removeOperation(const QString &opId, QString *error = nullptr);
// Deletes the oldest finished operations, keeping the most recent `keep`.
// Unfinished/failed/cancelled journals are never removed automatically.
int pruneFinished(int keep = 20);
} // namespace JournalStore

class Logger
{
public:
    explicit Logger(const QString &path);
    ~Logger();
    bool open(QString *error = nullptr);
    void write(const QString &message);
    QString path() const { return m_path; }

private:
    QString m_path;
    QFile *m_file = nullptr;
};

} // namespace slm
