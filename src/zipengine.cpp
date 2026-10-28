#include "archiveengine.h"

#include <zlib.h>

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QVector>
#include <QCoreApplication>
#include <QSaveFile>

// 说明：本引擎输出的是**无加密、无 ZIP64、无数据描述符**的标准 ZIP / GZ，
// 可以被 WinRAR / 7-Zip / WinZIP / 资源管理器 / tar / unzip 等任意标准工具直接打开。
// 不实现任何私有加密，避免出现"只有本程序能解开"的归档。

namespace {

constexpr quint64 kMax32 = 0xFFFFFFFFull;

void put16(QByteArray& b, quint16 v) { b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF)); }
void put32(QByteArray& b, quint32 v) { b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF)); b.append(char((v >> 16) & 0xFF)); b.append(char((v >> 24) & 0xFF)); }

// 小端读取（Qt6 移除了 qFromLittleEndian<T> 模板形式，自写保证可移植）
inline quint16 rd16(const uchar* p) { return quint16(p[0]) | (quint16(p[1]) << 8); }
inline quint32 rd32(const uchar* p) { return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24); }
#define RD16(x) rd16(reinterpret_cast<const uchar*>(x))
#define RD32(x) rd32(reinterpret_cast<const uchar*>(x))

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
    QString absDest = QDir::cleanPath(dest.absolutePath());
    if (!out.startsWith(absDest)) return QString();
    return out;
}

qint64 totalInputBytes(const QStringList& inputs) {
    qint64 sum = 0;
    for (const QString& p : inputs) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            QDir dir(p);
            QStringList sub;
            for (const QFileInfo& c : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) sub.append(c.filePath());
            sum += totalInputBytes(sub);
        } else if (fi.exists()) sum += fi.size();
    }
    return sum;
}

bool deflateData(const QByteArray& in, int level, QByteArray& out) {
    z_stream strm{};
    if (deflateInit2(&strm, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    out.clear();
    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.constData()));
    strm.avail_in = static_cast<uInt>(in.size());
    QByteArray buf(65536, 0);
    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        ret = deflate(&strm, Z_FINISH);
        if (ret == Z_STREAM_ERROR) { deflateEnd(&strm); return false; }
        out.append(buf.constData(), static_cast<int>(buf.size() - strm.avail_out));
    } while (strm.avail_out == 0);
    deflateEnd(&strm);
    return ret == Z_STREAM_END;
}

bool inflateData(const QByteArray& in, QByteArray& out) {
    z_stream strm{};
    if (inflateInit2(&strm, -15) != Z_OK) return false;
    out.clear();
    strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(in.constData()));
    strm.avail_in = static_cast<uInt>(in.size());
    QByteArray buf(65536, 0);
    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) { inflateEnd(&strm); return false; }
        out.append(buf.constData(), static_cast<int>(buf.size() - strm.avail_out));
    } while (ret != Z_STREAM_END && strm.avail_in > 0);
    inflateEnd(&strm);
    return ret == Z_STREAM_END;
}

void dosTime(const QDateTime& dt, quint16& time, quint16& date) {
    QDate d = dt.date(); QTime t = dt.time();
    time = quint16((t.hour() << 11) | (t.minute() << 5) | (t.second() / 2));
    date = quint16(((d.year() - 1980) << 9) | (d.month() << 5) | d.day());
}

struct ZipEntry {
    QByteArray name;
    QByteArray raw;     // 未压缩原文
    quint32 crc = 0;
    quint16 time = 0, date = 0;
    bool isDir = false;
};

