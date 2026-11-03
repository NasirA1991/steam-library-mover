// SPDX-License-Identifier: GPL-3.0-or-later
#include "journal.h"

#include "fileops.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <sys/stat.h>
#include <QStandardPaths>
#include <QUuid>

namespace slm {

namespace JournalStore {

QString stateDir()
{
    const QString override = qEnvironmentVariable("SLM_STATE_DIR");
    if (!override.isEmpty())
        return QDir::cleanPath(override);
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/state");
    return base + QStringLiteral("/steam-library-mover");
}

QString logsDir() { return stateDir() + QStringLiteral("/logs"); }
QString backupsDir() { return stateDir() + QStringLiteral("/backups"); }
QString operationsDir() { return stateDir() + QStringLiteral("/operations"); }

bool ensure(QString *error)
{
    const bool ok = fsutil::ensureDir(stateDir(), error) && fsutil::ensureDir(logsDir(), error)
        && fsutil::ensureDir(backupsDir(), error) && fsutil::ensureDir(operationsDir(), error);
    if (ok) {
        ::chmod(QFile::encodeName(stateDir()).constData(), 0700);
        ::chmod(QFile::encodeName(logsDir()).constData(), 0700);
        ::chmod(QFile::encodeName(backupsDir()).constData(), 0700);
        ::chmod(QFile::encodeName(operationsDir()).constData(), 0700);
    }
    return ok;
}

QString newOperationId()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'-"))
        + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

QString operationDir(const QString &opId) { return operationsDir() + QLatin1Char('/') + opId; }
QString journalPath(const QString &opId)
{
    return operationDir(opId) + QStringLiteral("/journal.json");
}
QString logPath(const QString &opId) { return operationDir(opId) + QStringLiteral("/log.txt"); }
QString backupDir(const QString &opId) { return operationDir(opId) + QStringLiteral("/backup"); }

bool writeObject(const QString &path, const QJsonObject &object, QString *error)
{
    if (!fsutil::ensureDir(QFileInfo(path).absolutePath(), error))
        return false;
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return fsutil::writeFileAtomic(path, data, 0600, error);
}

QJsonObject readObject(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return {};
    }
    QJsonParseError parseError {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid journal %1: %2").arg(path, parseError.errorString());
        return {};
    }
    return document.object();
}

QList<QJsonObject> listOperations()
{
    QList<QJsonObject> result;
    QDir dir(operationsDir());
    const QStringList ids = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &id : ids) {
        QString error;
        const QJsonObject object = readObject(journalPath(id), &error);
        if (!object.isEmpty())
            result.append(object);
    }
    return result;
}

bool removeOperation(const QString &opId, QString *error)
{
    return fsutil::removeTree(operationDir(opId), error);
}

} // namespace JournalStore

Logger::Logger(const QString &path)
    : m_path(path)
{
}

Logger::~Logger()
{
    delete m_file;
}

bool Logger::open(QString *error)
{
    if (m_file)
        return true;
    if (!fsutil::ensureDir(QFileInfo(m_path).absolutePath(), error))
        return false;
    m_file = new QFile(m_path);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Cannot open log %1: %2").arg(m_path, m_file->errorString());
        delete m_file;
        m_file = nullptr;
        return false;
    }
    return true;
}

void Logger::write(const QString &message)
{
    if (!m_file)
        return;
    const QString line = QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral(" ")
        + message + QLatin1Char('\n');
    m_file->write(line.toUtf8());
    m_file->flush();
}

} // namespace slm
