#include "iengine.h"
#include "accelerator.h"

#include <QCoreApplication>

namespace yasuo {

#ifdef YASUO_HAVE_LIBARCHIVE
IArchiveEngine* createLibArchiveEngine();
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

} // namespace

IArchiveEngine* Engines::best() {
    static IArchiveEngine* engine = nullptr;
    if (!engine) {
#ifdef YASUO_HAVE_LIBARCHIVE
        engine = createLibArchiveEngine();
#else
        engine = createZlibEngine();
#endif
    }
    return engine;
}

QVector<EngineCaps> Engines::availableCaps() {
    QVector<EngineCaps> list;
    list.append(best()->caps());
    if (haveLibArchive()) {
        EngineCaps fallback = createZlibEngine()->caps();
        fallback.displayName += QStringLiteral("（备用）");
        list.append(fallback);
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
    if (!c.notes.isEmpty()) lines << c.notes;
    return lines.join(QLatin1Char('\n'));
}

} // namespace yasuo