bool zipCompress(const QStringList& inputs, const QString& outPath, int level,
                 ProgressFn progress, CancelFn cancel, QString& err) {
    QVector<ZipEntry> entries;
    qint64 total = totalInputBytes(inputs);
    qint64 done = 0;
    std::function<bool(const QString&, const QString&)> add =
        [&](const QString& path, const QString& entryName) -> bool {
        if (cancel && cancel()) { err = QCoreApplication::translate("ZipEngine","已取消"); return false; }
        QFileInfo fi(path);
        if (fi.isDir()) {
            ZipEntry e; e.name = (entryName + "/").toUtf8(); e.isDir = true;
            dosTime(fi.lastModified(), e.time, e.date); entries.append(e);
            QDir dir(path);
            for (const QFileInfo& c : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
                if (!add(c.filePath(), entryName + "/" + c.fileName())) return false;
            }
            return true;
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) { err = QCoreApplication::translate("ZipEngine","无法打开: %1").arg(path); return false; }
        QByteArray raw = f.readAll(); f.close();
        ZipEntry e; e.name = entryName.toUtf8(); e.raw = raw;
        e.crc = quint32(crc32(0, reinterpret_cast<const Bytef*>(raw.constData()), quint32(raw.size())));
        dosTime(fi.lastModified(), e.time, e.date);
        entries.append(e);
        done += raw.size();
        if (progress) progress(done, total, entryName);
        return true;
    };
    for (const QString& in : inputs) {
        QFileInfo fi(in);
        QString root = fi.fileName();
        if (!add(in, root)) return false;
    }

    QSaveFile sf(outPath);
    if (!sf.open(QIODevice::WriteOnly)) { err = QCoreApplication::translate("ZipEngine","无法写入: %1").arg(outPath); return false; }
    QByteArray central;
    for (const ZipEntry& e : entries) {
        QByteArray finalData;
        quint16 method;
        if (e.isDir) {
            method = 0;
        } else {
            QByteArray defl;
            if (deflateData(e.raw, level, defl) && defl.size() < e.raw.size()) { finalData = defl; method = 8; }
            else { finalData = e.raw; method = 0; }
        }
        qint64 offset = sf.pos();
        // 不做 ZIP64，超过 4GB 直接明确报错，避免产出别的工具打不开的"坏包"
        if (quint64(e.raw.size()) > kMax32 || quint64(finalData.size()) > kMax32 || quint64(offset) > kMax32) {
            err = QCoreApplication::translate("ZipEngine","文件超过 4GB，本引擎不支持 ZIP64：%1").arg(QString::fromUtf8(e.name));
            return false;
        }
        quint32 usize = quint32(e.raw.size());
        quint32 csize = quint32(finalData.size());
        const quint16 flag = 0x0800;   // bit11: 文件名按 UTF-8 编码
        QByteArray lh;
        put32(lh, 0x04034b50); put16(lh, 20); put16(lh, flag); put16(lh, method);
        put16(lh, e.time); put16(lh, e.date);
        put32(lh, e.crc); put32(lh, csize); put32(lh, usize);
        put16(lh, quint16(e.name.size())); put16(lh, 0);
        lh.append(e.name);
        sf.write(lh); sf.write(finalData);
        QByteArray cd;
        put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, flag); put16(cd, method);
        put16(cd, e.time); put16(cd, e.date);
        put32(cd, e.crc); put32(cd, csize); put32(cd, usize);
        put16(cd, quint16(e.name.size())); put16(cd, 0); put16(cd, 0);
        put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, quint32(offset));
        cd.append(e.name);
        central.append(cd);
    }
    qint64 cdOffset = sf.pos();
    sf.write(central);
    QByteArray eocd;
    put32(eocd, 0x06054b50); put16(eocd, 0); put16(eocd, 0);
    put16(eocd, quint16(entries.size())); put16(eocd, quint16(entries.size()));
    put32(eocd, quint32(central.size())); put32(eocd, quint32(cdOffset)); put16(eocd, 0);
    sf.write(eocd);
    bool committed = sf.commit();
    if (!committed) err = QCoreApplication::translate("ZipEngine","提交写入失败: %1").arg(outPath);
    return committed;
}

