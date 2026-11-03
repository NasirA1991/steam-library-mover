// SPDX-License-Identifier: GPL-3.0-or-later
#include "uiformat.h"

#include <QApplication>
#include <QDir>
#include <QPainter>
#include <QPixmap>
#include <QPen>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStyle>

namespace ui {

namespace {

QString decodeUdevLabel(const QString &encoded)
{
    QByteArray raw = encoded.toUtf8();
    QByteArray decoded;
    decoded.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        if (raw.at(i) == '\\' && i + 3 < raw.size() && raw.at(i + 1) == 'x') {
            bool ok = false;
            const int value = raw.mid(i + 2, 2).toInt(&ok, 16);
            if (ok) {
                decoded.append(static_cast<char>(value));
                i += 3;
                continue;
            }
        }
        decoded.append(raw.at(i));
    }
    return QString::fromUtf8(decoded);
}

QString labelFromDeviceSource(const QString &source)
{
    static QHash<QString, QString> cache;
    if (source.isEmpty())
        return {};
    const auto cached = cache.constFind(source);
    if (cached != cache.constEnd())
        return cached.value();

    QString result;
    QDir labels(QStringLiteral("/dev/disk/by-label"));
    if (labels.exists()) {
        const QString canonicalSource = QFileInfo(source).canonicalFilePath();
        if (!canonicalSource.isEmpty()) {
            const QStringList entries = labels.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
            for (const QString &entry : entries) {
                const QFileInfo info(labels.filePath(entry));
                if (!info.isSymLink())
                    continue;
                if (info.canonicalFilePath() == canonicalSource) {
                    result = decodeUdevLabel(entry);
                    break;
                }
            }
        }
    }
    cache.insert(source, result);
    return result;
}

QString fallbackName(const QString &path)
{
    QFileInfo info(path);
    QString base = info.fileName();
    if (base.compare(QStringLiteral("SteamLibrary"), Qt::CaseInsensitive) == 0
        || base.compare(QStringLiteral("Steam"), Qt::CaseInsensitive) == 0) {
        base = QFileInfo(info.absolutePath()).fileName();
    }
    if (base.isEmpty() || base == QLatin1String("/"))
        return path;
    // Common raw mount directory names look better with a little formatting.
    static const QRegularExpression hddPattern(
        QStringLiteral(R"(^hdd([0-9]+)(tb|gb)$)"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch hddMatch = hddPattern.match(base);
    if (hddMatch.hasMatch()) {
        return QStringLiteral("HDD %1 %2")
            .arg(hddMatch.captured(1), hddMatch.captured(2).toUpper());
    }
    if (base.size() > 2 && base.startsWith(QStringLiteral("wd"), Qt::CaseInsensitive)) {
        QString suffix = base.mid(2);
        if (!suffix.isEmpty() && suffix.at(0).isLower())
            suffix[0] = suffix.at(0).toUpper();
        return QStringLiteral("WD %1").arg(suffix);
    }
    // Make path-derived drive names feel like labels without mangling an
    // already-capitalised name such as "WDBlack" or "TeamGroup".
    if (!base.isEmpty() && base.at(0).isLower())
        base[0] = base.at(0).toUpper();
    return base;
}

QString readSysfsValue(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLatin1(file.readAll()).trimmed();
}

QString blockDeviceForQueue(const QString &basename)
{
    if (basename.isEmpty())
        return basename;
    if (readSysfsValue(QStringLiteral("/sys/class/block/%1/queue/rotational").arg(basename))
        .isEmpty()) {
        // Partitions normally expose the queue through their parent block device.
        int end = basename.size();
        while (end > 0 && basename.at(end - 1).isDigit())
            --end;
        if (end > 0 && end < basename.size())
            return basename.left(end);
    }
    return basename;
}

} // namespace

QString driveTypeLabel(const slm::MountInfo &mount)
{
    if (mount.fsClass == slm::FsClass::Network || mount.isNetwork())
        return QStringLiteral("Network");
    const QString source = mount.source;
    const QString base = QFileInfo(source).fileName();
    if (source.startsWith(QStringLiteral("/dev/mapper/")))
        return QStringLiteral("LVM/encrypted");
    if (base.startsWith(QStringLiteral("nvme"), Qt::CaseInsensitive))
        return QStringLiteral("NVMe SSD");
    if (base.startsWith(QStringLiteral("mmcblk"), Qt::CaseInsensitive))
        return QStringLiteral("SD/eMMC");
    if (base.startsWith(QStringLiteral("loop"), Qt::CaseInsensitive))
        return QStringLiteral("Loop device");
    if (base.startsWith(QStringLiteral("dm-"), Qt::CaseInsensitive))
        return QStringLiteral("LVM/encrypted");
    if (base.startsWith(QStringLiteral("md"), Qt::CaseInsensitive))
        return QStringLiteral("RAID volume");
    if (base.startsWith(QStringLiteral("zd"), Qt::CaseInsensitive))
        return QStringLiteral("ZFS volume");
    if (base.startsWith(QStringLiteral("vd"), Qt::CaseInsensitive)) {
        const QString block = blockDeviceForQueue(base);
        const QString rotational =
            readSysfsValue(QStringLiteral("/sys/class/block/%1/queue/rotational").arg(block));
        return rotational == QStringLiteral("1") ? QStringLiteral("Virtio HDD")
                                                  : QStringLiteral("Virtio SSD");
    }
    if (base.startsWith(QStringLiteral("sd"), Qt::CaseInsensitive)
        || base.startsWith(QStringLiteral("hd"), Qt::CaseInsensitive)) {
        const QString block = blockDeviceForQueue(base);
        const QString removable =
            readSysfsValue(QStringLiteral("/sys/class/block/%1/removable").arg(block));
        if (removable == QStringLiteral("1"))
            return QStringLiteral("USB drive");
        const QString rotational =
            readSysfsValue(QStringLiteral("/sys/class/block/%1/queue/rotational").arg(block));
        if (rotational == QStringLiteral("1"))
            return QStringLiteral("HDD");
        if (rotational == QStringLiteral("0"))
            return base.startsWith(QStringLiteral("hd"), Qt::CaseInsensitive)
                ? QStringLiteral("IDE disk") : QStringLiteral("SATA SSD");
        return QStringLiteral("SATA disk");
    }
    if (mount.fsClass == slm::FsClass::Other)
        return {};
    return {};
}

QString filesystemLabel(const slm::MountInfo &mount)
{
    const QString label = labelFromDeviceSource(mount.source);
    if (!label.isEmpty())
        return label;
    return {};
}

QString driveDisplayName(const slm::Library &library)
{
    if (!library.label.trimmed().isEmpty())
        return library.label.trimmed();

    const QString home = QDir::homePath();
    if (!home.isEmpty() && library.path.startsWith(home + QLatin1Char('/'))
        && !library.path.startsWith(QStringLiteral("/mnt/"))
        && !library.path.startsWith(QStringLiteral("/media/"))
        && !library.path.startsWith(QStringLiteral("/run/media/"))) {
        return QStringLiteral("Home");
    }

    const QString label = filesystemLabel(library.mount);
    if (!label.trimmed().isEmpty())
        return label.trimmed();

    return fallbackName(library.path);
}

QString driveSubtitle(const slm::Library &library, int gameCount)
{
    QStringList parts;
    QString driveType = driveTypeLabel(library.mount);
    driveType.replace(QStringLiteral("SATA SSD"), QStringLiteral("SSD"));
    driveType.replace(QStringLiteral("NVMe SSD"), QStringLiteral("NVMe"));
    driveType.replace(QStringLiteral("USB drive"), QStringLiteral("USB"));
    driveType.replace(QStringLiteral("Loop device"), QStringLiteral("Loop"));
    if (!driveType.isEmpty())
        parts << driveType;
    if (!library.mount.fsType.isEmpty())
        parts << library.mount.fsType.toUpper();
    if (library.mount.totalBytes > 0) {
        const int used = qBound(0, static_cast<int>(
            ((library.mount.totalBytes - library.mount.freeBytes) * 100)
            / library.mount.totalBytes), 100);
        parts << QStringLiteral("%1% used").arg(used);
    }
    parts << QStringLiteral("%1 free").arg(slm::formatBytes(library.mount.freeBytes));
    if (gameCount >= 0)
        parts << QStringLiteral("%1 games").arg(gameCount);
    if (library.mount.isWindowsCompatible())
        parts << QStringLiteral("Windows FS");
    return parts.join(QStringLiteral(" · "));
}

QString driveTooltip(const slm::Library &library)
{
    QStringList lines;
    lines << library.rawPath;
    lines << QStringLiteral("Drive name: %1").arg(driveDisplayName(library));
    if (!library.mount.source.isEmpty())
        lines << QStringLiteral("Device: %1").arg(library.mount.source);
    lines << QStringLiteral("Filesystem: %1 (%2)")
                 .arg(library.mount.fsType,
                      library.mount.readOnly ? QStringLiteral("read-only")
                                             : QStringLiteral("read-write"));
    const QString driveType = driveTypeLabel(library.mount);
    if (!driveType.isEmpty())
        lines << QStringLiteral("Drive type: %1").arg(driveType);
    lines << QStringLiteral("Space: %1 free of %2")
                 .arg(slm::formatBytes(library.mount.freeBytes),
                      slm::formatBytes(library.mount.totalBytes));
    if (library.mount.totalBytes > 0) {
        const quint64 usedBytes = library.mount.totalBytes - library.mount.freeBytes;
        const int used = qBound(0, static_cast<int>((usedBytes * 100) / library.mount.totalBytes),
                                100);
        lines << QStringLiteral("Used: %1 of %2 (%3%)")
                     .arg(slm::formatBytes(usedBytes),
                          slm::formatBytes(library.mount.totalBytes))
                     .arg(used);
        lines << QStringLiteral("Usage: %1%% (bar: green below 80%%, amber 80–94%%, red 95%%+)")
                     .arg(used);
    }
    lines << QStringLiteral("Mount options: %1").arg(library.mount.options.join(QStringLiteral(", ")));
    if (library.mount.isWindowsCompatible()) {
        lines << QStringLiteral("Windows FS: native Linux executables may lose their executable "
                                "bits; Proton games are usually fine.");
    }
    if (library.compatdata.state == slm::CompatDataState::Symlink)
        lines << QStringLiteral("compatdata → %1").arg(library.compatdata.target);
    else if (library.compatdata.state == slm::CompatDataState::Directory)
        lines << QStringLiteral("compatdata: real directory");
    else if (library.compatdata.state == slm::CompatDataState::BrokenSymlink)
        lines << QStringLiteral("compatdata: broken symlink");
    else
        lines << QStringLiteral("compatdata: missing");
    return lines.join(QLatin1Char('\n'));
}

namespace {

QIcon makeDriveIcon(const QString &kind)
{
    const QPalette palette = QApplication::palette();
    const QColor text = palette.color(QPalette::Text);
    const QColor base = palette.color(QPalette::Base);
    const QColor accent = palette.color(QPalette::Highlight);
    QColor outline = text;
    outline.setAlpha(210);
    QColor fill = base;
    fill.setAlpha(230);
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(outline, 1.8);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(fill);

    if (kind == QStringLiteral("network")) {
        painter.drawEllipse(QRectF(6, 6, 20, 20));
        painter.drawLine(QPointF(6, 13), QPointF(26, 13));
        painter.drawLine(QPointF(6, 20), QPointF(26, 20));
        painter.drawEllipse(QRectF(12, 6, 8, 20));
    } else if (kind == QStringLiteral("usb")) {
        painter.drawRoundedRect(QRectF(10, 12, 14, 13), 3, 3);
        painter.drawRect(QRectF(14, 8, 6, 5));
        painter.setBrush(accent);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(QPointF(20, 18), 1.7, 1.7);
    } else {
        painter.drawRoundedRect(QRectF(4, 9, 24, 15), 3.5, 3.5);
        QColor detail = outline;
        detail.setAlpha(150);
        painter.setPen(QPen(detail, 1.4));
        if (kind == QStringLiteral("ssd") || kind == QStringLiteral("nvme")) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            QPolygonF bolt;
            bolt << QPointF(17, 11) << QPointF(12, 18) << QPointF(16, 18)
                 << QPointF(14, 23) << QPointF(21, 15) << QPointF(17, 15);
            painter.drawPolygon(bolt);
            painter.setBrush(fill);
            painter.setPen(QPen(detail, 1.2));
            painter.drawLine(QPointF(7, 19), QPointF(10, 19));
        } else {
            painter.drawLine(QPointF(8, 14), QPointF(19, 14));
            painter.drawLine(QPointF(8, 18), QPointF(19, 18));
            painter.setBrush(accent);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QPointF(23.5, 19.5), 1.7, 1.7);
        }
    }
    painter.end();
    return QIcon(pixmap);
}

} // namespace

