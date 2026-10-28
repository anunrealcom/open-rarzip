#include "archiveengine.h"

#include <archive.h>
#include <archive_entry.h>

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QVector>
#include <QCoreApplication>

namespace {

// 计算路径相对于基准目录的条目名（用于写入归档时保持目录结构）
QString relativeEntryName(const QString& inputPath) {
    QFileInfo fi(inputPath);
    if (fi.isDir())
        return fi.fileName();          // 目录本身作为根目录名
    return fi.fileName();              // 单文件直接使用文件名
}

// 把归档内条目名映射到安全的本地目标路径，防止路径穿越攻击（../ 或绝对路径）
QString safeDestPath(const QString& destDir, const QString& entryName) {
    QString rel = entryName;
    rel.replace('\\', '/');
    while (rel.startsWith("./")) rel = rel.mid(2);
    while (rel.startsWith('/'))  rel = rel.mid(1);

    QStringList parts = rel.split('/', Qt::SkipEmptyParts);
    QStringList clean;
    for (const QString& p : parts) {
        if (p == ".") continue;
        if (p == "..") { if (!clean.isEmpty()) clean.removeLast(); continue; }
        clean.append(p);
    }
    QString joined = clean.join('/');
    QDir dest(destDir);
    QString out = QDir::cleanPath(dest.absoluteFilePath(joined));
    // 二次校验：结果必须落在 destDir 之内
    QString absDest = QDir::cleanPath(dest.absolutePath());
    if (!out.startsWith(absDest))
        return QString();
    return out;
}

bool addFileEntry(struct archive* a, const QString& filePath, const QString& entryName,
                  qint64& done, qint64 total, ProgressFn progress, CancelFn cancel, QString& err) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        err = QCoreApplication::translate("ArchiveEngine", "无法打开文件: %1").arg(filePath);
        return false;
    }
    QFileInfo fi(filePath);
    struct archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, entryName.toUtf8().constData());
    archive_entry_set_size(entry, fi.size());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_mtime(entry, fi.lastModified().toSecsSinceEpoch(), 0);

    if (archive_write_header(a, entry) != ARCHIVE_OK) {
        err = QString::fromUtf8(archive_error_string(a));
        archive_entry_free(entry);
        return false;
    }

    const int bufSize = 65536;
    QByteArray buf(bufSize, 0);
    qint64 read;
    while ((read = f.read(buf.data(), bufSize)) > 0) {
        if (cancel && cancel()) {
            archive_entry_free(entry);
            return false;
        }
        ssize_t wrote = archive_write_data(a, buf.constData(), (size_t)read);
        if (wrote < 0) {
            err = QString::fromUtf8(archive_error_string(a));
            archive_entry_free(entry);
            return false;
        }
        done += read;
        if (progress) progress(done, total, entryName);
    }
    archive_entry_free(entry);
    f.close();
    return true;
}

bool addDirEntry(struct archive* a, const QString& dirPath, const QString& entryName,
                 qint64& done, qint64 total, ProgressFn progress, CancelFn cancel, QString& err) {
    QString dirEntry = entryName;
    if (!dirEntry.endsWith('/')) dirEntry += '/';
    struct archive_entry* entry = archive_entry_new();
    archive_entry_set_pathname(entry, dirEntry.toUtf8().constData());
    archive_entry_set_filetype(entry, AE_IFDIR);
    archive_entry_set_perm(entry, 0755);
    archive_entry_set_size(entry, 0);
    if (archive_write_header(a, entry) != ARCHIVE_OK) {
        err = QString::fromUtf8(archive_error_string(a));
        archive_entry_free(entry);
        return false;
    }
    archive_entry_free(entry);
    if (progress) progress(done, total, dirEntry);

    QDir dir(dirPath);
    QFileInfoList list = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const QFileInfo& child : list) {
        if (cancel && cancel()) return false;
        QString childPath = child.filePath();
        QString childEntry = entryName + "/" + child.fileName();
        if (child.isDir()) {
            if (!addDirEntry(a, childPath, childEntry, done, total, progress, cancel, err))
                return false;
        } else {
            if (!addFileEntry(a, childPath, childEntry, done, total, progress, cancel, err))
                return false;
        }
    }
    return true;
}