bool zipExtract(const QString& arc, const QString& destDir,
                ProgressFn progress, CancelFn cancel, QString& err) {
    QFile f(arc);
    if (!f.open(QIODevice::ReadOnly)) { err = QCoreApplication::translate("ZipEngine","无法打开: %1").arg(arc); return false; }
    QByteArray all = f.readAll(); f.close();

    qint64 eocdPos = -1;
    for (qint64 i = all.size() - 22; i >= 0; --i) {
        if (quint8(all.at(i)) == 0x50 && quint8(all.at(i+1)) == 0x4b &&
            quint8(all.at(i+2)) == 0x05 && quint8(all.at(i+3)) == 0x06) { eocdPos = i; break; }
    }
    if (eocdPos < 0) { err = QCoreApplication::translate("ZipEngine","不是有效的 ZIP 文件"); return false; }
    const char* p = all.constData() + eocdPos;
    quint16 entries = RD16(p + 10);
    quint32 cdOff  = RD32(p + 16);
    qint64 pos = cdOff;
    struct Cent { QByteArray name; quint32 loff; quint16 method; quint32 csize; };
    QVector<Cent> cds;
    for (int i = 0; i < entries; ++i) {
        const char* c = all.constData() + pos;
        if (RD32(c) != 0x02014b50) break;
        Cent x;
        x.method = RD16(c + 10);
        x.csize  = RD32(c + 20);
        quint16 nlen = RD16(c + 28);
        quint16 elen = RD16(c + 30);
        quint16 clen = RD16(c + 32);
        x.loff   = RD32(c + 42);
        x.name   = all.mid(pos + 46, nlen);
        cds.append(x);
        pos += 46 + nlen + elen + clen;
    }

    QDir(destDir).mkpath(".");
    qint64 total = 0, done = 0;
    for (const Cent& x : cds) total += x.csize;
    for (const Cent& x : cds) {
        if (cancel && cancel()) { err = QCoreApplication::translate("ZipEngine","已取消"); return false; }
        const char* l = all.constData() + x.loff;
        if (RD32(l) != 0x04034b50) continue;
        quint16 nlen = RD16(l + 26);
        quint16 elen = RD16(l + 28);
        QByteArray name = x.name;
        qint64 dataPos = x.loff + 30 + nlen + elen;
        QByteArray comp = all.mid(dataPos, x.csize);
        QString outPath = safeDestPath(destDir, QString::fromUtf8(name));
        if (outPath.isEmpty()) continue;
        if (name.endsWith('/')) { QDir().mkpath(outPath); continue; }
        QByteArray raw;
        if (x.method == 0) raw = comp;
        else { if (!inflateData(comp, raw)) { err = QCoreApplication::translate("ZipEngine","解压失败: %1").arg(QString::fromUtf8(name)); return false; } }
        QFileInfo ofi(outPath); QDir().mkpath(ofi.absolutePath());
        QSaveFile out(outPath);
        if (!out.open(QIODevice::WriteOnly)) { err = QCoreApplication::translate("ZipEngine","无法写入: %1").arg(outPath); return false; }
        out.write(raw); out.commit();
        done += x.csize;
        if (progress) progress(done, total, QString::fromUtf8(name));
    }
    return true;
}

bool gzipCompress(const QString& inPath, const QString& outPath, int level, ProgressFn progress,
                  CancelFn cancel, QString& err) {
    QFile f(inPath);
    if (!f.open(QIODevice::ReadOnly)) { err = QCoreApplication::translate("ZipEngine","无法打开: %1").arg(inPath); return false; }
    QByteArray raw = f.readAll(); f.close();
    QByteArray comp;
    if (!deflateData(raw, level, comp)) { err = QCoreApplication::translate("ZipEngine","压缩失败"); return false; }
    QSaveFile sf(outPath);
    if (!sf.open(QIODevice::WriteOnly)) { err = QCoreApplication::translate("ZipEngine","无法写入: %1").arg(outPath); return false; }
    QByteArray hdr;
    hdr.append(char(0x1f)); hdr.append(char(0x8b)); hdr.append(char(0x08)); hdr.append(char(0x00));
    put32(hdr, 0); hdr.append(char(0x02)); hdr.append(char(0x03));
    sf.write(hdr); sf.write(comp);
    QByteArray tail; put32(tail, quint32(crc32(0, reinterpret_cast<const Bytef*>(raw.constData()), quint32(raw.size()))));
    put32(tail, quint32(raw.size() & 0xFFFFFFFF));
    sf.write(tail);
    if (progress) progress(raw.size(), raw.size(), inPath);
    return sf.commit();
}

