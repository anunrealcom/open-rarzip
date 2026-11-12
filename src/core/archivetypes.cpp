#include "archivetypes.h"

#include <QFileInfo>
#include <cmath>

namespace yasuo {

QString formatName(Format f) {
    switch (f) {
    case Format::Zip:     return QStringLiteral("ZIP");
    case Format::SevenZip:return QStringLiteral("7Z");
    case Format::Tar:     return QStringLiteral("TAR");
    case Format::TarGz:   return QStringLiteral("TAR.GZ");
    case Format::TarBz2:  return QStringLiteral("TAR.BZ2");
    case Format::TarXz:   return QStringLiteral("TAR.XZ");
    case Format::TarZst:  return QStringLiteral("TAR.ZST");
    case Format::Gz:      return QStringLiteral("GZ");
    case Format::Bz2:     return QStringLiteral("BZ2");
    case Format::Xz:      return QStringLiteral("XZ");
    case Format::Zst:     return QStringLiteral("ZST");
    case Format::Rar:     return QStringLiteral("RAR");
    case Format::Auto:    return QStringLiteral("自动");
    default:              return QStringLiteral("未知");
    }
}

QString formatSuffix(Format f) {
    switch (f) {
    case Format::Zip:     return QStringLiteral("zip");
    case Format::SevenZip:return QStringLiteral("7z");
    case Format::Tar:     return QStringLiteral("tar");
    case Format::TarGz:   return QStringLiteral("tar.gz");
    case Format::TarBz2:  return QStringLiteral("tar.bz2");
    case Format::TarXz:   return QStringLiteral("tar.xz");
    case Format::TarZst:  return QStringLiteral("tar.zst");
    case Format::Gz:      return QStringLiteral("gz");
    case Format::Bz2:     return QStringLiteral("bz2");
    case Format::Xz:      return QStringLiteral("xz");
    case Format::Zst:     return QStringLiteral("zst");
    default:              return QStringLiteral("zip");
    }
}

QString formatFilterName(Format f) {
    switch (f) {
    case Format::Zip:      return QStringLiteral("zip");
    case Format::SevenZip: return QStringLiteral("7zip");
    case Format::Tar: case Format::TarGz: case Format::TarBz2:
    case Format::TarXz: case Format::TarZst: return QStringLiteral("pax");
    case Format::Gz: case Format::Bz2: case Format::Xz: case Format::Zst:
        return QStringLiteral("raw");
    default: return QStringLiteral("zip");
    }
}

QString formatCompressionFilter(Format f) {
    switch (f) {
    case Format::TarGz: case Format::Gz:   return QStringLiteral("gzip");
    case Format::TarBz2: case Format::Bz2: return QStringLiteral("bzip2");
    case Format::TarXz: case Format::Xz:   return QStringLiteral("xz");
    case Format::TarZst: case Format::Zst: return QStringLiteral("zstd");
    default: return QStringLiteral("none");
    }
}

Format formatFromSuffix(const QString& path) {
    const QString s = QFileInfo(path).fileName().toLower();
    if (s.endsWith(QStringLiteral(".tar.gz")) || s.endsWith(QStringLiteral(".tgz"))) return Format::TarGz;
    if (s.endsWith(QStringLiteral(".tar.bz2")) || s.endsWith(QStringLiteral(".tbz2"))) return Format::TarBz2;
    if (s.endsWith(QStringLiteral(".tar.xz")) || s.endsWith(QStringLiteral(".txz"))) return Format::TarXz;
    if (s.endsWith(QStringLiteral(".tar.zst")) || s.endsWith(QStringLiteral(".tzst"))) return Format::TarZst;
    if (s.endsWith(QStringLiteral(".zip")))    return Format::Zip;
    if (s.endsWith(QStringLiteral(".7z")))     return Format::SevenZip;
    if (s.endsWith(QStringLiteral(".tar")))    return Format::Tar;
    if (s.endsWith(QStringLiteral(".rar")))    return Format::Rar;
    if (s.endsWith(QStringLiteral(".gz")))     return Format::Gz;
    if (s.endsWith(QStringLiteral(".bz2")))    return Format::Bz2;
    if (s.endsWith(QStringLiteral(".xz")))     return Format::Xz;
    if (s.endsWith(QStringLiteral(".zst")))    return Format::Zst;
    return Format::Unknown;
}

Format formatFromName(const QString& name) {
    const QString n = name.trimmed().toLower();
    if (n.isEmpty()) return Format::Auto;
    if (n == QLatin1String("zip")) return Format::Zip;
    if (n == QLatin1String("7z") || n == QLatin1String("sevenzip") || n == QLatin1String("7zip"))
        return Format::SevenZip;
    if (n == QLatin1String("tar")) return Format::Tar;
    if (n == QLatin1String("tar.gz") || n == QLatin1String("tgz")) return Format::TarGz;
    if (n == QLatin1String("tar.bz2") || n == QLatin1String("tbz2")) return Format::TarBz2;
    if (n == QLatin1String("tar.xz") || n == QLatin1String("txz")) return Format::TarXz;
    if (n == QLatin1String("tar.zst") || n == QLatin1String("tzst")) return Format::TarZst;
    if (n == QLatin1String("gz") || n == QLatin1String("gzip")) return Format::Gz;
    if (n == QLatin1String("bz2") || n == QLatin1String("bzip2")) return Format::Bz2;
    if (n == QLatin1String("xz")) return Format::Xz;
    if (n == QLatin1String("zst") || n == QLatin1String("zstd")) return Format::Zst;
    if (n == QLatin1String("rar")) return Format::Rar;
    if (n == QLatin1String("auto")) return Format::Auto;
    return Format::Unknown;
}

Encryption encryptionFromName(const QString& name) {
    const QString n = name.trimmed().toLower();
    if (n == QLatin1String("aes256") || n == QLatin1String("aes-256")) return Encryption::Aes256;
    if (n == QLatin1String("aes128") || n == QLatin1String("aes-128")) return Encryption::Aes128;
    if (n == QLatin1String("zipcrypto") || n == QLatin1String("zipcrypt") || n == QLatin1String("legacy"))
        return Encryption::ZipCrypto;
    if (n.isEmpty() || n == QLatin1String("none")) return Encryption::None;
    return Encryption::None;
}

QString encryptionName(Encryption e) {
    switch (e) {
    case Encryption::ZipCrypto: return QStringLiteral("ZipCrypto（兼容旧软件，强度低）");
    case Encryption::Aes128:    return QStringLiteral("WinZip AES-128");
    case Encryption::Aes256:    return QStringLiteral("WinZip AES-256");
    default:                    return QStringLiteral("不加密");
    }
}

QString humanBytes(qint64 bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    return QString::number(v, 'f', v >= 100 ? 0 : 1) + QLatin1Char(' ') + QLatin1String(units[i]);
}

} // namespace yasuo