QIcon driveIcon(const slm::Library &library)
{
    const QString type = driveTypeLabel(library.mount);
    if (library.mount.isNetwork() || type.contains(QStringLiteral("Network")))
        return makeDriveIcon(QStringLiteral("network"));
    if (type.contains(QStringLiteral("USB")))
        return makeDriveIcon(QStringLiteral("usb"));
    if (type.contains(QStringLiteral("NVMe")))
        return makeDriveIcon(QStringLiteral("nvme"));
    if (type.contains(QStringLiteral("SSD")))
        return makeDriveIcon(QStringLiteral("ssd"));
    return makeDriveIcon(QStringLiteral("hdd"));
}

QColor driveTypeColor(const slm::Library &library)
{
    const QString type = driveTypeLabel(library.mount);
    if (library.mount.isNetwork() || type.contains(QStringLiteral("Network")))
        return QColor(QStringLiteral("#79b8ff"));
    if (type.contains(QStringLiteral("USB")))
        return QColor(QStringLiteral("#c79bff"));
    if (type.contains(QStringLiteral("NVMe")))
        return QColor(QStringLiteral("#5b9cff"));
    if (type.contains(QStringLiteral("SSD")))
        return QColor(QStringLiteral("#43b77a"));
    if (type.contains(QStringLiteral("HDD")))
        return QColor(QStringLiteral("#e1a63b"));
    QColor neutral = QApplication::palette().color(QPalette::Mid);
    return neutral;
}

QIcon allLibrariesIcon()
{
    const QPalette palette = QApplication::palette();
    QColor outline = palette.color(QPalette::Text);
    outline.setAlpha(210);
    QColor fill = palette.color(QPalette::Base);
    fill.setAlpha(230);
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(outline, 1.8);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(5, 10, 22, 15), 3, 3);
    painter.drawRoundedRect(QRectF(7, 7, 10, 5), 2, 2);
    painter.end();
    return QIcon(pixmap);
}

} // namespace ui
