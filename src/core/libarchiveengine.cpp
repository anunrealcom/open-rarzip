#include "iengine.h"
#include "pathguard.h"
#include "accelerator.h"

#include <archive.h>
#include <archive_entry.h>

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QSet>
#include <QSaveFile>
#include <QCoreApplication>
#include <QRegularExpression>

namespace yasuo {
namespace {

constexpr int kBufSize = 64 * 1024;

struct SourceItem {
    QString localPath;
    QString entryName;
    bool isDir = false;
    qint64 size = 0;
    QDateTime modified;
};

QString laError(struct archive* a) {
    const char* s = archive_error_string(a);
    return s ? QString::fromUtf8(s) : QStringLiteral("未知归档错误");
}

// Windows 下 CRT 的 fopen 使用系统 ANSI 代码页，直接传 UTF-8 窄字符串会打不开
// 中文/Unicode 路径（甚至生成乱码名的文件）。统一改用 libarchive 的宽字符入口。
int openReadFilename(struct archive* a, const QString& path, size_t blockSize) {
#ifdef Q_OS_WIN
    return archive_read_open_filename_w(
        a, reinterpret_cast<const wchar_t*>(path.utf16()), blockSize);
#else
    return archive_read_open_filename(a, path.toUtf8().constData(), blockSize);
#endif
}

int openWriteFilename(struct archive* a, const QString& path) {
#ifdef Q_OS_WIN
    return archive_write_open_filename_w(
        a, reinterpret_cast<const wchar_t*>(path.utf16()));
#else
    return archive_write_open_filename(a, path.toUtf8().constData());
#endif
}

// 本构建关闭了 iconv，ZIP 条目名的两个字段在不同来源的归档上互补：
//  - 本项目（libarchive）写出的归档：archive_entry_pathname() 即为正确的 UTF-8 名；
//  - 外部工具带 UTF-8 标志（bit11）的归档：pathname 可能为空，只有
//    archive_entry_pathname_utf8() 能拿到中文名。
// 旧代码只取 pathname，遇到后者会把条目当成空路径静默丢弃 —— list 里名字为空、
// extract 返回成功却少解压文件。因此优先 pathname，为空时回退 UTF-8 名称。
QString entryPathUtf8(struct archive_entry* e) {
    const char* local = archive_entry_pathname(e);
    if (local && *local) return QString::fromUtf8(local);
    const char* u8 = archive_entry_pathname_utf8(e);
    return QString::fromUtf8(u8 ? u8 : "");
}

QString entryLinkUtf8(struct archive_entry* e) {
    const char* local = archive_entry_symlink(e);
    if (local && *local) return QString::fromUtf8(local);
    const char* u8 = archive_entry_symlink_utf8(e);
    return QString::fromUtf8(u8 ? u8 : "");
}

bool excluded(const QString& name, const QStringList& patterns) {
    for (const QString& p : patterns) {
        if (p.isEmpty()) continue;
        if (QDir::match(p, name)) return true;
    }
    return false;
}

void collect(const QString& path, const QString& entryName, const QStringList& excludes,
             QVector<SourceItem>& out) {
    QFileInfo fi(path);
    if (excluded(fi.fileName(), excludes)) return;

    SourceItem item;
    item.localPath = path;
    item.entryName = entryName;
    item.modified = fi.lastModified();
    item.isDir = fi.isDir();
    item.size = fi.isDir() ? 0 : fi.size();
    out.append(item);

    if (!fi.isDir()) return;
    QDir d(path);
    const QFileInfoList children = d.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& c : children)
        collect(c.filePath(), entryName + QLatin1Char('/') + c.fileName(), excludes, out);
}

quint16 read16(const char* p) {
    return static_cast<quint16>(static_cast<uchar>(p[0]) |
                               (static_cast<quint16>(static_cast<uchar>(p[1])) << 8));
}
quint32 read32(const char* p) {
    return static_cast<quint32>(static_cast<uchar>(p[0])) |
           (static_cast<quint32>(static_cast<uchar>(p[1])) << 8) |
           (static_cast<quint32>(static_cast<uchar>(p[2])) << 16) |
           (static_cast<quint32>(static_cast<uchar>(p[3])) << 24);
}

// libarchive 读取 WinZip AES 加密的 ZIP 时，archive_entry_is_encrypted() 与
// archive_read_has_encrypted_entries() 都可能返回「未加密」，导致无密码解压
// 静默产出乱码。直接检查中央目录的 general purpose bit0 才可靠。
bool zipHasEncryptedEntry(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const qint64 size = f.size();
    if (size < 22) return false;
    const qint64 tailStart = qMax<qint64>(0, size - 65557);
    if (!f.seek(tailStart)) return false;
    const QByteArray tail = f.read(size - tailStart);
    qint64 eocd = -1;
    for (int i = tail.size() - 22; i >= 0; --i) {
        if (static_cast<uchar>(tail.at(i)) == 0x50 &&
            static_cast<uchar>(tail.at(i + 1)) == 0x4B &&
            static_cast<uchar>(tail.at(i + 2)) == 0x05 &&
            static_cast<uchar>(tail.at(i + 3)) == 0x06) { eocd = i; break; }
    }
    if (eocd < 0) return false;
    const char* p = tail.constData() + eocd;
    const quint16 count = read16(p + 10);
    const quint32 cdOff = read32(p + 16);
    if (count == 0xFFFF || cdOff == 0xFFFFFFFFu) return false;
    if (!f.seek(cdOff)) return false;
    for (int i = 0; i < count; ++i) {
        const QByteArray hdr = f.read(46);
        if (hdr.size() < 46 || read32(hdr.constData()) != 0x02014b50u) break;
        const char* c = hdr.constData();
        // 传统 ZipCrypto 用 bit0；WinZip AES 把 compression method 置为 99
        const quint16 flag = read16(c + 8);
        const quint16 method = read16(c + 10);
        if ((flag & 0x0001) || method == 99) return true;
        const int skip = read16(c + 28) + read16(c + 30) + read16(c + 32);
        if (skip > 0 && !f.seek(f.pos() + skip)) break;
    }
    return false;
}

bool isStreamFormat(Format f) {
    return f == Format::Gz || f == Format::Bz2 || f == Format::Xz || f == Format::Zst;
}

bool isWritable(Format f) {
    switch (f) {
    case Format::Zip: case Format::SevenZip: case Format::Tar:
    case Format::TarGz: case Format::TarBz2: case Format::TarXz: case Format::TarZst:
    case Format::Gz: case Format::Bz2: case Format::Xz: case Format::Zst:
        return true;
    default:
        return false;
    }
}

struct ReadSession {
    struct archive* a = nullptr;
    explicit ReadSession(const QString& path, const QString& password, QString& err) {
        a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        if (!password.isEmpty())
            archive_read_add_passphrase(a, password.toUtf8().constData());
        if (openReadFilename(a, path, 16 * 1024) != ARCHIVE_OK) {
            err = laError(a);
            archive_read_free(a);
            a = nullptr;
        }
    }
    ~ReadSession() { if (a) archive_read_free(a); }
    Q_DISABLE_COPY_MOVE(ReadSession)
};

class LibArchiveEngine final : public IArchiveEngine {
public:
    EngineCaps caps() const override {
        EngineCaps c;
        c.id = QStringLiteral("libarchive-3.9");
        c.displayName = QStringLiteral("libarchive 3.9 全格式引擎");
        c.writeFormats = { Format::Zip, Format::SevenZip, Format::Tar,
                           Format::TarGz, Format::TarBz2, Format::TarXz, Format::TarZst,
                           Format::Gz, Format::Bz2, Format::Xz, Format::Zst };
        c.readFormats = c.writeFormats;
        c.readFormats.append(Format::Rar);
        c.encryption = true;      // ZIP WinZip AES-128/256 + ZipCrypto（Windows CNG 后端）
        c.rarRead = true;         // RAR / RAR5 只读，符合 unRAR 许可
        c.encryptedRarRead = false;
        c.solid = false;          // 7z 固实写入需 7-Zip SDK，见路线图
        c.multiThread = true;     // zip:threads / zstd:threads 真并行编码
        c.zip64 = true;
        c.volumes = false;
        c.editable = true;       // ZIP / 7Z / TAR 系列支持包内删除（重写归档）
        c.notes = QStringLiteral("RAR 为专有格式，只能解压不能生成；加密 RAR 需 7-Zip SDK 后端");
        return c;
    }