bool gzipExtract(const QString& inPath, const QString& outDir, ProgressFn progress, CancelFn cancel, QString& err) {
    QFile f(inPath);
    if (!f.open(QIODevice::ReadOnly)) { err = QCoreApplication::translate("ZipEngine","无法打开: %1").arg(inPath); return false; }
    QByteArray all = f.readAll(); f.close();
    if (all.size() < 18 || quint8(all.at(0)) != 0x1f || quint8(all.at(1)) != 0x8b) {
        err = QCoreApplication::translate("ZipEngine","不是有效的 GZIP 文件"); return false;
    }
    QByteArray comp = all.mid(10, all.size() - 10 - 8);
    QByteArray raw;
    if (!inflateData(comp, raw)) { err = QCoreApplication::translate("ZipEngine","解压失败"); return false; }
    QString base = QFileInfo(inPath).fileName();
    if (base.endsWith(".gz")) base.chop(3);
    if (base.isEmpty()) base = "out";
    QDir(outDir).mkpath(".");
    QString outPath = safeDestPath(outDir, base);
    if (outPath.isEmpty()) outPath = outDir + "/out";
    QSaveFile sf(outPath);
    if (!sf.open(QIODevice::WriteOnly)) { err = QCoreApplication::translate("ZipEngine","无法写入: %1").arg(outPath); return false; }
    sf.write(raw); sf.commit();
    if (progress) progress(raw.size(), raw.size(), base);
    return true;
}

} // namespace

namespace ArchiveEngine {

bool compress(const CompressOptions& opt, ProgressFn progress, CancelFn cancel, QString& err) {
    if (opt.inputs.isEmpty()) { err = QCoreApplication::translate("ZipEngine","未选择任何文件或文件夹"); return false; }
    if (opt.format == "zip") {
        return zipCompress(opt.inputs, opt.archivePath, opt.level < 0 ? 6 : opt.level, progress, cancel, err);
    }
    if (opt.format == "raw" && opt.filter == "gzip") {
        if (opt.inputs.size() != 1) { err = QCoreApplication::translate("ZipEngine","GZ 仅支持单个文件"); return false; }
        return gzipCompress(opt.inputs.first(), opt.archivePath, opt.level < 0 ? 6 : opt.level, progress, cancel, err);
    }
    err = QCoreApplication::translate("ZipEngine",
        "当前本地引擎仅支持 ZIP / GZ。7Z、TAR.XZ、TAR.BZ2、TAR.ZST 等需 libarchive（见 build_deps.sh 在可联网环境编译）。");
    return false;
}

bool extract(const QString& archivePath, const QString& destDir,
             ProgressFn progress, CancelFn cancel, QString& err) {
    QFile f(archivePath);
    if (!f.open(QIODevice::ReadOnly)) { err = QCoreApplication::translate("ZipEngine","归档文件不存在: %1").arg(archivePath); return false; }
    QByteArray head = f.read(4); f.close();
    bool isGz = (head.size() >= 2 && quint8(head.at(0)) == 0x1f && quint8(head.at(1)) == 0x8b);
    if (isGz) return gzipExtract(archivePath, destDir, progress, cancel, err);
    return zipExtract(archivePath, destDir, progress, cancel, err);
}

QString detectFormatLabel(const QString& archivePath) {
    QString s = archivePath.toLower();
    if (s.endsWith(".zip")) return "ZIP";
    if (s.endsWith(".7z"))  return "7Z";
    if (s.endsWith(".gz"))  return "GZ";
    if (s.endsWith(".tar")) return "TAR";
    if (s.endsWith(".tgz")||s.endsWith(".tar.gz")) return "TAR.GZ";
    if (s.endsWith(".xz"))  return "XZ";
    if (s.endsWith(".bz2")) return "BZ2";
    if (s.endsWith(".zst")) return "ZST";
    return "未知";
}

QVector<QString> supportedFormats() {
    // 本地 zlib 引擎仅支持标准 ZIP 与单文件 GZ
    return {"ZIP", "GZ"};
}

} // namespace ArchiveEngine
