#include "pathguard.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSet>
#include <QDateTime>

namespace yasuo {
namespace PathGuard {

static bool isReservedName(const QString& p) {
    static const QSet<QString> reserved = {
        QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"),
        QStringLiteral("NUL"), QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")
    };
    QString base = p;
    const int dot = base.indexOf('.');
    if (dot >= 0) base = base.left(dot);
    return reserved.contains(base.toUpper());
}

bool sanitize(const QString& entry, QString& normalized, QString& reason) {
    normalized.clear();
    if (entry.trimmed().isEmpty()) { reason = QStringLiteral("条目名为空"); return false; }

    QString s = entry;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));

    // 控制字符与 NUL 是路径穿越/欺骗的常见载体
    for (int i = 0; i < s.size(); ++i) {
        const ushort c = s.at(i).unicode();
        if (c < 0x20 || c == 0x7F) {
            reason = QStringLiteral("条目名含控制字符");
            return false;
        }
    }

    // 绝对路径、Windows 盘符、UNC 一律拒绝，不做剥离后落盘
    if (s.startsWith(QLatin1Char('/')) || s.startsWith(QStringLiteral("//")) ||
        (s.size() >= 2 && s.at(1) == QLatin1Char(':'))) {
        reason = QStringLiteral("拒绝绝对路径 / 盘符路径");
        return false;
    }

    // 去掉 "./" 前缀，但严格拒绝任何 ".."（旧实现会静默剥离，可导致越界写入）
    while (s.startsWith(QStringLiteral("./"))) s.remove(0, 2);

    const QStringList parts = s.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList clean;
    for (const QString& part : parts) {
        if (part == QStringLiteral(".")) continue;
        if (part == QStringLiteral("..")) {
            reason = QStringLiteral("拒绝含 .. 的越界路径");
            return false;
        }
        QString seg = part;
        // Windows 不允许结尾空格/点，且文件名不能含这些字符
        while (seg.endsWith(QLatin1Char(' ')) || seg.endsWith(QLatin1Char('.'))) seg.chop(1);
        if (seg.isEmpty()) { reason = QStringLiteral("路径段为空"); return false; }
        if (seg.contains(QLatin1Char(':')) || seg.contains(QLatin1Char('*')) ||
            seg.contains(QLatin1Char('?')) || seg.contains(QLatin1Char('"')) ||
            seg.contains(QLatin1Char('<')) || seg.contains(QLatin1Char('>')) ||
            seg.contains(QLatin1Char('|'))) {
            reason = QStringLiteral("路径段含非法字符");
            return false;
        }
        if (isReservedName(seg)) { reason = QStringLiteral("Windows 保留设备名"); return false; }
        clean.append(seg);
    }
    if (clean.isEmpty()) { reason = QStringLiteral("路径为空"); return false; }

    normalized = clean.join(QLatin1Char('/'));
    if (entry.endsWith(QLatin1Char('/')) || entry.endsWith(QLatin1Char('\\')))
        normalized += QLatin1Char('/');
    return true;
}

bool resolve(const QString& destDir, const QString& entry, QString& out, QString& reason) {
    QString rel;
    if (!sanitize(entry, rel, reason)) return false;

    QDir dest(destDir);
    const QString absDest = QDir::cleanPath(dest.absolutePath());
    QString candidate = QDir::cleanPath(dest.absoluteFilePath(rel));

    // 双重校验：前缀比较必须带分隔符，且相对路径不能越出根目录。
    // 旧实现用 startsWith(absDest)，"../output/x" 会被误判为落在 /tmp/out 内。
    const bool prefixOk = (candidate == absDest) ||
                          candidate.startsWith(absDest + QLatin1Char('/'));
    const QString relative = QDir(absDest).relativeFilePath(candidate);
    const bool relativeOk = !relative.startsWith(QStringLiteral("..")) &&
                            !QDir::isAbsolutePath(relative);
    if (!prefixOk || !relativeOk) {
        reason = QStringLiteral("目标路径越出解压目录");
        return false;
    }
    out = candidate;
    return true;
}

QString decide(const QString& desiredPath, OverwritePolicy policy, bool& skip,
               const QDateTime& entryTime) {
    skip = false;
    QFileInfo fi(desiredPath);
    if (!fi.exists()) return desiredPath;

    switch (policy) {
    case OverwritePolicy::Overwrite:
        return desiredPath;
    case OverwritePolicy::Skip:
        skip = true;
        return desiredPath;
    case OverwritePolicy::OverwriteIfNewer:
        // 只有归档条目确实比本地文件新才覆盖；无时间信息时保守跳过。
        skip = !entryTime.isValid() || entryTime <= fi.lastModified();
        return desiredPath;
    case OverwritePolicy::Rename: {
        const QString base = fi.completeBaseName();
        const QString suffix = fi.suffix();
        const QString dir = fi.absolutePath();
        for (int i = 1; i < 10000; ++i) {
            QString candidate = dir + QLatin1Char('/') + base +
                                QStringLiteral(" (%1)").arg(i);
            if (!suffix.isEmpty()) candidate += QLatin1Char('.') + suffix;
            if (!QFileInfo::exists(candidate)) return candidate;
        }
        skip = true;
        return desiredPath;
    }
    case OverwritePolicy::Ask:
    default:
        // 交互策略由 UI 在任务开始前统一询问并转成具体策略
        return desiredPath;
    }
}

bool ensureParent(const QString& filePath, QString& reason) {
    const QString parent = QFileInfo(filePath).absolutePath();
    QDir d;
    if (d.exists(parent)) return true;
    if (!d.mkpath(parent)) {
        reason = QStringLiteral("无法创建目录: %1").arg(parent);
        return false;
    }
    return true;
}

} // namespace PathGuard
} // namespace yasuo