    Format detect(const QString& archivePath) const override {
        QFile f(archivePath);
        if (!f.open(QIODevice::ReadOnly)) return Format::Unknown;
        const QByteArray h = f.read(512);
        f.close();
        if (h.startsWith("PK\x03\x04") || h.startsWith("PK\x05\x06")) return Format::Zip;
        if (h.startsWith("7z\xBC\xAF\x27\x1C")) return Format::SevenZip;
        if (h.startsWith("Rar!\x1A\x07\x00")) return Format::Rar;
        if (h.startsWith("Rar!\x1A\x07\x01\x00")) return Format::Rar;
        if (h.startsWith("\x1F\x8B")) return Format::Gz;
        if (h.startsWith("BZh")) return Format::Bz2;
        if (h.startsWith("\xFD" "7zXZ\x00")) return Format::Xz;
        if (h.startsWith("\x28\xB5\x2F\xFD")) return Format::Zst;
        if (h.size() > 262 && QByteArray(h.constData() + 257, 5) == "ustar") return Format::Tar;
        const Format bySuffix = formatFromSuffix(archivePath);
        return bySuffix == Format::Unknown ? Format::Unknown : bySuffix;
    }

    bool list(const QString& archivePath, const QString& password,
              ArchiveInfo& out, QString& err) override {
        ReadSession s(archivePath, password, err);
        if (!s.a) return false;

        out = ArchiveInfo{};
        out.path = archivePath;
        out.format = detect(archivePath);
        out.formatName = formatName(out.format);
        out.totalCompressed = QFileInfo(archivePath).size();

        struct archive_entry* e = nullptr;
        int r;
        while ((r = archive_read_next_header(s.a, &e)) == ARCHIVE_OK) {
            EntryInfo info;
            info.path = entryPathUtf8(e);
            info.path.replace(QLatin1Char('\\'), QLatin1Char('/'));
            info.size = qMax<qint64>(0, archive_entry_size(e));
            info.modified = QDateTime::fromSecsSinceEpoch(archive_entry_mtime(e));
            const mode_t ft = archive_entry_filetype(e);
            info.isDir = (ft == AE_IFDIR);
            info.isLink = (ft == AE_IFLNK);
            if (info.isLink)
                info.linkTarget = entryLinkUtf8(e);
            info.encrypted = archive_entry_is_encrypted(e) > 0;
            out.totalSize += info.size;
            out.entries.append(info);
            archive_read_data_skip(s.a);
        }
        if (r != ARCHIVE_EOF) {
            err = laError(s.a);
            return false;
        }
        out.entryCount = out.entries.size();
        const int hasEncrypted = archive_read_has_encrypted_entries(s.a);
        out.encrypted = hasEncrypted > 0 ||
                        (out.format == Format::Zip && zipHasEncryptedEntry(archivePath));
        if (out.encrypted && password.isEmpty() && !out.entries.isEmpty()) {
            // 允许列出文件名，数据读取会在 extract/test 时要求密码
            out.comment = QStringLiteral("归档含加密条目，解压需要提供密码");
        }
        return true;
    }

