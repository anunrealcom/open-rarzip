#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QDateTime>
#include <functional>
#include <cstdint>

namespace yasuo {

enum class Format {
    Auto, Zip, SevenZip, Tar, TarGz, TarBz2, TarXz, TarZst,
    Gz, Bz2, Xz, Zst, Rar, Unknown
};

enum class Encryption { None, ZipCrypto, Aes128, Aes256 };

enum class OverwritePolicy { Ask, Overwrite, Skip, Rename, OverwriteIfNewer };

enum class JobKind { Compress, Extract, Test, Browse, Remove };

enum class JobState { Queued, Running, Done, Failed, Cancelled };

struct EntryInfo {
    QString path;              // 归档内路径，统一为 / 分隔
    qint64 size = 0;           // 原始大小
    qint64 compressedSize = 0; // 压缩后大小
    QDateTime modified;
    bool isDir = false;
    bool isLink = false;
    bool encrypted = false;
    quint32 crc = 0;
    QString linkTarget;
    QString hostPath;          // 压缩时对应的本地源路径（仅内存中使用）
};

struct ArchiveInfo {
    QString path;
    Format format = Format::Unknown;
    QString formatName;
    QString comment;
    qint64 totalSize = 0;
    qint64 totalCompressed = 0;
    int entryCount = 0;
    bool encrypted = false;
    bool multiVolume = false;
    QVector<EntryInfo> entries;
};

struct CompressOptions {
    QString archivePath;
    QStringList inputs;
    Format format = Format::Zip;
    int level = 6;                    // 0..9
    QString password;
    Encryption encryption = Encryption::None;
    bool encryptNames = false;        // 7z 特性；当前引擎不支持时会明确提示
    bool solid = false;
    int dictionaryMB = 32;
    int threads = 0;                  // 0 = 自动
    qint64 volumeSize = 0;            // 0 = 不分卷
    QString comment;
    QStringList excludes;             // glob 风格片段，如 "*.tmp"
    bool followSymlinks = false;
    bool deleteAfter = false;
    bool verifyAfter = false;
};

struct ExtractOptions {
    QString archivePath;
    QString destDir;
    QString password;
    OverwritePolicy overwrite = OverwritePolicy::Ask;
    QStringList selectedEntries;       // 空 = 全部
    bool preservePaths = true;
    bool openAfter = false;
    bool verifyCrc = true;
};

struct EngineCaps {
    QString id;
    QString displayName;
    QVector<Format> writeFormats;
    QVector<Format> readFormats;
    bool encryption = false;           // ZIP AES / ZipCrypto 写入
    bool rarRead = false;
    bool encryptedRarRead = false;
    bool solid = false;
    bool multiThread = false;
    bool zip64 = false;
    bool volumes = false;
    bool editable = false;         // 是否支持包内删除（重写归档）
    QString notes;
};

using ProgressFn = std::function<void(qint64 done, qint64 total, const QString& current)>;
using CancelFn   = std::function<bool()>;
using LogFn      = std::function<void(const QString&)>;

QString formatName(Format f);
QString formatSuffix(Format f);
QString formatFilterName(Format f);        // libarchive 的 archive_write_set_format_by_name 参数
QString formatCompressionFilter(Format f); // libarchive 的 filter 名
Format  formatFromSuffix(const QString& path);
QString encryptionName(Encryption e);
QString humanBytes(qint64 bytes);

} // namespace yasuo
