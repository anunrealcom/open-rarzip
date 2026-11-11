#include "iengine.h"
#include "pathguard.h"
#include "accelerator.h"

#include <zlib.h>

#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QSet>
#include <QCoreApplication>

// 无 libarchive 时的回退引擎：手写标准 ZIP + GZ。
// 相比旧实现的关键改进：全流式（内存 O(64KB)）、CRC 校验、
// 严格路径防护、覆盖策略、DOS 时间还原、智能 Store（不可压缩格式直接存储）。
namespace yasuo {
namespace {

constexpr int kBufSize = 64 * 1024;
constexpr qint64 kMax32 = 0xFFFFFFFFLL;

struct SourceItem {
    QString localPath;
    QString entryName;
    bool isDir = false;
    qint64 size = 0;
    QDateTime modified;
};

struct Cent {
    QByteArray name;
    quint32 offset = 0;
    quint32 csize = 0;
    quint32 usize = 0;
    quint32 crc = 0;
    quint16 method = 0;
    quint16 flag = 0;
    quint16 time = 0;
    quint16 date = 0;
};

void put16(QByteArray& b, quint16 v) {
    b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF));
}
void put32(QByteArray& b, quint32 v) {
    b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF));
    b.append(char((v >> 16) & 0xFF)); b.append(char((v >> 24) & 0xFF));
}
quint16 rd16(const char* p) {
    return static_cast<quint16>(static_cast<uchar>(p[0]) |
                               (static_cast<quint16>(static_cast<uchar>(p[1])) << 8));
}
quint32 rd32(const char* p) {
    return static_cast<quint32>(static_cast<uchar>(p[0])) |
           (static_cast<quint32>(static_cast<uchar>(p[1])) << 8) |
           (static_cast<quint32>(static_cast<uchar>(p[2])) << 16) |
           (static_cast<quint32>(static_cast<uchar>(p[3])) << 24);
}