    bool compress(const CompressOptions& opt, ProgressFn progress,
                  CancelFn cancel, QString& err) override {
        if (opt.inputs.isEmpty()) {
            err = QStringLiteral("未选择任何文件或文件夹");
            return false;
        }
        if (!isWritable(opt.format)) {
            err = QStringLiteral("RAR 是专有格式，开源软件只能解压不能生成；请选择 7Z 或 ZIP");
            return false;
        }
        if (isStreamFormat(opt.format) && opt.inputs.size() > 1) {
            err = QStringLiteral("GZ/BZ2/XZ/ZST 单文件流格式只能压缩一个文件");
            return false;
        }
        if (!opt.password.isEmpty() && opt.format != Format::Zip) {
            err = QStringLiteral("当前引擎仅支持 ZIP 的 AES 加密；7z 加密请使用 7-Zip SDK 后端");
            return false;
        }
        if (opt.encryptNames || opt.solid || opt.volumeSize > 0) {
            err = QStringLiteral("加密文件名 / 固实压缩 / 分卷需要 7-Zip SDK 后端（当前未启用）");
            return false;
        }

        QVector<SourceItem> items;
        for (const QString& p : opt.inputs) {
            if (!QFileInfo::exists(p)) {
                err = QStringLiteral("路径不存在: %1").arg(p);
                return false;
            }
            collect(p, QFileInfo(p).fileName(), opt.excludes, items);
        }
        qint64 total = 0;
        for (const SourceItem& it : items) total += it.size;
        qint64 done = 0;

        // 先写临时文件，成功后再原子替换，避免取消/失败留下损坏归档
        const QString tempPath = opt.archivePath + QStringLiteral(".yasuo-part");
        QFile::remove(tempPath);
        if (!PathGuard::ensureParent(tempPath, err)) return false;

        struct archive* a = archive_write_new();
        if (archive_write_set_format_by_name(a, formatFilterName(opt.format).toUtf8().constData()) != ARCHIVE_OK) {
            err = laError(a); archive_write_free(a); return false;
        }
        // libarchive 没有名为 "none" 的过滤器：不加过滤器即为无压缩流
        const QString filterName = formatCompressionFilter(opt.format);
        if (filterName != QLatin1String("none") &&
            archive_write_add_filter_by_name(a, filterName.toUtf8().constData()) != ARCHIVE_OK) {
            err = laError(a); archive_write_free(a); return false;
        }

        // 逐项设置选项，而不是用 archive_write_set_options() 批量拼接：
        // libarchive 的批量接口一旦遇到任一「未定义选项」就整体返回 FAILED 并中止
        // 后续所有选项（见 archive_options.c:_archive_set_options）。旧代码多传了一个
        // 根本不存在的 "zip:utf8"，使排在它后面的 zip:encryption 从未生效 —— 标称
        // AES-256 的 ZIP 实际是明文包，无密码也能解压。TAR 的 hdrcharset 也被连坐失效。
        // 压缩级别/线程数是可选优化项，个别格式不支持时降级即可；加密是必须校验的关键项。
        const int threads = opt.threads > 0 ? opt.threads : Accelerator::idealThreads();
        const QByteArray level = QByteArray::number(qBound(0, opt.level, 9));
        const QByteArray threadCount = QByteArray::number(qMax(1, threads));
        (void)archive_write_set_option(a, nullptr, "compression-level", level.constData());
        if (opt.format == Format::Zip) {
            (void)archive_write_set_option(a, "zip", "threads", threadCount.constData());
        } else if (opt.format == Format::TarZst || opt.format == Format::Zst) {
            (void)archive_write_set_option(a, "zstd", "threads", threadCount.constData());
        }
        if (formatFilterName(opt.format) == QStringLiteral("pax"))
            (void)archive_write_set_option(a, nullptr, "hdrcharset", "UTF-8");

        if (!opt.password.isEmpty()) {
            const char* enc = "zipcrypt";
            switch (opt.encryption) {
            case Encryption::Aes256:   enc = "aes256"; break;
            case Encryption::Aes128:   enc = "aes128"; break;
            default:                   enc = "zipcrypt"; break;
            }
            if (archive_write_set_option(a, "zip", "encryption", enc) != ARCHIVE_OK) {
                err = QStringLiteral("无法启用 ZIP 加密（当前构建缺少 AES/ZipCrypto 后端）: %1")
                          .arg(laError(a));
                archive_write_free(a);
                return false;
            }
            if (archive_write_set_passphrase(a, opt.password.toUtf8().constData()) != ARCHIVE_OK) {
                err = QStringLiteral("设置压缩密码失败: %1").arg(laError(a));
                archive_write_free(a);
                return false;
            }
        }

        if (openWriteFilename(a, tempPath) != ARCHIVE_OK) {
            err = laError(a); archive_write_free(a); return false;
        }

        bool ok = true;
        for (const SourceItem& it : items) {
            if (cancel && cancel()) { ok = false; err = QStringLiteral("已取消"); break; }

            struct archive_entry* e = archive_entry_new();
            QString name = it.entryName;
            if (it.isDir && !name.endsWith(QLatin1Char('/'))) name += QLatin1Char('/');
            archive_entry_set_pathname(e, name.toUtf8().constData());
            archive_entry_set_filetype(e, it.isDir ? AE_IFDIR : AE_IFREG);
            archive_entry_set_perm(e, it.isDir ? 0755 : 0644);
            archive_entry_set_size(e, it.size);
            archive_entry_set_mtime(e, it.modified.toSecsSinceEpoch(), 0);
            if (archive_write_header(a, e) != ARCHIVE_OK) {
                err = laError(a); archive_entry_free(e); ok = false; break;
            }

            if (!it.isDir) {
                QFile f(it.localPath);
                if (!f.open(QIODevice::ReadOnly)) {
                    err = QStringLiteral("无法打开: %1").arg(it.localPath);
                    archive_entry_free(e); ok = false; break;
                }
                QByteArray buf(kBufSize, Qt::Uninitialized);
                qint64 n;
                bool readOk = true;
                while ((n = f.read(buf.data(), kBufSize)) > 0) {
                    if (cancel && cancel()) { ok = false; err = QStringLiteral("已取消"); readOk = false; break; }
                    if (archive_write_data(a, buf.constData(), static_cast<size_t>(n)) < 0) {
                        err = laError(a); ok = false; readOk = false; break;
                    }
                    done += n;
                    if (progress) progress(done, total, name);
                }
                f.close();
                archive_entry_free(e);
                if (!readOk || !ok) break;
                if (n < 0) { err = QStringLiteral("读取失败: %1").arg(it.localPath); ok = false; break; }
            } else {
                archive_entry_free(e);
                if (progress) progress(done, total, name);
            }
        }

        if (archive_write_close(a) != ARCHIVE_OK && ok) { err = laError(a); ok = false; }
        archive_write_free(a);

        if (!ok) { QFile::remove(tempPath); return false; }

        // 压缩后自检：重新打开并遍历全部数据
        if (opt.verifyAfter) {
            ArchiveInfo check;
            QString checkErr;
            if (!list(tempPath, opt.password, check, checkErr) || check.entryCount == 0) {
                err = QStringLiteral("压缩后校验失败: %1").arg(checkErr);
                QFile::remove(tempPath);
                return false;
            }
        }

        if (QFileInfo::exists(opt.archivePath)) QFile::remove(opt.archivePath);
        if (!QFile::rename(tempPath, opt.archivePath)) {
            err = QStringLiteral("无法写入目标文件: %1").arg(opt.archivePath);
            QFile::remove(tempPath);
            return false;
        }
        if (opt.deleteAfter) {
            for (const QString& in : opt.inputs) {
                QFileInfo fi(in);
                if (fi.isFile()) QFile::remove(in);
            }
        }
        return true;
    }

