// SPDX-License-Identifier: GPL-3.0-or-later
#include "appmanifest.h"

#include "vdf.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace slm {

namespace {

constexpr qint64 kMaxManifestSize = 8 * 1024 * 1024;

quint64 toUInt64(const QString &value, bool *ok = nullptr)
{
    return value.toULongLong(ok, 10);
}

bool parseAppIdFromFileName(const QString &path, quint32 *appId)
{
    static const QRegularExpression expression(
        QStringLiteral(R"(^appmanifest_([0-9]{1,10})\.acf$)"));
    const QRegularExpressionMatch match = expression.match(QFileInfo(path).fileName());
    if (!match.hasMatch())
        return false;
    bool ok = false;
    const qulonglong value = match.captured(1).toULongLong(&ok, 10);
    if (!ok || value == 0 || value > 0xffffffffULL)
        return false;
    *appId = static_cast<quint32>(value);
    return true;
}

} // namespace

bool isSafeInstallDirName(const QString &name)
{
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    if (name.contains(QChar(0)))
        return false;
    return true;
}

ManifestLoadResult loadAppManifest(const QString &path)
{
    ManifestLoadResult result;
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        result.error = QStringLiteral("Manifest does not exist: %1").arg(path);
        return result;
    }
    if (info.size() <= 0 || info.size() > kMaxManifestSize) {
        result.error = QStringLiteral("Manifest size is suspicious: %1").arg(path);
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot read manifest %1: %2").arg(path, file.errorString());
        return result;
    }
    const QByteArray data = file.readAll();
    file.close();

    QString parseError;
    int offset = 0;
    const auto document = VdfDocument::parse(data, &parseError, &offset);
    if (!document) {
        result.error = QStringLiteral("Invalid manifest %1 at byte %2: %3")
                           .arg(path)
                           .arg(offset)
                           .arg(parseError);
        return result;
    }

    const VdfNode *state = document->object(QStringLiteral("AppState"));
    if (!state) {
        result.error = QStringLiteral("Manifest has no AppState object: %1").arg(path);
        return result;
    }

    quint32 fileNameAppId = 0;
    if (!parseAppIdFromFileName(path, &fileNameAppId)) {
        result.error = QStringLiteral("Manifest filename is not appmanifest_<appid>.acf: %1").arg(path);
        return result;
    }

    bool idOk = false;
    const qulonglong idValue = state->childValue(QStringLiteral("appid")).toULongLong(&idOk, 10);
    if (!idOk || idValue == 0 || idValue > 0xffffffffULL) {
        result.error = QStringLiteral("Manifest has an invalid appid: %1").arg(path);
        return result;
    }
    if (static_cast<quint32>(idValue) != fileNameAppId) {
        result.error = QStringLiteral("Manifest appid %1 does not match filename %2")
                           .arg(idValue)
                           .arg(fileNameAppId);
        return result;
    }

    bool flagsOk = false;
    const qulonglong flags = state->childValue(QStringLiteral("StateFlags")).toULongLong(&flagsOk, 10);
    if (!flagsOk || flags > 0xffffffffULL) {
        result.error = QStringLiteral("Manifest has invalid StateFlags: %1").arg(path);
        return result;
    }

    bool sizeOk = false;
    const qulonglong size = toUInt64(state->childValue(QStringLiteral("SizeOnDisk")), &sizeOk);

    bool lastPlayedOk = false;
    const qlonglong lastPlayed =
        state->childValue(QStringLiteral("LastPlayed")).toLongLong(&lastPlayedOk, 10);
    bool buildOk = false;
    const qulonglong buildId =
        state->childValue(QStringLiteral("buildid")).toULongLong(&buildOk, 10);

    const QString name = state->childValue(QStringLiteral("name"));
    const QString installDir = state->childValue(QStringLiteral("installdir"));

    result.manifest.appId = static_cast<quint32>(idValue);
    result.manifest.name = name;
    result.manifest.installDir = installDir;
    result.manifest.stateFlags = static_cast<quint32>(flags);
    result.manifest.sizeOnDisk = sizeOk ? size : 0;
    result.manifest.lastPlayed = lastPlayedOk ? lastPlayed : 0;
    result.manifest.buildId = buildOk ? buildId : 0;
    result.manifest.path = QFileInfo(path).absoluteFilePath();
    result.ok = true;
    return result;
}

} // namespace slm