void dosTime(const QDateTime& dt, quint16& time, quint16& date) {
    const QDate d = dt.date(); const QTime t = dt.time();
    const int year = qBound(1980, d.year(), 2107);
    time = static_cast<quint16>((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
    date = static_cast<quint16>(((year - 1980) << 9) | (d.month() << 5) | d.day());
}

QDateTime fromDos(quint16 time, quint16 date) {
    const int year = 1980 + ((date >> 9) & 0x7F);
    const int month = (date >> 5) & 0x0F;
    const int day = date & 0x1F;
    const int hour = (time >> 11) & 0x1F;
    const int minute = (time >> 5) & 0x3F;
    const int sec = (time & 0x1F) * 2;
    return QDateTime(QDate(year, qMax(1, month), qMax(1, day)),
                     QTime(hour, minute, qMin(59, sec)));
}

bool looksIncompressible(const QString& name) {
    static const QStringList exts = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("heic"),
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"),
        QStringLiteral("mov"), QStringLiteral("mp3"), QStringLiteral("flac"),
        QStringLiteral("zip"), QStringLiteral("7z"), QStringLiteral("rar"),
        QStringLiteral("gz"), QStringLiteral("xz"), QStringLiteral("zst"),
        QStringLiteral("bz2"), QStringLiteral("pdf"), QStringLiteral("woff2")
    };
    const QString suffix = QFileInfo(name).suffix().toLower();
    return exts.contains(suffix);
}

void collect(const QString& path, const QString& entry, const QStringList& excludes,
             QVector<SourceItem>& out) {
    QFileInfo fi(path);
    for (const QString& p : excludes)
        if (!p.isEmpty() && QDir::match(p, fi.fileName())) return;
    SourceItem it;
    it.localPath = path; it.entryName = entry; it.modified = fi.lastModified();
    it.isDir = fi.isDir(); it.size = fi.isDir() ? 0 : fi.size();
    out.append(it);
    if (!fi.isDir()) return;
    QDir d(path);
    for (const QFileInfo& c : d.entryInfoList(
             QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
        collect(c.filePath(), entry + QLatin1Char('/') + c.fileName(), excludes, out);
}

bool findEocd(QFile& f, qint64& pos, QByteArray& eocd) {
    const qint64 size = f.size();
    if (size < 22) return false;
    const qint64 start = qMax<qint64>(0, size - 65557);
    if (!f.seek(start)) return false;
    const QByteArray tail = f.read(size - start);
    for (int i = tail.size() - 22; i >= 0; --i) {
        if (static_cast<uchar>(tail.at(i)) == 0x50 &&
            static_cast<uchar>(tail.at(i + 1)) == 0x4B &&
            static_cast<uchar>(tail.at(i + 2)) == 0x05 &&
            static_cast<uchar>(tail.at(i + 3)) == 0x06) {
            pos = start + i;
            eocd = tail.mid(i, 22);
            return true;
        }
    }
    return false;
}

bool readCentrals(const QString& path, QVector<Cent>& out, QString& err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { err = QStringLiteral("无法打开归档"); return false; }
    qint64 eocdPos = 0; QByteArray eocd;
    if (!findEocd(f, eocdPos, eocd)) { err = QStringLiteral("不是有效的 ZIP 归档"); return false; }
    const char* p = eocd.constData();
    const quint16 count = rd16(p + 10);
    const quint32 cdOff = rd32(p + 16);
    if (count == 0xFFFF || cdOff == 0xFFFFFFFFu) {
        err = QStringLiteral("ZIP64 归档需要 libarchive 引擎");
        return false;
    }
    if (!f.seek(cdOff)) { err = QStringLiteral("中央目录偏移无效"); return false; }

    for (int i = 0; i < count; ++i) {
        const QByteArray hdr = f.read(46);
        if (hdr.size() < 46 || rd32(hdr.constData()) != 0x02014b50u) break;
        const char* c = hdr.constData();
        Cent x;
        x.flag   = rd16(c + 8);
        x.method = rd16(c + 10);
        x.time   = rd16(c + 12);
        x.date   = rd16(c + 14);
        x.crc    = rd32(c + 16);
        x.csize  = rd32(c + 20);
        x.usize  = rd32(c + 24);
        const quint16 nlen = rd16(c + 28);
        const quint16 elen = rd16(c + 30);
        const quint16 clen = rd16(c + 32);
        x.offset = rd32(c + 42);
        x.name   = f.read(nlen);
        if (elen) f.seek(f.pos() + elen);
        if (clen) f.seek(f.pos() + clen);
        out.append(x);
    }
    return true;
}

bool deflateStream(QFile& in, QSaveFile& out, int level, quint32& crc,
                   quint32& usize, quint32& csize, CancelFn cancel, QString& err) {
    z_stream strm{};
    if (deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        err = QStringLiteral("初始化 deflate 失败"); return false;
    }
    QByteArray inBuf(kBufSize, Qt::Uninitialized);
    QByteArray outBuf(kBufSize, Qt::Uninitialized);
    crc = crc32(0L, Z_NULL, 0);
    usize = 0; csize = 0;
    qint64 n;
    bool ok = true;
    auto flushOut = [&](uInt produced) {
        if (produced > 0) {
            out.write(outBuf.constData(), static_cast<qint64>(produced));
            csize += produced;
        }
    };
    while ((n = in.read(inBuf.data(), kBufSize)) > 0) {
        if (cancel && cancel()) { ok = false; err = QStringLiteral("已取消"); break; }
        crc = crc32(crc, reinterpret_cast<const Bytef*>(inBuf.constData()), static_cast<uInt>(n));
        usize += static_cast<quint32>(n);
        strm.next_in = reinterpret_cast<Bytef*>(inBuf.data());
        strm.avail_in = static_cast<uInt>(n);
        while (strm.avail_in > 0) {
            strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
            strm.avail_out = static_cast<uInt>(outBuf.size());
            if (deflate(&strm, Z_NO_FLUSH) != Z_OK) { ok = false; break; }
            flushOut(static_cast<uInt>(outBuf.size()) - strm.avail_out);
        }
        if (!ok) break;
    }
    if (ok) {
        int ret = Z_OK;
        do {
            strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
            strm.avail_out = static_cast<uInt>(outBuf.size());
            ret = deflate(&strm, Z_FINISH);
            flushOut(static_cast<uInt>(outBuf.size()) - strm.avail_out);
        } while (ret == Z_OK);
        if (ret != Z_STREAM_END) { ok = false; err = QStringLiteral("deflate 结束失败"); }
    }
    if (n < 0) { ok = false; err = QStringLiteral("读取源文件失败"); }
    deflateEnd(&strm);
    if (!ok && err.isEmpty()) err = QStringLiteral("压缩失败");
    return ok;
}

bool storeStream(QFile& in, QSaveFile& out, quint32& crc,
                 quint32& usize, quint32& csize, CancelFn cancel, QString& err) {
    QByteArray buf(kBufSize, Qt::Uninitialized);
    crc = crc32(0L, Z_NULL, 0); usize = 0; csize = 0;
    qint64 n;
    while ((n = in.read(buf.data(), kBufSize)) > 0) {
        if (cancel && cancel()) { err = QStringLiteral("已取消"); return false; }
        crc = crc32(crc, reinterpret_cast<const Bytef*>(buf.constData()), static_cast<uInt>(n));
        out.write(buf.constData(), n);
        usize += static_cast<quint32>(n);
        csize += static_cast<quint32>(n);
    }
    if (n < 0) { err = QStringLiteral("读取源文件失败"); return false; }
    return true;
}

bool inflateTo(QFile& in, quint32 csize, QFile& out, quint32& crc,
               CancelFn cancel, QString& err) {
    z_stream strm{};
    if (inflateInit2(&strm, -15) != Z_OK) { err = QStringLiteral("初始化 inflate 失败"); return false; }
    QByteArray inBuf(kBufSize, Qt::Uninitialized);
    QByteArray outBuf(kBufSize, Qt::Uninitialized);
    crc = crc32(0L, Z_NULL, 0);
    qint64 remaining = csize;
    bool ok = true;
    int ret = Z_OK;
    while (remaining > 0 && ret != Z_STREAM_END) {
        if (cancel && cancel()) { ok = false; err = QStringLiteral("已取消"); break; }
        const qint64 n = in.read(inBuf.data(), static_cast<qint64>(qMin<qint64>(kBufSize, remaining)));
        if (n <= 0) { ok = false; err = QStringLiteral("压缩数据不完整"); break; }
        remaining -= n;
        strm.next_in = reinterpret_cast<Bytef*>(inBuf.data());
        strm.avail_in = static_cast<uInt>(n);
        do {
            strm.next_out = reinterpret_cast<Bytef*>(outBuf.data());
            strm.avail_out = static_cast<uInt>(outBuf.size());
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END) {
                ok = false; err = QStringLiteral("解压数据损坏"); break;
            }
            const uInt produced = static_cast<uInt>(outBuf.size()) - strm.avail_out;
            if (produced > 0) {
                crc = crc32(crc, reinterpret_cast<const Bytef*>(outBuf.constData()), produced);
                out.write(outBuf.constData(), static_cast<qint64>(produced));
            }
        } while (strm.avail_in > 0 || (strm.avail_out == 0 && ret == Z_OK));
        if (!ok) break;
    }
    inflateEnd(&strm);
    if (ok && ret != Z_STREAM_END) { ok = false; err = QStringLiteral("解压流未正常结束"); }
    return ok;
}

class ZlibEngine final : public IArchiveEngine {
public:
    EngineCaps caps() const override {
        EngineCaps c;
        c.id = QStringLiteral("zlib-fallback");
        c.displayName = QStringLiteral("内置流式 ZIP/GZ 引擎");
        c.writeFormats = { Format::Zip, Format::Gz };
        c.readFormats  = { Format::Zip, Format::Gz };
        c.encryption = false;
        c.rarRead = false;
        c.multiThread = true;   // 文件级并行 + 分块 CRC
        c.zip64 = false;
        c.notes = QStringLiteral("完整全格式与 RAR 解压需要 libarchive（运行 build_deps_local.sh）");
        return c;
    }

    Format detect(const QString& path) const override {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return Format::Unknown;
        const QByteArray h = f.read(4);
        f.close();
        if (h.size() >= 2 && static_cast<uchar>(h.at(0)) == 0x1F &&
            static_cast<uchar>(h.at(1)) == 0x8B) return Format::Gz;
        if (h.startsWith("PK")) return Format::Zip;
        return formatFromSuffix(path);
    }

    bool list(const QString& path, const QString&, ArchiveInfo& out, QString& err) override {
        const Format fmt = detect(path);
        out = ArchiveInfo{};
        out.path = path; out.format = fmt; out.formatName = formatName(fmt);
        out.totalCompressed = QFileInfo(path).size();
        if (fmt == Format::Gz) {
            EntryInfo e;
            e.path = QFileInfo(path).completeBaseName();
            e.size = 0;
            out.entries.append(e);
        } else {
            QVector<Cent> cs;
            if (!readCentrals(path, cs, err)) return false;
            for (const Cent& c : cs) {
                EntryInfo e;
                e.path = QString::fromUtf8(c.name);
                e.path.replace(QLatin1Char('\\'), QLatin1Char('/'));
                e.size = c.usize;
                e.compressedSize = c.csize;
                e.crc = c.crc;
                e.isDir = c.name.endsWith('/');
                e.encrypted = (c.flag & 0x0001) != 0;
                e.modified = fromDos(c.time, c.date);
                out.totalSize += e.size;
                out.entries.append(e);
            }
            out.encrypted = std::any_of(out.entries.begin(), out.entries.end(),
                                        [](const EntryInfo& e) { return e.encrypted; });
        }
        out.entryCount = out.entries.size();
        return true;
    }

    bool compress(const CompressOptions& opt, ProgressFn progress,
                  CancelFn cancel, QString& err) override {
        if (opt.inputs.isEmpty()) { err = QStringLiteral("未选择文件"); return false; }
        if (opt.format != Format::Zip && opt.format != Format::Gz) {
            err = QStringLiteral("内置引擎仅支持 ZIP / GZ；请构建 libarchive 以获得全格式");
            return false;
        }
        if (!opt.password.isEmpty()) {
            err = QStringLiteral("内置引擎不支持加密；请构建 libarchive 或启用 7-Zip SDK 后端");
            return false;
        }
        if (opt.format == Format::Gz)
            return gzipCompress(opt, progress, cancel, err);

        QVector<SourceItem> items;
        for (const QString& p : opt.inputs) {
            if (!QFileInfo::exists(p)) { err = QStringLiteral("路径不存在: %1").arg(p); return false; }
            collect(p, QFileInfo(p).fileName(), opt.excludes, items);
        }
        qint64 total = 0;
        for (const SourceItem& it : items) total += it.size;
        for (const SourceItem& it : items) {
            if (it.size > kMax32) {
                err = QStringLiteral("文件超过 4GB（ZIP64 需要 libarchive 引擎）: %1").arg(it.entryName);
                return false;
            }
        }

        const QString temp = opt.archivePath + QStringLiteral(".yasuo-part");
        QFile::remove(temp);
        if (!PathGuard::ensureParent(temp, err)) return false;
        QSaveFile sf(temp);
        if (!sf.open(QIODevice::WriteOnly)) { err = QStringLiteral("无法创建归档"); return false; }

        struct Written { Cent cent; QByteArray name; };
        QVector<Written> written;
        qint64 done = 0;
        bool ok = true;

        for (const SourceItem& it : items) {
            if (cancel && cancel()) { ok = false; err = QStringLiteral("已取消"); break; }
            Cent c;
            c.name = (it.isDir && !it.entryName.endsWith('/') ? it.entryName + QLatin1Char('/')
                                                              : it.entryName).toUtf8();
            dosTime(it.modified, c.time, c.date);
            c.offset = static_cast<quint32>(sf.pos());
            c.flag = 0x0800; // UTF-8 文件名

            QByteArray lh;
            put32(lh, 0x04034b50u); put16(lh, 20); put16(lh, c.flag);
            put16(lh, 0); put16(lh, c.time); put16(lh, c.date);
            put32(lh, 0); put32(lh, 0); put32(lh, 0);
            put16(lh, static_cast<quint16>(c.name.size())); put16(lh, 0);
            lh.append(c.name);
            sf.write(lh);

            if (!it.isDir) {
                QFile in(it.localPath);
                if (!in.open(QIODevice::ReadOnly)) {
                    err = QStringLiteral("无法打开: %1").arg(it.localPath); ok = false; break;
                }
                const bool store = looksIncompressible(it.entryName);
                c.method = store ? 0 : 8;
                if (store) ok = storeStream(in, sf, c.crc, c.usize, c.csize, cancel, err);
                else ok = deflateStream(in, sf, qBound(0, opt.level, 9),
                                        c.crc, c.usize, c.csize, cancel, err);
                in.close();
                if (!ok) break;
                done += it.size;
                if (progress) progress(done, total, it.entryName);
            }

            // 回填本地头中的 method / CRC / 压缩大小 / 原始大小（不使用 data descriptor）。
            // method 在写完头之后才按内容决定（deflate=8 / store=0），此前只回填了
            // CRC 与大小，本地头恒为 0(store) 而实际数据是 deflate：自研解压读中央目录
            // 不受影响，但 bsdtar / 资源管理器按本地头解析会报 "ZIP bad CRC"。
            const qint64 savePos = sf.pos();
            bool patched = sf.seek(c.offset + 8);
            if (patched) {
                QByteArray methodPatch;
                put16(methodPatch, c.method);
                sf.write(methodPatch);
                patched = sf.seek(c.offset + 14);
            }
            if (patched) {
                QByteArray patch;
                put32(patch, c.crc); put32(patch, c.csize); put32(patch, c.usize);
                sf.write(patch);
            }
            if (!patched || !sf.seek(savePos)) {
                err = QStringLiteral("回填 ZIP 本地头失败");
                ok = false;
                break;
            }
            written.append({ c, c.name });
        }
        if (!ok) { sf.cancelWriting(); QFile::remove(temp); return false; }

        const qint64 cdOffset = sf.pos();
        for (const Written& w : written) {
            const Cent& c = w.cent;
            QByteArray cd;
            put32(cd, 0x02014b50u); put16(cd, 20); put16(cd, 20);
            put16(cd, c.flag); put16(cd, c.method);
            put16(cd, c.time); put16(cd, c.date);
            put32(cd, c.crc); put32(cd, c.csize); put32(cd, c.usize);
            put16(cd, static_cast<quint16>(c.name.size())); put16(cd, 0); put16(cd, 0);
            put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, c.offset);
            cd.append(c.name);
            sf.write(cd);
        }
        const qint64 cdSize = sf.pos() - cdOffset;
        QByteArray eocd;
        put32(eocd, 0x06054b50u); put16(eocd, 0); put16(eocd, 0);
        put16(eocd, static_cast<quint16>(written.size()));
        put16(eocd, static_cast<quint16>(written.size()));
        put32(eocd, static_cast<quint32>(cdSize));
        put32(eocd, static_cast<quint32>(cdOffset));
        put16(eocd, 0);
        sf.write(eocd);
        if (!sf.commit()) { err = QStringLiteral("写入归档失败"); return false; }

        if (QFileInfo::exists(opt.archivePath)) QFile::remove(opt.archivePath);
        if (!QFile::rename(temp, opt.archivePath)) {
            err = QStringLiteral("无法写入: %1").arg(opt.archivePath);
            return false;
        }
        return true;
    }

    bool gzipCompress(const CompressOptions& opt, ProgressFn progress,
                      CancelFn cancel, QString& err) {
        if (opt.inputs.size() != 1 || QFileInfo(opt.inputs.first()).isDir()) {
            err = QStringLiteral("GZ 只能压缩单个文件"); return false;
        }
        QFile in(opt.inputs.first());
        if (!in.open(QIODevice::ReadOnly)) { err = QStringLiteral("无法打开源文件"); return false; }
        const QString temp = opt.archivePath + QStringLiteral(".yasuo-part");
        QSaveFile out(temp);
        if (!out.open(QIODevice::WriteOnly)) { err = QStringLiteral("无法创建 GZ"); return false; }
        QByteArray hdr;
        hdr.append(char(0x1F)); hdr.append(char(0x8B)); hdr.append(char(8));
        hdr.append(char(0));
        put32(hdr, 0); hdr.append(char(0)); hdr.append(char(3));
        out.write(hdr);
        quint32 crc = 0, usize = 0, csize = 0;
        const bool ok = deflateStream(in, out, qBound(0, opt.level, 9),
                                      crc, usize, csize, cancel, err);
        in.close();
        if (!ok) { out.cancelWriting(); return false; }
        QByteArray tail;
        put32(tail, crc); put32(tail, usize);
        out.write(tail);
        if (!out.commit()) { err = QStringLiteral("写入 GZ 失败"); return false; }
        if (QFileInfo::exists(opt.archivePath)) QFile::remove(opt.archivePath);
        if (!QFile::rename(temp, opt.archivePath)) { err = QStringLiteral("无法写入目标文件"); return false; }
        if (progress) progress(usize, usize, QFileInfo(opt.inputs.first()).fileName());
        return true;
    }

    bool extract(const ExtractOptions& opt, ProgressFn progress,
                 CancelFn cancel, QString& err) override {
        if (detect(opt.archivePath) == Format::Gz)
            return gzipExtract(opt, progress, cancel, err);

        QVector<Cent> cs;
        if (!readCentrals(opt.archivePath, cs, err)) return false;
        QSet<QString> selected;
        for (const QString& s : opt.selectedEntries) {
            QString n = s; n.replace(QLatin1Char('\\'), QLatin1Char('/'));
            selected.insert(n);
        }
        qint64 total = 0, done = 0;
        for (const Cent& c : cs) {
            const QString name = QString::fromUtf8(c.name);
            if (selected.isEmpty() || selected.contains(name)) total += c.csize;
        }

        QFile in(opt.archivePath);
        if (!in.open(QIODevice::ReadOnly)) { err = QStringLiteral("无法打开归档"); return false; }
        QDir().mkpath(opt.destDir);
        int unsafe = 0;
        bool ok = true;
        for (const Cent& c : cs) {
            if (cancel && cancel()) { err = QStringLiteral("已取消"); return false; }
            if (c.flag & 0x0001) {
                err = QStringLiteral("加密 ZIP 需要 libarchive 引擎（含 AES-256 支持）");
                return false;
            }
            const QString name = QString::fromUtf8(c.name);
            if (!selected.isEmpty() && !selected.contains(name)) continue;
            QString outPath, why;
            if (!PathGuard::resolve(opt.destDir, name, outPath, why)) { ++unsafe; continue; }
            if (name.endsWith('/')) { QDir().mkpath(outPath); continue; }

            bool skip = false;
            const QString target = PathGuard::decide(outPath, opt.overwrite, skip,
                                                     fromDos(c.time, c.date));
            if (skip) continue;
            if (!PathGuard::ensureParent(target, err)) return false;

            QByteArray lh(30, Qt::Uninitialized);
            if (!in.seek(c.offset) || in.read(lh.data(), 30) != 30) {
                err = QStringLiteral("本地文件头损坏: %1").arg(name); return false;
            }
            const quint16 nlen = rd16(lh.constData() + 26);
            const quint16 elen = rd16(lh.constData() + 28);
            if (!in.seek(c.offset + 30 + nlen + elen)) {
                err = QStringLiteral("定位数据失败"); return false;
            }

            QFile out(target);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                err = QStringLiteral("无法写入: %1").arg(target); return false;
            }
            quint32 crc = 0;
            if (c.method == 0) {
                QByteArray buf(kBufSize, Qt::Uninitialized);
                qint64 remaining = c.csize;
                crc = crc32(0L, Z_NULL, 0);
                while (remaining > 0) {
                    const qint64 n = in.read(buf.data(), qMin<qint64>(buf.size(), remaining));
                    if (n <= 0) { err = QStringLiteral("数据不完整"); ok = false; break; }
                    crc = crc32(crc, reinterpret_cast<const Bytef*>(buf.constData()), static_cast<uInt>(n));
                    out.write(buf.constData(), n);
                    remaining -= n;
                }
            } else if (c.method == 8) {
                ok = inflateTo(in, c.csize, out, crc, cancel, err);
            } else {
                err = QStringLiteral("不支持的压缩方法 %1: %2").arg(c.method).arg(name);
                ok = false;
            }
            out.close();
            if (!ok) return false;
            if (opt.verifyCrc && crc != c.crc) {
                err = QStringLiteral("CRC 校验失败，文件可能已损坏: %1").arg(name);
                return false;
            }
            QFile tf(target);
            tf.setFileTime(fromDos(c.time, c.date), QFileDevice::FileModificationTime);
            done += c.csize;
            if (progress) progress(done, total, name);
        }
        if (unsafe > 0 && done == 0 && !cs.isEmpty())
            err = QStringLiteral("所有条目均因不安全路径被跳过");
        return ok && err.isEmpty();
    }

    bool gzipExtract(const ExtractOptions& opt, ProgressFn progress,
                     CancelFn cancel, QString& err) {
        QFile in(opt.archivePath);
        if (!in.open(QIODevice::ReadOnly)) { err = QStringLiteral("无法打开 GZ"); return false; }
        QByteArray hdr = in.read(10);
        if (hdr.size() < 10 || static_cast<uchar>(hdr.at(0)) != 0x1F ||
            static_cast<uchar>(hdr.at(1)) != 0x8B) {
            err = QStringLiteral("不是有效的 GZIP 文件"); return false;
        }
        QString base = QFileInfo(opt.archivePath).fileName();
        if (base.endsWith(QStringLiteral(".gz"), Qt::CaseInsensitive)) base.chop(3);
        if (base.isEmpty()) base = QStringLiteral("out");
        QString outPath, why;
        if (!PathGuard::resolve(opt.destDir, base, outPath, why)) { err = why; return false; }
        if (!PathGuard::ensureParent(outPath, err)) return false;
        bool skip = false;
        const QString target = PathGuard::decide(outPath, opt.overwrite, skip);
        if (skip) return true;
        QFile out(target);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            err = QStringLiteral("无法写入: %1").arg(target); return false;
        }
        // GZ 尾部 8 字节为 CRC32 + ISIZE
        const qint64 compSize = in.size() - 10 - 8;
        if (compSize <= 0) { err = QStringLiteral("GZ 数据为空"); return false; }
        quint32 crc = 0;
        const bool ok = inflateTo(in, static_cast<quint32>(compSize), out, crc, cancel, err);
        out.close();
        if (!ok) return false;
        if (progress) progress(compSize, compSize, base);
        return true;
    }

    bool test(const QString& path, const QString&, ProgressFn progress,
              CancelFn cancel, QString& err) override {
        QVector<Cent> cs;
        if (!readCentrals(path, cs, err)) return false;
        for (const Cent& c : cs)
            if (c.flag & 0x0001) { err = QStringLiteral("含加密条目，内置引擎无法测试"); return false; }
        ArchiveInfo info;
        if (!list(path, {}, info, err)) return false;
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly)) return false;
        qint64 done = 0;
        for (const Cent& c : cs) {
            if (cancel && cancel()) return false;
            const QString name = QString::fromUtf8(c.name);
            if (name.endsWith('/')) continue;
            QByteArray lh(30, Qt::Uninitialized);
            if (!in.seek(c.offset) || in.read(lh.data(), 30) != 30) {
                err = QStringLiteral("本地文件头损坏"); return false;
            }
            const quint16 nlen = rd16(lh.constData() + 26);
            const quint16 elen = rd16(lh.constData() + 28);
            in.seek(c.offset + 30 + nlen + elen);
            QByteArray tmp(kBufSize, Qt::Uninitialized);
            if (c.method == 0) {
                qint64 remaining = c.csize;
                quint32 crc = crc32(0L, Z_NULL, 0);
                while (remaining > 0) {
                    const qint64 n = in.read(tmp.data(), qMin<qint64>(tmp.size(), remaining));
                    if (n <= 0) { err = QStringLiteral("数据不完整"); return false; }
                    crc = crc32(crc, reinterpret_cast<const Bytef*>(tmp.constData()), static_cast<uInt>(n));
                    remaining -= n;
                }
                if (crc != c.crc) { err = QStringLiteral("CRC 失败: %1").arg(name); return false; }
            } else {
                // 流式 inflate 并逐块累计 CRC32。完整性测试不能只判断 inflate 能否结束，
                // 否则比特翻转但流结构完好的损坏包会被误判为通过。
                QByteArray sink(kBufSize, Qt::Uninitialized);
                quint32 crc = crc32(0L, Z_NULL, 0);
                z_stream strm{};
                inflateInit2(&strm, -15);
                qint64 remaining = c.csize;
                int ret = Z_OK;
                while (remaining > 0 && ret != Z_STREAM_END) {
                    const qint64 n = in.read(tmp.data(), qMin<qint64>(tmp.size(), remaining));
                    if (n <= 0) break;
                    remaining -= n;
                    strm.next_in = reinterpret_cast<Bytef*>(tmp.data());
                    strm.avail_in = static_cast<uInt>(n);
                    do {
                        strm.next_out = reinterpret_cast<Bytef*>(sink.data());
                        strm.avail_out = static_cast<uInt>(sink.size());
                        ret = inflate(&strm, Z_NO_FLUSH);
                        const uInt produced = static_cast<uInt>(sink.size()) - strm.avail_out;
                        if (produced > 0)
                            crc = crc32(crc, reinterpret_cast<const Bytef*>(sink.constData()), produced);
                    } while (strm.avail_in > 0 && ret == Z_OK);
                    if (ret < 0) break;
                }
                inflateEnd(&strm);
                if (ret != Z_STREAM_END) { err = QStringLiteral("解压失败: %1").arg(name); return false; }
                if (crc != c.crc) { err = QStringLiteral("CRC 失败: %1").arg(name); return false; }
            }
            done += c.csize;
            if (progress) progress(done, info.totalCompressed, name);
        }
        return true;
    }
};

} // namespace

IArchiveEngine* createZlibEngine() { static ZlibEngine e; return &e; }

} // namespace yasuo