    bool extract(const ExtractOptions& opt, ProgressFn progress,
                 CancelFn cancel, QString& err) override {
        ArchiveInfo info;
        if (!list(opt.archivePath, opt.password, info, err)) return false;
        if (opt.password.isEmpty() && zipHasEncryptedEntry(opt.archivePath)) {
            err = QStringLiteral("归档含加密条目，请先输入密码");
            return false;
        }

        QSet<QString> selected;
        for (const QString& s : opt.selectedEntries) {
            QString n = s; n.replace(QLatin1Char('\\'), QLatin1Char('/'));
            selected.insert(n);
        }
        qint64 total = 0;
        for (const EntryInfo& e : info.entries)
            if (selected.isEmpty() || selected.contains(e.path) ||
                selected.contains(e.path + QLatin1Char('/')))
                total += e.size;

        ReadSession s(opt.archivePath, opt.password, err);
        if (!s.a) return false;
        QDir().mkpath(opt.destDir);

        qint64 done = 0;
        bool ok = true;
        int skippedUnsafe = 0;
        struct archive_entry* e = nullptr;
        int r;
        while ((r = archive_read_next_header(s.a, &e)) == ARCHIVE_OK) {
            if (cancel && cancel()) { ok = false; err = QStringLiteral("已取消"); break; }
            QString name = entryPathUtf8(e);
            name.replace(QLatin1Char('\\'), QLatin1Char('/'));
            // 宁可整体失败，也不能静默跳过一个解不出名字的条目（会丢用户文件）
            if (name.isEmpty()) {
                err = QStringLiteral("归档含无法解析名称的条目（可能是文件名编码不受支持），已中止以免丢失文件");
                ok = false;
                break;
            }
            if (!selected.isEmpty() && !selected.contains(name)) {
                archive_read_data_skip(s.a);
                continue;
            }

            if (archive_entry_is_encrypted(e) > 0 && opt.password.isEmpty()) {
                err = QStringLiteral("归档含加密条目，请先输入密码");
                ok = false;
                break;
            }

            QString outPath;
            QString why;
            if (!PathGuard::resolve(opt.destDir, name, outPath, why)) {
                ++skippedUnsafe;
                archive_read_data_skip(s.a);
                continue;
            }

            const mode_t ft = archive_entry_filetype(e);
            if (ft == AE_IFDIR || name.endsWith(QLatin1Char('/'))) {
                QDir().mkpath(outPath);
                continue;
            }
            if (ft != AE_IFREG) { // 符号链接/设备文件：安全起见不还原
                archive_read_data_skip(s.a);
                continue;
            }

            const time_t mtime = archive_entry_mtime(e);
            const QDateTime entryTime = mtime > 0
                ? QDateTime::fromSecsSinceEpoch(mtime) : QDateTime();
            bool skip = false;
            const QString target = PathGuard::decide(outPath, opt.overwrite, skip, entryTime);
            if (skip) { archive_read_data_skip(s.a); continue; }
            if (!PathGuard::ensureParent(target, err)) { ok = false; break; }

            QFile out(target);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                err = QStringLiteral("无法写入: %1").arg(target);
                ok = false; archive_read_data_skip(s.a); break;
            }
            QByteArray buf(kBufSize, Qt::Uninitialized);
            la_ssize_t n;
            while ((n = archive_read_data(s.a, buf.data(), kBufSize)) > 0) {
                if (cancel && cancel()) { ok = false; err = QStringLiteral("已取消"); break; }
                if (out.write(buf.constData(), static_cast<qint64>(n)) != static_cast<qint64>(n)) {
                    err = QStringLiteral("写入失败: %1").arg(target);
                    ok = false; break;
                }
                done += static_cast<qint64>(n);
                if (progress) progress(done, total, name);
            }
            out.close();
            if (!ok) break;
            if (n < 0) { err = laError(s.a); ok = false; break; }

            if (mtime > 0) {
                QFile tf(target);
                tf.setFileTime(entryTime, QFileDevice::FileModificationTime);
            }
        }
        if (r != ARCHIVE_EOF && ok) { err = laError(s.a); ok = false; }
        if (ok && skippedUnsafe > 0 && total == 0 && info.entryCount > 0)
            err = QStringLiteral("所有条目均因不安全路径被跳过");
        return ok;
    }