qint64 totalInputBytes(const QStringList& inputs) {
    qint64 sum = 0;
    for (const QString& p : inputs) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            QDir dir(p);
            QFileInfoList list = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
            QStringList sub;
            for (const QFileInfo& c : list) sub.append(c.filePath());
            sum += totalInputBytes(sub);
        } else if (fi.exists()) {
            sum += fi.size();
        }
    }
    return sum;
}

} // namespace

namespace ArchiveEngine {

bool compress(const CompressOptions& opt, ProgressFn progress, CancelFn cancel, QString& err) {
    if (opt.inputs.isEmpty()) {
        err = QCoreApplication::translate("ArchiveEngine", "未选择任何文件或文件夹");
        return false;
    }
    // raw 格式（单文件流）仅支持单个输入
    if (opt.format == "raw" && opt.inputs.size() > 1) {
        err = QCoreApplication::translate("ArchiveEngine",
            "GZ/BZ2/XZ/ZST 等单文件流格式仅支持压缩一个文件");
        return false;
    }

    struct archive* a = archive_write_new();
    if (archive_write_set_format_by_name(a, opt.format.toUtf8().constData()) != ARCHIVE_OK) {
        err = QCoreApplication::translate("ArchiveEngine", "不支持的归档格式: %1").arg(opt.format);
        archive_write_free(a);
        return false;
    }
    if (archive_write_add_filter_by_name(a, opt.filter.toUtf8().constData()) != ARCHIVE_OK) {
        err = QCoreApplication::translate("ArchiveEngine", "不支持的压缩算法: %1").arg(opt.filter);
        archive_write_free(a);
        return false;
    }

    // 选项：压缩级别 + tar/pax 的 UTF-8 文件名 + zip 强制 UTF-8
    // 注意：不设置任何 passphrase，产物一律为无加密的标准归档
    QVector<QString> optList;
    if (opt.level >= 0 && opt.level <= 9)
        optList.append(QString("compression-level=%1").arg(opt.level));
    if (opt.format == "pax" || opt.format == "tar" || opt.format == "ustar" || opt.format == "gnutar")
        optList.append("hdrcharset=UTF-8");
    if (opt.format == "zip")
        optList.append("zip:utf8");
    if (!optList.isEmpty())
        archive_write_set_options(a, optList.join(',').toUtf8().constData());

    if (archive_write_open_filename(a, opt.archivePath.toUtf8().constData()) != ARCHIVE_OK) {
        err = QString::fromUtf8(archive_error_string(a));
        archive_write_free(a);
        return false;
    }

    qint64 total = totalInputBytes(opt.inputs);
    qint64 done = 0;
    bool ok = true;
    for (const QString& input : opt.inputs) {
        if (cancel && cancel()) { ok = false; err = QCoreApplication::translate("ArchiveEngine","已取消"); break; }
        QFileInfo fi(input);
        if (!fi.exists()) {
            err = QCoreApplication::translate("ArchiveEngine", "路径不存在: %1").arg(input);
            ok = false; break;
        }
        if (fi.isDir()) {
            if (!addDirEntry(a, input, fi.fileName(), done, total, progress, cancel, err)) { ok = false; break; }
        } else {
            if (!addFileEntry(a, input, fi.fileName(), done, total, progress, cancel, err)) { ok = false; break; }
        }
    }

    if (archive_write_close(a) != ARCHIVE_OK && ok) {
        err = QString::fromUtf8(archive_error_string(a));
        ok = false;
    }
    archive_write_free(a);
    return ok;
}

bool extract(const QString& archivePath, const QString& destDir,
             ProgressFn progress, CancelFn cancel, QString& err) {
    QFileInfo srcInfo(archivePath);
    if (!srcInfo.exists()) {
        err = QCoreApplication::translate("ArchiveEngine", "归档文件不存在: %1").arg(archivePath);
        return false;
    }

    // 第一遍：统计总字节数用于进度
    qint64 total = 0;
    {
        struct archive* scan = archive_read_new();
        archive_read_support_filter_all(scan);
        archive_read_support_format_all(scan);
        if (archive_read_open_filename(scan, archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
            err = QString::fromUtf8(archive_error_string(scan));
            archive_read_free(scan);
            return false;
        }
        struct archive_entry* entry;
        int r;
        while ((r = archive_read_next_header(scan, &entry)) == ARCHIVE_OK) {
            total += archive_entry_size(entry);
            archive_read_data_skip(scan);
        }
        archive_read_free(scan);
        if (r != ARCHIVE_EOF && r != ARCHIVE_OK) {
            err = QCoreApplication::translate("ArchiveEngine", "无法读取归档: %1").arg(archivePath);
            return false;
        }
    }

    // 第二遍：实际解压
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, archivePath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        err = QString::fromUtf8(archive_error_string(a));
        archive_read_free(a);
        return false;
    }

    QDir dest(destDir);
    dest.mkpath(".");
    qint64 done = 0;
    bool ok = true;
    struct archive_entry* entry;
    int r;
    while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
        if (cancel && cancel()) { ok = false; err = QCoreApplication::translate("ArchiveEngine","已取消"); break; }

        QString name = QString::fromUtf8(archive_entry_pathname(entry));
        QString outPath = safeDestPath(destDir, name);
        if (outPath.isEmpty()) {
            // 跳过不安全的路径
            archive_read_data_skip(a);
            continue;
        }

        mode_t filetype = archive_entry_filetype(entry);
        if (filetype == AE_IFDIR) {
            QDir().mkpath(outPath);
        } else if (filetype == AE_IFREG) {
            QFileInfo ofi(outPath);
            QDir().mkpath(ofi.absolutePath());
            QFile out(outPath);
            if (!out.open(QIODevice::WriteOnly)) {
                err = QCoreApplication::translate("ArchiveEngine", "无法写入: %1").arg(outPath);
                ok = false; break;
            }
            const void* buff;
            size_t size;
            la_int64_t offset;
            int dr;
            while ((dr = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
                if (cancel && cancel()) { out.close(); ok = false; err = QCoreApplication::translate("ArchiveEngine","已取消"); break; }
                qint64 wrote = out.write(reinterpret_cast<const char*>(buff), static_cast<qint64>(size));
                if (wrote < 0) { err = QCoreApplication::translate("ArchiveEngine","写入失败"); ok=false; break; }
                done += static_cast<qint64>(size);
                if (progress) progress(done, total, name);
            }
            out.close();
            if (dr != ARCHIVE_EOF) {
                err = QString::fromUtf8(archive_error_string(a));
                ok = false; break;
            }
            // 还原修改时间
            time_t mtime = archive_entry_mtime(entry);
            if (mtime > 0) {
                QFile tf(outPath);
                tf.setFileTime(QDateTime::fromSecsSinceEpoch(mtime), QFileDevice::FileModificationTime);
            }
        } else {
            // 跳过符号链接/设备文件等非常规类型
            archive_read_data_skip(a);
        }
    }
    if (r != ARCHIVE_EOF && r != ARCHIVE_OK && ok) {
        err = QString::fromUtf8(archive_error_string(a));
        ok = false;
    }
    archive_read_free(a);
    return ok;
}

QString detectFormatLabel(const QString& archivePath) {
    QString s = archivePath.toLower();
    if (s.endsWith(".zip"))   return "ZIP";
    if (s.endsWith(".7z"))    return "7Z";
    if (s.endsWith(".tar"))   return "TAR";
    if (s.endsWith(".tgz") || s.endsWith(".tar.gz"))  return "TAR.GZ";
    if (s.endsWith(".tbz2") || s.endsWith(".tar.bz2")) return "TAR.BZ2";
    if (s.endsWith(".txz") || s.endsWith(".tar.xz"))   return "TAR.XZ";
    if (s.endsWith(".tzst") || s.endsWith(".tar.zst")) return "TAR.ZST";
    if (s.endsWith(".gz"))    return "GZ";
    if (s.endsWith(".bz2"))   return "BZ2";
    if (s.endsWith(".xz"))    return "XZ";
    if (s.endsWith(".zst"))   return "ZST";
    return "未知";
}

QVector<QString> supportedFormats() {
    // libarchive 引擎：全格式
    return {"ZIP", "7Z", "TAR", "TAR.GZ", "TAR.BZ2", "TAR.XZ", "TAR.ZST",
            "GZ", "BZ2", "XZ", "ZST"};
}

} // namespace ArchiveEngine
