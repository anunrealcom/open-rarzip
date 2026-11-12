#include "iengine.h"
#include "pathguard.h"
#include "accelerator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QProcess>
#include <QTextStream>
#include <QElapsedTimer>
#include <zlib.h>
#include <algorithm>

#include "sevenzip/sevenzip_wrap.h"

using namespace yasuo;

namespace yasuo {
IArchiveEngine* createZlibEngine();
IArchiveEngine* createSevenZipEngine();
}

namespace {

QTextStream out(stdout);
int passed = 0, failed = 0;

void check(const QString& name, bool ok, const QString& detail = QString()) {
    if (ok) { ++passed; out << "[PASS] " << name << "\n"; }
    else { ++failed; out << "[FAIL] " << name;
           if (!detail.isEmpty()) out << " -- " << detail;
           out << "\n"; }
    out.flush();
}

bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(data) == data.size();
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

QString sha(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
}

QString makeSample(const QString& root) {
    QDir().mkpath(root + QStringLiteral("/sub"));
    QDir().mkpath(root + QStringLiteral("/空目录"));
    writeFile(root + QStringLiteral("/a.txt"), QByteArray(4096, 'A') + "hello zip\n");
    writeFile(root + QStringLiteral("/中文 文件名.txt"), "中文内容测试\n");
    writeFile(root + QStringLiteral("/sub/b.txt"), "second file content\n");
    writeFile(root + QStringLiteral("/empty.txt"), QByteArray());
    return root;
}

bool runCmd(const QString& program, const QStringList& args) {
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.start();
    if (!p.waitForStarted(10000)) {
        out << "       [cmd] 启动失败: " << program << "\n";
        return false;
    }
    if (!p.waitForFinished(60000)) {
        out << "       [cmd] 超时: " << program << "\n";
        return false;
    }
    const int code = p.exitCode();
    if (code != 0)
        out << "       [cmd] " << program << " exit=" << code
            << " stderr=" << QString::fromLocal8Bit(p.readAllStandardError()).trimmed() << "\n";
    return code == 0;
}

void testPathGuard() {
    QString norm, why;
    check(QStringLiteral("安全: 拒绝 ../ 越界"),
          !PathGuard::sanitize(QStringLiteral("../output/evil.txt"), norm, why));
    check(QStringLiteral("安全: 拒绝绝对路径"),
          !PathGuard::sanitize(QStringLiteral("/etc/passwd"), norm, why));
    check(QStringLiteral("安全: 拒绝盘符路径"),
          !PathGuard::sanitize(QStringLiteral("C:/Windows/system32/x"), norm, why));
    check(QStringLiteral("安全: 拒绝嵌套 .."),
          !PathGuard::sanitize(QStringLiteral("a/b/../../x"), norm, why));
    check(QStringLiteral("安全: 拒绝保留设备名"),
          !PathGuard::sanitize(QStringLiteral("CON.txt"), norm, why));

    QTemporaryDir tmp;
    QString dest, reason;
    const QString base = tmp.path() + QStringLiteral("/out");
    bool ok = PathGuard::resolve(base, QStringLiteral("dir/file.txt"), dest, reason);
    check(QStringLiteral("安全: 正常路径落在目标目录内"),
          ok && dest.startsWith(QDir::cleanPath(base) + QStringLiteral("/")));

    // 旧实现的前缀漏洞：目标 /tmp/out，恶意条目 ../output/x 曾被误判为合法
    const QString evil = tmp.path() + QStringLiteral("/output/x.txt");
    check(QStringLiteral("安全: 前缀相似目录不越界"),
          !PathGuard::resolve(base, QStringLiteral("../output/x.txt"), dest, reason) ||
          dest != evil);
}

void testOverwritePolicies() {
    QTemporaryDir tmp;
    const QString p = tmp.path() + QStringLiteral("/same.txt");
    writeFile(p, QByteArray("old content"));
    const QDateTime local = QFileInfo(p).lastModified();
    bool skip = false;

    PathGuard::decide(p, OverwritePolicy::OverwriteIfNewer, skip, local.addSecs(-3600));
    check(QStringLiteral("覆盖策略: 归档较旧时跳过"), skip);
    skip = true;
    PathGuard::decide(p, OverwritePolicy::OverwriteIfNewer, skip, local.addSecs(3600));
    check(QStringLiteral("覆盖策略: 归档较新时覆盖"), !skip);
    skip = false;
    PathGuard::decide(p, OverwritePolicy::OverwriteIfNewer, skip, QDateTime());
    check(QStringLiteral("覆盖策略: 时间缺失时保守跳过"), skip);

    skip = false;
    PathGuard::decide(p, OverwritePolicy::Skip, skip);
    check(QStringLiteral("覆盖策略: 跳过策略生效"), skip);
    skip = true;
    PathGuard::decide(p, OverwritePolicy::Overwrite, skip);
    check(QStringLiteral("覆盖策略: 覆盖策略生效"), !skip);
    const QString renamed = PathGuard::decide(p, OverwritePolicy::Rename, skip);
    check(QStringLiteral("覆盖策略: 自动改名生成可用路径"),
          !skip && renamed != p && !QFileInfo::exists(renamed), renamed);
}

void testChecksum(const QString& bigFile) {
    // 基准：zlib 顺序流式 CRC32
    QFile f(bigFile);
    if (!f.open(QIODevice::ReadOnly)) {
        check(QStringLiteral("加速: 打开基准文件"), false, bigFile);
        return;
    }
    quint32 expected = crc32(0L, Z_NULL, 0);
    QByteArray buf(64 * 1024, Qt::Uninitialized);
    qint64 n;
    while ((n = f.read(buf.data(), buf.size())) > 0)
        expected = crc32(expected, reinterpret_cast<const Bytef*>(buf.constData()),
                         static_cast<uInt>(n));
    f.close();

    bool ok = false;
    const quint32 single = Accelerator::parallelCrc32(bigFile, 1, {}, ok);
    const quint32 multi = Accelerator::parallelCrc32(bigFile, 4, {}, ok);
    check(QStringLiteral("加速: 并行 CRC 与标准 CRC32 一致"),
          ok && single == expected && multi == expected,
          QStringLiteral("标准 %1 / 单线程 %2 / 并行 %3").arg(expected).arg(single).arg(multi));
}

void testRoundTrip(IArchiveEngine* engine, const QString& tag, const QString& src) {
    QTemporaryDir tmp;
    if (!tmp.isValid()) return;
    const QString root = QFileInfo(src).fileName();

    CompressOptions co;
    co.archivePath = tmp.path() + QStringLiteral("/round_") + tag + QStringLiteral(".zip");
    co.inputs = QStringList{ src };
    co.format = Format::Zip;
    co.level = 6;
    QString err;
    const bool compressed = engine->compress(co, {}, {}, err);
    check(QStringLiteral("%1: ZIP 压缩").arg(tag), compressed, err);
    if (!compressed) return;

    ArchiveInfo info;
    const bool listed = engine->list(co.archivePath, {}, info, err);
    check(QStringLiteral("%1: 列出条目与中文名").arg(tag),
          listed && info.entryCount >= 5 &&
          std::any_of(info.entries.begin(), info.entries.end(),
                      [](const EntryInfo& e) { return e.path.contains(QStringLiteral("中文")); }),
          err);

    ExtractOptions eo;
    eo.archivePath = co.archivePath;
    eo.destDir = tmp.path() + QStringLiteral("/un");
    eo.overwrite = OverwritePolicy::Overwrite;
    eo.verifyCrc = true;
    const bool extracted = engine->extract(eo, {}, {}, err);
    check(QStringLiteral("%1: ZIP 解压").arg(tag), extracted, err);
    if (!extracted) return;

    const QString outRoot = eo.destDir + QLatin1Char('/') + root;
    const bool same = readFile(outRoot + QStringLiteral("/a.txt")) ==
                          readFile(src + QStringLiteral("/a.txt")) &&
                      readFile(outRoot + QStringLiteral("/中文 文件名.txt")) ==
                          readFile(src + QStringLiteral("/中文 文件名.txt")) &&
                      QFile::exists(outRoot + QStringLiteral("/empty.txt"));
    check(QStringLiteral("%1: 内容逐字节一致").arg(tag), same);

    const bool tested = engine->test(co.archivePath, {}, {}, {}, err);
    check(QStringLiteral("%1: 完整性测试").arg(tag), tested, err);
}

void testEncryption(IArchiveEngine* engine, const QString& src) {
    if (!engine->caps().encryption) {
        out << "[SKIP] 当前引擎不支持加密\n";
        return;
    }
    QTemporaryDir tmp;
    CompressOptions co;
    co.archivePath = tmp.path() + QStringLiteral("/secret.zip");
    co.inputs = QStringList{ src };
    co.format = Format::Zip;
    co.password = QStringLiteral("Yasuo-Test-2026");
    co.encryption = Encryption::Aes256;
    QString err;
    const bool ok = engine->compress(co, {}, {}, err);
    check(QStringLiteral("加密: 创建 AES-256 ZIP"), ok, err);
    if (!ok) return;

    ArchiveInfo info;
    const bool listed = engine->list(co.archivePath, co.password, info, err);
    check(QStringLiteral("加密: 带密码可列出条目"), listed && info.entryCount > 0, err);
    // 必须真的加密：曾经因为压缩选项被静默丢弃，生成的是明文 ZIP 却标称 AES-256。
    check(QStringLiteral("加密: 归档被识别为加密"), info.encrypted);

    ArchiveInfo noPassInfo;
    err.clear();
    const bool listedNoPass = engine->list(co.archivePath, {}, noPassInfo, err);
    check(QStringLiteral("加密: 无密码可浏览文件名"), listedNoPass &&
          noPassInfo.entryCount > 0 && noPassInfo.encrypted, err);

    err.clear();
    check(QStringLiteral("加密: 带密码完整性测试通过"),
          engine->test(co.archivePath, co.password, {}, {}, err), err);

    ExtractOptions bad;
    bad.archivePath = co.archivePath;
    bad.destDir = tmp.path() + QStringLiteral("/bad");
    err.clear();
    const bool badOk = engine->extract(bad, {}, {}, err);
    check(QStringLiteral("加密: 无密码解压被拒绝"), !badOk && !err.isEmpty(), err);
    const QString root = QFileInfo(src).fileName();
    check(QStringLiteral("加密: 无密码未留下明文副本"),
          readFile(bad.destDir + QLatin1Char('/') + root + QStringLiteral("/a.txt")) !=
              readFile(src + QStringLiteral("/a.txt")));

    err.clear();
    check(QStringLiteral("加密: 无密码完整性测试被拒绝"),
          !engine->test(co.archivePath, {}, {}, {}, err));

    ExtractOptions wrong;
    wrong.archivePath = co.archivePath;
    wrong.destDir = tmp.path() + QStringLiteral("/wrong");
    wrong.password = QStringLiteral("not-the-password");
    err.clear();
    check(QStringLiteral("加密: 错误密码被拒绝"),
          !engine->extract(wrong, {}, {}, err), err);

    ExtractOptions good;
    good.archivePath = co.archivePath;
    good.destDir = tmp.path() + QStringLiteral("/good");
    good.password = co.password;
    good.overwrite = OverwritePolicy::Overwrite;
    err.clear();
    const bool goodOk = engine->extract(good, {}, {}, err);
    check(QStringLiteral("加密: 正确密码解压成功"), goodOk, err);
    if (goodOk) {
        check(QStringLiteral("加密: 解压内容一致"),
              readFile(good.destDir + QLatin1Char('/') + root + QStringLiteral("/a.txt")) ==
                  readFile(src + QStringLiteral("/a.txt")));
    }
}

void testFormats(IArchiveEngine* engine, const QString& src) {
    if (!engine->caps().rarRead) return;
    struct Case { Format f; QString suffix; };
    const QVector<Case> cases = {
        { Format::SevenZip, QStringLiteral("7z") },
        { Format::TarGz,    QStringLiteral("tar.gz") },
        { Format::TarXz,    QStringLiteral("tar.xz") },
        { Format::TarZst,   QStringLiteral("tar.zst") },
        { Format::Tar,      QStringLiteral("tar") }
    };
    QTemporaryDir tmp;
    for (const Case& c : cases) {
        CompressOptions co;
        co.archivePath = tmp.path() + QStringLiteral("/f.") + c.suffix;
        co.inputs = QStringList{ src };
        co.format = c.f;
        QString err;
        const bool ok = engine->compress(co, {}, {}, err);
        ArchiveInfo info;
        const bool listed = ok && engine->list(co.archivePath, {}, info, err);
        check(QStringLiteral("格式: %1 写入并读回").arg(formatName(c.f)),
              ok && listed && info.entryCount > 0, err);
    }
    // RAR 只能解压、不能生成，必须有明确拒绝
    CompressOptions rar;
    rar.archivePath = tmp.path() + QStringLiteral("/x.rar");
    rar.inputs = QStringList{ src };
    rar.format = Format::Rar;
    QString err;
    check(QStringLiteral("合规: 拒绝生成 RAR"),
          !engine->compress(rar, {}, {}, err) && err.contains(QStringLiteral("专有")));
}

void testRemoveEntries(IArchiveEngine* engine, const QString& src) {
    if (!engine->caps().editable) {
        out << "[SKIP] 当前引擎不支持包内删除\n";
        return;
    }
    QTemporaryDir tmp;
    const QString root = QFileInfo(src).fileName();
    CompressOptions co;
    co.archivePath = tmp.path() + QStringLiteral("/edit.zip");
    co.inputs = QStringList{ src };
    co.format = Format::Zip;
    QString err;
    const bool made = engine->compress(co, {}, {}, err);
    check(QStringLiteral("包内编辑: 建立测试归档"), made, err);
    if (!made) return;

    ArchiveInfo before;
    err.clear();
    check(QStringLiteral("包内编辑: 删除前可列出"),
          engine->list(co.archivePath, {}, before, err) && before.entryCount > 0, err);

    const QString victim = root + QStringLiteral("/a.txt");
    err.clear();
    check(QStringLiteral("包内编辑: 删除单个条目"),
          engine->removeEntries(co.archivePath, QStringList{ victim }, {}, err), err);

    ArchiveInfo after;
    err.clear();
    const bool listed = engine->list(co.archivePath, {}, after, err);
    bool gone = true;
    for (const EntryInfo& e : after.entries)
        if (e.path == victim || e.path == victim + QLatin1Char('/')) gone = false;
    check(QStringLiteral("包内编辑: 删除后条目消失"), listed && gone, err);
    check(QStringLiteral("包内编辑: 其余条目数量正确"),
          after.entryCount == before.entryCount - 1);

    ExtractOptions eo;
    eo.archivePath = co.archivePath;
    eo.destDir = tmp.path() + QStringLiteral("/after");
    eo.overwrite = OverwritePolicy::Overwrite;
    err.clear();
    check(QStringLiteral("包内编辑: 删除后仍可解压"),
          engine->extract(eo, {}, {}, err), err);
    check(QStringLiteral("包内编辑: 剩余内容完整"),
          readFile(eo.destDir + QLatin1Char('/') + root + QStringLiteral("/中文 文件名.txt")) ==
              readFile(src + QStringLiteral("/中文 文件名.txt")));

    err.clear();
    check(QStringLiteral("包内编辑: 删除目录级联"),
          engine->removeEntries(co.archivePath, QStringList{ root + QStringLiteral("/sub/") },
                                {}, err), err);
    ArchiveInfo after2;
    err.clear();
    engine->list(co.archivePath, {}, after2, err);
    bool subGone = true;
    for (const EntryInfo& e : after2.entries)
        if (e.path.startsWith(root + QStringLiteral("/sub"))) subGone = false;
    check(QStringLiteral("包内编辑: 目录下条目全部移除"), subGone);

    err.clear();
    check(QStringLiteral("包内编辑: 拒绝删除不存在条目"),
          !engine->removeEntries(co.archivePath, QStringList{ QStringLiteral("nope.txt") },
                                 {}, err), err);
    ArchiveInfo after3;
    engine->list(co.archivePath, {}, after3, err);
    check(QStringLiteral("包内编辑: 失败后归档未改变"),
          after3.entryCount == after2.entryCount);
    err.clear();
    check(QStringLiteral("包内编辑: 拒绝空删除列表"),
          !engine->removeEntries(co.archivePath, {}, {}, err), err);

    if (engine->caps().encryption) {
        CompressOptions sec;
        sec.archivePath = tmp.path() + QStringLiteral("/edit-secret.zip");
        sec.inputs = QStringList{ src };
        sec.format = Format::Zip;
        sec.password = QStringLiteral("edit-pass-2026");
        sec.encryption = Encryption::Aes256;
        err.clear();
        if (engine->compress(sec, {}, {}, err)) {
            err.clear();
            check(QStringLiteral("包内编辑: 拒绝加密归档删除"),
                  !engine->removeEntries(sec.archivePath, QStringList{ victim },
                                         sec.password, err), err);
        } else {
            check(QStringLiteral("包内编辑: 建立加密测试归档"), false, err);
        }
    }
#ifdef Q_OS_WIN
    const QString bsdtar = QDir::fromNativeSeparators(
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")) +
        QStringLiteral("/System32/tar.exe"));
    check(QStringLiteral("包内编辑: bsdtar 仍可读取改写后的归档"),
          runCmd(bsdtar, { QStringLiteral("-tf"), co.archivePath }));
#endif
}

void put16le(QByteArray& b, quint16 v) {
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
}
void put32le(QByteArray& b, quint32 v) {
    for (int i = 0; i < 4; ++i) b.append(char((v >> (8 * i)) & 0xFF));
}

// 手工构造「外部工具生成」的 ZIP：条目名是 UTF-8 字节且置 general purpose bit11。
// 未启用 iconv 的 libarchive 对这类条目 archive_entry_pathname() 可能返回 NULL，
// 只能靠 pathname_utf8 取回名字；曾因此静默丢弃中文文件（返回成功码但不落盘）。
QByteArray externalUtf8Zip() {
    struct E { QByteArray name, data; quint32 crc = 0, off = 0; };
    QVector<E> ents = {
        { QStringLiteral("中文/说明.txt").toUtf8(), QByteArray("chinese content\n"), 0, 0 },
        { QByteArray("plain.txt"), QByteArray("ascii content\n"), 0, 0 }
    };
    QByteArray out;
    for (E& e : ents) {
        e.crc = static_cast<quint32>(crc32(
            0L, reinterpret_cast<const Bytef*>(e.data.constData()),
            static_cast<uInt>(e.data.size())));
        e.off = static_cast<quint32>(out.size());
        put32le(out, 0x04034b50u);
        put16le(out, 20); put16le(out, 0x0800); put16le(out, 0); // UTF-8 名 + store
        put16le(out, 0); put16le(out, 0);
        put32le(out, e.crc);
        put32le(out, static_cast<quint32>(e.data.size()));
        put32le(out, static_cast<quint32>(e.data.size()));
        put16le(out, static_cast<quint16>(e.name.size())); put16le(out, 0);
        out.append(e.name);
        out.append(e.data);
    }
    const quint32 cdOff = static_cast<quint32>(out.size());
    for (const E& e : ents) {
        put32le(out, 0x02014b50u);
        put16le(out, 20); put16le(out, 20);
        put16le(out, 0x0800); put16le(out, 0);
        put16le(out, 0); put16le(out, 0);
        put32le(out, e.crc);
        put32le(out, static_cast<quint32>(e.data.size()));
        put32le(out, static_cast<quint32>(e.data.size()));
        put16le(out, static_cast<quint16>(e.name.size()));
        put16le(out, 0); put16le(out, 0);
        put16le(out, 0); put16le(out, 0);
        put32le(out, 0); put32le(out, e.off);
        out.append(e.name);
    }
    const quint32 cdSize = static_cast<quint32>(out.size()) - cdOff;
    put32le(out, 0x06054b50u);
    put16le(out, 0); put16le(out, 0);
    put16le(out, static_cast<quint16>(ents.size()));
    put16le(out, static_cast<quint16>(ents.size()));
    put32le(out, cdSize); put32le(out, cdOff);
    put16le(out, 0);
    return out;
}

void testExternalUtf8Names(IArchiveEngine* engine, const QString& tag) {
    QTemporaryDir tmp;
    const QString path = tmp.path() + QStringLiteral("/external-utf8.zip");
    const QString cn = QStringLiteral("中文/说明.txt");
    const QByteArray cnData("chinese content\n");
    const QByteArray asciiData("ascii content\n");
    if (!writeFile(path, externalUtf8Zip())) {
        check(QStringLiteral("外部 UTF-8(%1): 构造样本").arg(tag), false);
        return;
    }
    ArchiveInfo info;
    QString err;
    const bool listed = engine->list(path, {}, info, err);
    bool nameOk = false;
    for (const EntryInfo& e : info.entries)
        if (e.path == cn) nameOk = true;
    check(QStringLiteral("外部 UTF-8(%1): list 取回中文条目名").arg(tag),
          listed && nameOk, err);
    check(QStringLiteral("外部 UTF-8(%1): 条目数正确").arg(tag), info.entryCount == 2);

    ExtractOptions eo;
    eo.archivePath = path;
    eo.destDir = tmp.path() + QStringLiteral("/out");
    eo.overwrite = OverwritePolicy::Overwrite;
    err.clear();
    const bool ok = engine->extract(eo, {}, {}, err);
    check(QStringLiteral("外部 UTF-8(%1): 解压成功").arg(tag), ok, err);
    check(QStringLiteral("外部 UTF-8(%1): 中文文件未被静默丢弃").arg(tag),
          readFile(eo.destDir + QLatin1Char('/') + cn) == cnData);
    check(QStringLiteral("外部 UTF-8(%1): ASCII 文件完整").arg(tag),
          readFile(eo.destDir + QStringLiteral("/plain.txt")) == asciiData);
}

void testCrossTools(IArchiveEngine* engine, const QString& tag) {
#ifdef Q_OS_WIN
    // 用纯 ASCII 文件名做第三方交叉验证：Windows 自带 bsdtar 对 ZIP 内的
    // 中文 UTF-8 名存在编码兼容问题，中文名已由上面的自往返测试单独覆盖。
    QTemporaryDir tmp;
    const QString src = tmp.path() + QStringLiteral("/ascii");
    QDir().mkpath(src + QStringLiteral("/sub"));
    writeFile(src + QStringLiteral("/a.txt"), QByteArray(1024, 'A') + "hello\n");
    writeFile(src + QStringLiteral("/sub/b.txt"), "second file\n");
    CompressOptions co;
    co.archivePath = tmp.path() + QStringLiteral("/cross.zip");
    co.inputs = QStringList{ src };
    co.format = Format::Zip;
    QString err;
    const bool made = engine->compress(co, {}, {}, err);
    check(QStringLiteral("兼容(%1): 生成交叉验证 ZIP").arg(tag), made, err);
    if (!made) return;

    const QString dest = tmp.path() + QStringLiteral("/tar");
    QDir().mkpath(dest);
    // 必须用 Windows 自带 bsdtar；Git for Windows 的 GNU tar 不认 ZIP
    const QString bsdtar = QDir::fromNativeSeparators(
        qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")) +
        QStringLiteral("/System32/tar.exe"));
    const bool listed = runCmd(bsdtar, { QStringLiteral("-tf"), co.archivePath });
    const bool extracted = runCmd(bsdtar,
                                  { QStringLiteral("-xf"), co.archivePath, QStringLiteral("-C"), dest });
    const bool same = QFile::exists(dest + QStringLiteral("/ascii/a.txt")) &&
                      readFile(dest + QStringLiteral("/ascii/a.txt")) ==
                          readFile(src + QStringLiteral("/a.txt"));
    check(QStringLiteral("兼容(%1): 系统 bsdtar 可读取并还原产出 ZIP").arg(tag),
          listed && extracted && same);
#else
    Q_UNUSED(engine); Q_UNUSED(tag);
    out << "[SKIP] 外部工具交叉验证仅在 Windows 运行\n";
#endif
}

void testLargeFile(IArchiveEngine* engine) {
    QTemporaryDir tmp;
    QByteArray payload(8 * 1024 * 1024, Qt::Uninitialized);
    for (int i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<char>((i * 31 + (i >> 8)) & 0xFF);
    const QString src = tmp.path() + QStringLiteral("/big.bin");
    writeFile(src, payload);
    testChecksum(src);

    CompressOptions co;
    co.archivePath = tmp.path() + QStringLiteral("/big.zip");
    co.inputs = QStringList{ src };
    co.format = Format::Zip;
    QString err;
    QElapsedTimer t; t.start();
    const bool ok = engine->compress(co, {}, {}, err);
    check(QStringLiteral("大文件: 8MB 流式压缩"), ok, err);
    if (!ok) return;
    ExtractOptions eo;
    eo.archivePath = co.archivePath;
    eo.destDir = tmp.path() + QStringLiteral("/bigout");
    eo.overwrite = OverwritePolicy::Overwrite;
    const bool ok2 = engine->extract(eo, {}, {}, err);
    check(QStringLiteral("大文件: 解压后 SHA1 一致"),
          ok2 && sha(readFile(eo.destDir + QStringLiteral("/big.bin"))) == sha(payload), err);
    out << "       耗时 " << t.elapsed() << " ms\n";
}

// 7-Zip 只读后端端到端验证：用 szCreate7z（经 7z.dll 的 IOutArchive）造真实 7z
// 样本，再经 SevenZipEngine（或按魔数分派的引擎）读回，覆盖 list/extract/test
// 以及「加密内容 + 加密文件名」场景。直接驱动 7z.dll 能证明后端不是空壳。
void testSevenZipBackend() {
    // szCreate7z 直接调用 g_lib.createObject，必须先加载 7z.dll（随可执行文件部署）。
    if (!sevenzip::szInit(QCoreApplication::applicationDirPath().toUtf8().constData())) {
        out << "[SKIP] 7z.dll 不可用，跳过 7-Zip 后端端到端测试\n";
        return;
    }

    QTemporaryDir tmp;
    const QString srcDir = tmp.path() + QStringLiteral("/src");
    QDir().mkpath(srcDir + QStringLiteral("/sub"));
    const QByteArray fileA(4096, 'A');
    const QByteArray cnData("中文内容测试\n");
    const QByteArray bData("second file content\n");
    writeFile(srcDir + QStringLiteral("/a.txt"), fileA);
    writeFile(srcDir + QStringLiteral("/中文 文件名.txt"), cnData);
    writeFile(srcDir + QStringLiteral("/sub/b.txt"), bData);

    std::vector<sevenzip::SzCreateItem> items;
    items.push_back({ L"a.txt", (srcDir + QStringLiteral("/a.txt")).toStdWString(), false, (uint64_t)fileA.size() });
    items.push_back({ L"中文 文件名.txt", (srcDir + QStringLiteral("/中文 文件名.txt")).toStdWString(), false, (uint64_t)cnData.size() });
    items.push_back({ L"sub/b.txt", (srcDir + QStringLiteral("/sub/b.txt")).toStdWString(), false, (uint64_t)bData.size() });

    // ---- 明文 7z ----
    const QString plain7z = tmp.path() + QStringLiteral("/plain.7z");
    std::wstring werr;
    check(QStringLiteral("7-Zip 后端: szCreate7z 生成明文 7z"),
          sevenzip::szCreate7z(items, plain7z.toStdWString(), L"", false, werr),
          QString::fromWCharArray(werr.c_str()));
    if (!QFile::exists(plain7z)) return;

    IArchiveEngine* sz = createSevenZipEngine();
    check(QStringLiteral("7-Zip 后端: 按魔数分派到 7-Zip 引擎"),
          Engines::best(plain7z)->caps().id == QStringLiteral("7zip"));

    {
        ArchiveInfo info; QString err;
        check(QStringLiteral("7-Zip 后端: 列出明文 7z"),
              sz->list(plain7z, QString(), info, err) && info.entryCount == 3, err);
        bool hasCn = false;
        for (const EntryInfo& e : info.entries)
            if (e.path.contains(QStringLiteral("中文"))) hasCn = true;
        check(QStringLiteral("7-Zip 后端: 中文条目名可读取"), hasCn);
    }
    {
        ExtractOptions eo; eo.archivePath = plain7z;
        eo.destDir = tmp.path() + QStringLiteral("/un_plain");
        eo.overwrite = OverwritePolicy::Overwrite;
        QString err;
        check(QStringLiteral("7-Zip 后端: 解压明文 7z"), sz->extract(eo, {}, {}, err), err);
        check(QStringLiteral("7-Zip 后端: 解压内容一致(a.txt)"),
              readFile(eo.destDir + QStringLiteral("/a.txt")) == fileA);
        check(QStringLiteral("7-Zip 后端: 解压内容一致(中文)"),
              readFile(eo.destDir + QStringLiteral("/中文 文件名.txt")) == cnData);
    }
    {
        QString err;
        check(QStringLiteral("7-Zip 后端: 完整性测试(明文)"),
              sz->test(plain7z, QString(), {}, {}, err), err);
    }

    // ---- 加密 7z（AES 内容 + 加密文件名 "he"）----
    const QString enc7z = tmp.path() + QStringLiteral("/enc.7z");
    const QString pw = QStringLiteral("Yasuo-7z-2026");
    werr.clear();
    check(QStringLiteral("7-Zip 后端: szCreate7z 生成加密 7z(含文件名加密)"),
          sevenzip::szCreate7z(items, enc7z.toStdWString(), pw.toStdWString(), true, werr),
          QString::fromWCharArray(werr.c_str()));
    if (!QFile::exists(enc7z)) return;

    {
        ArchiveInfo info; QString err;
        check(QStringLiteral("7-Zip 后端: 带密码列出加密 7z"),
              sz->list(enc7z, pw, info, err) && info.entryCount == 3, err);
        bool allEnc = true;
        for (const EntryInfo& e : info.entries) if (!e.encrypted) allEnc = false;
        check(QStringLiteral("7-Zip 后端: 加密 7z 条目标记为 encrypted"), allEnc);
    }
    {
        ExtractOptions eo; eo.archivePath = enc7z;
        eo.destDir = tmp.path() + QStringLiteral("/bad");
        eo.overwrite = OverwritePolicy::Overwrite;
        QString err;
        check(QStringLiteral("7-Zip 后端: 无密码解压加密 7z 被拒绝"),
              !sz->extract(eo, {}, {}, err), err);
    }
    {
        ExtractOptions eo; eo.archivePath = enc7z;
        eo.destDir = tmp.path() + QStringLiteral("/un_enc");
        eo.overwrite = OverwritePolicy::Overwrite;
        eo.password = pw;
        QString err;
        check(QStringLiteral("7-Zip 后端: 正确密码解压加密 7z"), sz->extract(eo, {}, {}, err), err);
        check(QStringLiteral("7-Zip 后端: 加密解压内容一致(中文)"),
              readFile(eo.destDir + QStringLiteral("/中文 文件名.txt")) == cnData);
    }
    {
        QString err;
        check(QStringLiteral("7-Zip 后端: 完整性测试(加密)"),
              sz->test(enc7z, pw, {}, {}, err), err);
    }
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("YasuoCompressor"));
    QCoreApplication::setOrganizationName(QStringLiteral("Yasuo"));

    out << "==== 压索 引擎自测 ====\n";
    out << Engines::summary() << "\n\n";

    QTemporaryDir sampleDir;
    const QString src = makeSample(sampleDir.path() + QStringLiteral("/sample"));

    testPathGuard();
    testOverwritePolicies();

    IArchiveEngine* best = Engines::best();
    testRoundTrip(best, QStringLiteral("主引擎"), src);
    testRoundTrip(createZlibEngine(), QStringLiteral("内置引擎"), src);
    testEncryption(best, src);
    testFormats(best, src);
    testRemoveEntries(best, src);
    testLargeFile(best);

    // 两个引擎都要过第三方校验：只测 best() 会掩盖内置引擎的本地文件头缺陷
    // （libarchive 成为主引擎时，内置引擎的 ZIP 从未被 bsdtar 验证过）。
    testCrossTools(best, QStringLiteral("主引擎"));
    if (best != createZlibEngine())
        testCrossTools(createZlibEngine(), QStringLiteral("内置引擎"));

    // 外部 UTF-8 名 ZIP 的回归：两套引擎都不得静默丢弃中文条目
    testExternalUtf8Names(best, QStringLiteral("主引擎"));
    if (best != createZlibEngine())
        testExternalUtf8Names(createZlibEngine(), QStringLiteral("内置引擎"));

    // 7-Zip 只读后端：用 7z.dll 现场造档并读回，验证后端真正可用
    testSevenZipBackend();

    out << "\n==== RESULT: " << (failed == 0 ? "ALL PASS" : "FAIL")
        << " (" << passed << " passed, " << failed << " failed) ====\n";
    return failed == 0 ? 0 : 1;
}