    bool test(const QString& archivePath, const QString& password,
              ProgressFn progress, CancelFn cancel, QString& err) override {
        ArchiveInfo info;
        if (!list(archivePath, password, info, err)) return false;
        if ((info.encrypted || zipHasEncryptedEntry(archivePath)) && password.isEmpty()) {
            err = QStringLiteral("归档含加密条目，请输入密码后再测试");
            return false;
        }
        ReadSession s(archivePath, password, err);
        if (!s.a) return false;

        qint64 total = info.totalSize, done = 0;
        struct archive_entry* e = nullptr;
        QByteArray buf(kBufSize, Qt::Uninitialized);
        int r;
        while ((r = archive_read_next_header(s.a, &e)) == ARCHIVE_OK) {
            if (cancel && cancel()) { err = QStringLiteral("已取消"); return false; }
            if (archive_entry_is_encrypted(e) > 0 && password.isEmpty()) {
                err = QStringLiteral("归档含加密条目，请输入密码后再测试");
                return false;
            }
            const QString name = entryPathUtf8(e);
            la_ssize_t n;
            while ((n = archive_read_data(s.a, buf.data(), kBufSize)) > 0) {
                if (cancel && cancel()) { err = QStringLiteral("已取消"); return false; }
                done += static_cast<qint64>(n);
                if (progress) progress(done, total, name);
            }
            if (n < 0) { err = laError(s.a); return false; }
        }
        if (r != ARCHIVE_EOF) { err = laError(s.a); return false; }
        return true;
    }

