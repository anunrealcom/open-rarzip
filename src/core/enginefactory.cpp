#include "iengine.h"
#include "accelerator.h"

#include <QCoreApplication>
#include <QFile>

#include <cstdint>

namespace yasuo {

#ifdef YASUO_HAVE_LIBARCHIVE
IArchiveEngine* createLibArchiveEngine();
#endif
#ifdef YASUO_HAVE_SEVENZIP
IArchiveEngine* createSevenZipEngine();
#endif
IArchiveEngine* createZlibEngine();

namespace {

bool haveLibArchive() {
#ifdef YASUO_HAVE_LIBARCHIVE
    return true;
#else
    return false;
#endif
}

bool haveSevenZip() {
#ifdef YASUO_HAVE_SEVENZIP
    return true;
#else
    return false;
#endif
}

// 轻量魔数探测：7z / RAR 优先交由 7-Zip 后端读取。
bool needsSevenZip(const QString& archivePath) {
    if (!haveSevenZip()) return false;
    QFile f(archivePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    unsigned char hdr[8] = {0};
    qint64 n = f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    f.close();
    if (n >= 6 && hdr[0] == '7' && hdr[1] == 'z' &&
        hdr[2] == 0xBC && hdr[3] == 0xAF && hdr[4] == 0x27 && hdr[5] == 0x1C)
        return true;
    if (n >= 7 && hdr[0] == 'R' && hdr[1] == 'a' && hdr[2] == 'r' && hdr[3] == '!' &&
        hdr[4] == 0x1A && hdr[5] == 0x07)
        return true;
    return false;
}

} // namespace

IArchiveEngine* Engines::best() {
    static IArchiveEngine* engine = nullptr;
    if (!engine) {
#ifdef YASUO_HAVE_LIBARCHIVE
        engine = createLibArchiveEngine();
#elif defined(YASUO_HAVE_SEVENZIP)
        engine = createSevenZipEngine();
#else
        engine = createZlibEngine();
#endif
    }
    return engine;
}

IArchiveEngine* Engines::best(const QString& archivePath) {
    if (needsSevenZip(archivePath))
        return createSevenZipEngine();
    return best();
}

QVector<EngineCaps> Engines::availableCaps() {
    QVector<EngineCaps> list;
    list.append(best()->caps());
    if (haveLibArchive()) {
        EngineCaps fallback = createZlibEngine()->caps();
        fallback.displayName += QStringLiteral("（备用）");
        list.append(fallback);
    }
    if (haveSevenZip()) {
        EngineCaps sz = createSevenZipEngine()->caps();
        sz.displayName += QStringLiteral("（只读）");
        list.append(sz);
    }
    return list;
}

QStringList Engines::writableFormatLabels() {
    QStringList labels;
    const EngineCaps c = best()->caps();
    for (Format f : c.writeFormats)
        labels.append(formatName(f));
    return labels;
}

QString Engines::summary() {
    const EngineCaps c = best()->caps();
    QStringList lines;
    lines << QStringLiteral("当前引擎：%1").arg(c.displayName);
    lines << QStringLiteral("可写格式：%1 种").arg(c.writeFormats.size());
    lines << QStringLiteral("RAR 解压：%1").arg(c.rarRead ? QStringLiteral("支持（只读）") : QStringLiteral("不支持"));
    lines << QStringLiteral("加密归档：%1").arg(c.encryption ? QStringLiteral("ZIP AES-256 / AES-128 / ZipCrypto") : QStringLiteral("不支持"));
    lines << QStringLiteral("多线程编码：%1").arg(c.multiThread ? QStringLiteral("已启用") : QStringLiteral("未启用"));
    lines << QStringLiteral("加速后端：%1").arg(Accelerator::statusText());
    if (haveSevenZip())
        lines << QStringLiteral("7-Zip 后端：已启用（7z / RAR 读取，含加密文件名、固实、分卷）");
    if (!c.notes.isEmpty()) lines << c.notes;
    return lines.join(QLatin1Char('\n'));
}

} // namespace yasuo
