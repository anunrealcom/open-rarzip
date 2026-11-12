// sevenzipengine.cpp — IArchiveEngine backed by 7z.dll (read-only).
//
// Routes 7z / RAR / RAR5 *reading* through 7-Zip's own decoder (Format7zF), which
// gives us encrypted filenames, solid & multivolume 7z, and password-protected RAR5
// that libarchive handles less reliably. Creation/RAR-writing is intentionally
// unsupported (unRAR license + we author archives via libarchive/zlib instead).

#include "iengine.h"
#include "sevenzip/sevenzip_wrap.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <QFile>

#include <mutex>

namespace yasuo {

namespace {
// 7z: "7z\xBC\xAF\x27\x1C" ; RAR4: "Rar!\x1A\x07" ; RAR5: "Rar!\x1A\x07\x01\x00"
Format detectByMagic(const QString& archivePath) {
    QFile f(archivePath);
    if (!f.open(QIODevice::ReadOnly))
        return Format::Unknown;
    char hdr[8] = {0};
    qint64 n = f.read(hdr, sizeof(hdr));
    f.close();
    if (n >= 6 && hdr[0] == '7' && hdr[1] == 'z' && (unsigned char)hdr[2] == 0xBC &&
        (unsigned char)hdr[3] == 0xAF && (unsigned char)hdr[4] == 0x27 &&
        (unsigned char)hdr[5] == 0x1C) {
        return Format::SevenZip;
    }
    if (n >= 7 && hdr[0] == 'R' && hdr[1] == 'a' && hdr[2] == 'r' && hdr[3] == '!' &&
        (unsigned char)hdr[4] == 0x1A && (unsigned char)hdr[5] == 0x07) {
        return Format::Rar; // covers RAR4 and RAR5 (7z.dll distinguishes internally)
    }
    return Format::Unknown;
}

// 1601-01-01 epoch offset (FILETIME) expressed in milliseconds since 1970-01-01.
constexpr qint64 FILETIME_TO_UNIX_MS = 11644473600000LL;

QDateTime fileTimeToQDateTime(uint64_t ft) {
    if (ft == 0) return QDateTime();
    qint64 ms = (qint64)(ft / 10000) - FILETIME_TO_UNIX_MS;
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC);
}

bool ensureLoaded() {
    static std::once_flag s_flag;
    static bool s_ok = false;
    std::call_once(s_flag, []() {
        QString dir = QCoreApplication::applicationDirPath();
        s_ok = sevenzip::szInit(dir.toUtf8().constData());
    });
    return s_ok;
}
} // namespace

class SevenZipEngine : public IArchiveEngine {
public:
    EngineCaps caps() const override {
        EngineCaps c;
        c.id = QStringLiteral("7zip");
        c.displayName = QStringLiteral("7-Zip SDK（仅读取）");
        c.readFormats = { Format::SevenZip, Format::Rar };
        c.rarRead = true;
        c.encryptedRarRead = true;   // 7z.dll decodes password-protected RAR5
        c.solid = true;              // 7z 固实
        c.volumes = true;            // 7z / RAR 分卷
        c.encryption = false;        // 本引擎只读取，不生成加密归档
        c.editable = false;          // 不支持包内删除
        c.notes = QStringLiteral("通过 7z.dll 读取 7z/RAR（含加密文件名、固实、分卷、加密 RAR5）。"
                                 "创建归档请使用主引擎。");
        return c;
    }

    bool list(const QString& archivePath, const QString& password,
              ArchiveInfo& out, QString& err) override {
        out = ArchiveInfo();
        out.path = archivePath;
        out.format = detect(archivePath);
        out.formatName = formatName(out.format);

        if (!ensureLoaded()) {
            err = QStringLiteral("未能加载 7z.dll（应与可执行文件同目录）");
            return false;
        }
        std::vector<sevenzip::SzEntry> entries;
        std::wstring werr;
        if (!sevenzip::szList(archivePath.toStdWString(), password.toStdWString(),
                              entries, werr)) {
            err = QString::fromWCharArray(werr.c_str());
            return false;
        }
        out.entryCount = (int)entries.size();
        for (const auto& e : entries) {
            EntryInfo ei;
            ei.path = QString::fromWCharArray(e.path.c_str()).replace(QLatin1Char('\\'), QLatin1Char('/'));
            ei.size = (qint64)e.size;
            ei.compressedSize = (qint64)e.packedSize;
            ei.isDir = e.isDir;
            ei.encrypted = e.encrypted;
            ei.modified = fileTimeToQDateTime(e.mtime);
            out.entries.append(ei);
            out.totalSize += ei.size;
            out.totalCompressed += ei.compressedSize;
        }
        return true;
    }

    bool compress(const CompressOptions&, ProgressFn, CancelFn, QString& err) override {
        err = QStringLiteral("7-Zip 后端为只读；创建归档请使用主引擎（libarchive / 内置）");
        return false;
    }

    bool extract(const ExtractOptions& opt, ProgressFn, CancelFn, QString& err) override {
        if (!ensureLoaded()) {
            err = QStringLiteral("未能加载 7z.dll（应与可执行文件同目录）");
            return false;
        }
        std::wstring werr;
        // 7z.dll 解压全部条目；部分条目选择由上层在调用前筛选（当前后端解压全集）。
        if (!sevenzip::szExtract(opt.archivePath.toStdWString(),
                                 opt.destDir.toStdWString(),
                                 opt.password.toStdWString(), werr)) {
            err = QString::fromWCharArray(werr.c_str());
            return false;
        }
        return true;
    }

    bool test(const QString& archivePath, const QString& password,
              ProgressFn, CancelFn, QString& err) override {
        if (!ensureLoaded()) {
            err = QStringLiteral("未能加载 7z.dll（应与可执行文件同目录）");
            return false;
        }
        std::wstring werr;
        if (!sevenzip::szTest(archivePath.toStdWString(), password.toStdWString(), werr)) {
            err = QString::fromWCharArray(werr.c_str());
            return false;
        }
        return true;
    }

    Format detect(const QString& archivePath) const override {
        return detectByMagic(archivePath);
    }
};

IArchiveEngine* createSevenZipEngine() {
    static SevenZipEngine engine;
    return &engine;
}

} // namespace yasuo