    bool removeEntries(const QString& archivePath, const QStringList& names,
                       const QString& password, QString& err) override {
        if (names.isEmpty()) { err = QStringLiteral("未选择要删除的条目"); return false; }
        if (!QFileInfo::exists(archivePath)) {
            err = QStringLiteral("归档不存在: %1").arg(archivePath);
            return false;
        }
        const Format fmt = detect(archivePath);
        switch (fmt) {
        case Format::Zip: case Format::SevenZip: case Format::Tar:
        case Format::TarGz: case Format::TarBz2: case Format::TarXz: case Format::TarZst:
            break;
        default:
            err = QStringLiteral("该格式不支持包内删除（仅 ZIP / 7Z / TAR 系列；RAR 为只读）");
            return false;
        }

        ArchiveInfo info;
        if (!list(archivePath, password, info, err)) return false;
        if (info.encrypted) {
            // 重写会丢掉原有加密方式/密钥派生参数，静默降级为明文是严重安全问题。
            err = QStringLiteral("加密归档暂不支持包内删除（避免重写后丢失加密设置）");
            return false;
        }

        QStringList targets;
        for (const QString& raw : names) {
            QString s = raw.trimmed();
            s.replace(QLatin1Char('\\'), QLatin1Char('/'));
            while (s.startsWith(QStringLiteral("./"))) s.remove(0, 2);
            while (s.endsWith(QLatin1Char('/'))) s.chop(1);
            if (!s.isEmpty() && !targets.contains(s)) targets.append(s);
        }
        if (targets.isEmpty()) { err = QStringLiteral("条目名无效"); return false; }

        const auto matches = [&targets](const QString& path) {
            QString p = path;
            p.replace(QLatin1Char('\\'), QLatin1Char('/'));
            while (p.startsWith(QStringLiteral("./"))) p.remove(0, 2);
            while (p.endsWith(QLatin1Char('/'))) p.chop(1);
            for (const QString& t : targets)
                if (p == t || p.startsWith(t + QLatin1Char('/'))) return true;
            return false;
        };
        int existing = 0;
        for (const EntryInfo& e : info.entries)
            if (matches(e.path)) ++existing;
        if (existing == 0) { err = QStringLiteral("归档中未找到要删除的条目"); return false; }

        const QString temp = archivePath + QStringLiteral(".yasuo-edit");
        QFile::remove(temp);
        bool ok = true;
        int removed = 0;
        {
            struct archive* out = archive_write_new();
            if (archive_write_set_format_by_name(
                    out, formatFilterName(fmt).toUtf8().constData()) != ARCHIVE_OK) {
                err = laError(out); archive_write_free(out); return false;
            }
            const QString filter = formatCompressionFilter(fmt);
            if (filter != QLatin1String("none") &&
                archive_write_add_filter_by_name(out, filter.toUtf8().constData()) != ARCHIVE_OK) {
                err = laError(out); archive_write_free(out); return false;
            }
            (void)archive_write_set_option(out, nullptr, "compression-level", "6");
            if (formatFilterName(fmt) == QStringLiteral("pax"))
                (void)archive_write_set_option(out, nullptr, "hdrcharset", "UTF-8");
            if (openWriteFilename(out, temp) != ARCHIVE_OK) {
                err = laError(out); archive_write_free(out);
                QFile::remove(temp); return false;
            }

            ReadSession in(archivePath, password, err);
            if (!in.a) {
                archive_write_free(out); QFile::remove(temp); return false;
            }
            struct archive_entry* e = nullptr;
            int r;
            while ((r = archive_read_next_header(in.a, &e)) == ARCHIVE_OK) {
                const QString name = QString::fromUtf8(
                    archive_entry_pathname(e) ? archive_entry_pathname(e) : "");
                if (matches(name)) { ++removed; archive_read_data_skip(in.a); continue; }
                if (archive_write_header(out, e) != ARCHIVE_OK) {
                    err = laError(out); ok = false; break;
                }
                QByteArray buf(kBufSize, Qt::Uninitialized);
                la_ssize_t n;
                while ((n = archive_read_data(in.a, buf.data(), kBufSize)) > 0) {
                    if (archive_write_data(out, buf.constData(), static_cast<size_t>(n)) < 0) {
                        err = laError(out); ok = false; break;
                    }
                }
                if (!ok) break;
                if (n < 0) { err = laError(in.a); ok = false; break; }
            }
            if (r != ARCHIVE_EOF && ok) { err = laError(in.a); ok = false; }
            if (archive_write_close(out) != ARCHIVE_OK && ok) { err = laError(out); ok = false; }
            archive_write_free(out);
        } // 读写句柄全部释放后才能在 Windows 上安全替换原文件

        if (!ok || removed == 0) {
            QFile::remove(temp);
            if (err.isEmpty()) err = QStringLiteral("包内删除失败");
            return false;
        }

        ArchiveInfo check;
        QString checkErr;
        if (!list(temp, password, check, checkErr)) {
            err = QStringLiteral("删除后校验失败: %1").arg(checkErr);
            QFile::remove(temp);
            return false;
        }
        for (const EntryInfo& e : check.entries) {
            if (matches(e.path)) {
                err = QStringLiteral("删除后校验失败：条目仍存在 %1").arg(e.path);
                QFile::remove(temp);
                return false;
            }
        }

        const QString backup = archivePath + QStringLiteral(".yasuo-bak");
        QFile::remove(backup);
        if (!QFile::rename(archivePath, backup)) {
            err = QStringLiteral("无法备份原归档（文件可能被占用）");
            QFile::remove(temp);
            return false;
        }
        if (!QFile::rename(temp, archivePath)) {
            QFile::rename(backup, archivePath);
            err = QStringLiteral("无法写回归档，原文件已恢复");
            QFile::remove(temp);
            return false;
        }
        QFile::remove(backup);
        return true;
    }
};

} // namespace

#ifdef YASUO_HAVE_LIBARCHIVE
IArchiveEngine* createLibArchiveEngine() { static LibArchiveEngine e; return &e; }
#endif

} // namespace yasuo
