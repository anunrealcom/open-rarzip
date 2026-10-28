#include "archiveengine.h"

#include <cstdio>

#include <QFile>
#include <QDir>
#include <QProcess>
#include <QCryptographicHash>
#include <QDebug>

// Qt 在 Windows 下默认把 qDebug 送到调试器，这里改成直接写 stderr，方便命令行/日志查看
struct LogSink {
    QString buf;
    LogSink& operator<<(const QString& s) { buf += s; return *this; }
    LogSink& operator<<(const char* s)    { buf += QString::fromUtf8(s); return *this; }
    LogSink& operator<<(bool b)           { buf += b ? QStringLiteral("true") : QStringLiteral("false"); return *this; }
    LogSink& operator<<(int n)            { buf += QString::number(n); return *this; }
    LogSink& operator<<(qint64 n)         { buf += QString::number(n); return *this; }
    ~LogSink() { fprintf(stderr, "%s\n", buf.toUtf8().constData()); fflush(stderr); }
};
#undef qDebug
#undef qWarning
#define qDebug()   LogSink()
#define qWarning() LogSink()

static QString sha1File(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
}

static bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    return f.write(data) == data.size();
}

// 建一个测试目录：英文文件名（供外部工具交叉验证，规避控制台编码问题）
static QString makeAsciiSample(const QString& tag) {
    QString tmp = QDir::tempPath() + "/yasuo_" + tag;
    QDir(tmp).removeRecursively();
    QDir().mkpath(tmp + "/sub");
    writeFile(tmp + "/a.txt", QByteArray(4096, 'A') + "hello zip\n");
    writeFile(tmp + "/sub/b.txt", "second file content\n");
    writeFile(tmp + "/empty.txt", QByteArray());
    return tmp;
}

// 建一个含中文名的目录（验证 UTF-8 文件名标志）
static QString makeUtf8Sample(const QString& tag) {
    QString tmp = QDir::tempPath() + "/yasuo_" + tag;
    QDir(tmp).removeRecursively();
    QDir().mkpath(tmp + "/子目录");
    writeFile(tmp + "/中文 文件名.txt", "中文内容测试\n");
    writeFile(tmp + "/子目录/第二个.txt", "嵌套中文\n");
    return tmp;
}

static bool runCmd(const QString& program, const QStringList& args) {
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.start();
    if (!p.waitForStarted(10000)) { qWarning() << "无法启动" << program; return false; }
    if (!p.waitForFinished(60000)) { qWarning() << "超时" << program; return false; }
    if (p.exitCode() != 0)
        qWarning() << program << "返回" << p.exitCode() << QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
    return p.exitCode() == 0;
}

// 本程序自己压 -> 自己解
static bool selfRoundTrip(const QString& src, const QString& tag, bool utf8) {
    CompressOptions opt;
    opt.archivePath = QDir::tempPath() + "/yasuo_out_" + tag + ".zip";
    opt.inputs = QStringList({src});
    opt.format = "zip";
    opt.level = 6;
    QString err;
    if (!ArchiveEngine::compress(opt, {}, {}, err)) { qWarning() << "[压缩失败]" << err; return false; }

    QString out = QDir::tempPath() + "/yasuo_self_" + tag;
    QDir(out).removeRecursively();
    if (!ArchiveEngine::extract(opt.archivePath, out, {}, {}, err)) { qWarning() << "[解压失败]" << err; return false; }

    // 压缩包内保留顶层目录名，解压后是 out/<顶层名>/...
    QString root = QFileInfo(src).fileName();
    bool same;
    if (utf8) {
        same = sha1File(src + "/中文 文件名.txt")   == sha1File(out + "/" + root + "/中文 文件名.txt")
            && sha1File(src + "/子目录/第二个.txt") == sha1File(out + "/" + root + "/子目录/第二个.txt");
    } else {
        same = sha1File(src + "/a.txt")     == sha1File(out + "/" + root + "/a.txt")
            && sha1File(src + "/sub/b.txt") == sha1File(out + "/" + root + "/sub/b.txt")
            && QFile::exists(out + "/" + root + "/empty.txt");
    }
    qDebug() << (utf8 ? "[ZIP-UTF8]" : "[ZIP]") << "本程序自往返:" << same;
    return same;
}

// 本程序压 -> Windows 自带 tar.exe 解（第三方实现，验证兼容性）
static bool crossCheckWithTar(const QString& zipPath, const QString& srcDir) {
    QString out = QDir::tempPath() + "/yasuo_tar";
    QDir(out).removeRecursively();
    QDir().mkpath(out);
    if (!runCmd("tar", {"-tf", zipPath})) { qDebug() << "[tar] 无法列出归档"; return false; }
    if (!runCmd("tar", {"-xf", zipPath, "-C", out})) { qDebug() << "[tar] 解压失败"; return false; }

    QString root = QFileInfo(srcDir).fileName();
    bool same = sha1File(srcDir + "/a.txt")        == sha1File(out + "/" + root + "/a.txt")
             && sha1File(srcDir + "/sub/b.txt")    == sha1File(out + "/" + root + "/sub/b.txt");
    qDebug() << "[tar.exe] 外部工具解出并比对:" << same;
    return same;
}

// 本程序压 -> PowerShell Expand-Archive 解（.NET 实现，校验最严格之一）
static bool crossCheckWithPowerShell(const QString& zipPath, const QString& srcDir) {
    QString out = QDir::tempPath() + "/yasuo_ps";
    QDir(out).removeRecursively();
    QDir().mkpath(out);
    QString cmd = QString("Expand-Archive -LiteralPath '%1' -DestinationPath '%2' -Force")
                      .arg(zipPath, out);
    if (!runCmd("powershell", {"-NoProfile", "-NonInteractive", "-Command", cmd})) {
        qDebug() << "[PowerShell] 解压失败";
        return false;
    }
    QString root = QFileInfo(srcDir).fileName();
    bool same = sha1File(srcDir + "/a.txt")     == sha1File(out + "/" + root + "/a.txt")
             && sha1File(srcDir + "/sub/b.txt") == sha1File(out + "/" + root + "/sub/b.txt");
    qDebug() << "[PowerShell Expand-Archive] 外部工具解出并比对:" << same;
    return same;
}

static bool roundTripGz() {
    QString tmp = QDir::tempPath() + "/yasuo_gz";
    QDir(tmp).removeRecursively();
    QDir().mkpath(tmp);
    QByteArray payload = "gzip single file 内容 test\n";
    writeFile(tmp + "/s.txt", payload);

    CompressOptions opt;
    opt.archivePath = tmp + "/s.gz";
    opt.inputs = QStringList({tmp + "/s.txt"});
    opt.format = "raw";
    opt.filter = "gzip";
    QString err;
    if (!ArchiveEngine::compress(opt, {}, {}, err)) { qWarning() << "[GZ] 压缩失败:" << err; return false; }
    QString outDir = tmp + "/outdir";   // 解压目标：目录
    if (!ArchiveEngine::extract(opt.archivePath, outDir, {}, {}, err)) { qWarning() << "[GZ] 解压失败:" << err; return false; }
    bool same = sha1File(outDir + "/s") == sha1File(tmp + "/s.txt");
    qDebug() << "[GZ] 自往返:" << same;
    return same;
}

// 打包出来的 ZIP 不能带任何加密标志位
static bool checkNoEncryptionFlag(const QString& zipPath) {
    QFile f(zipPath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QByteArray all = f.readAll();
    qint64 eocd = -1;
    for (qint64 i = all.size() - 22; i >= 0; --i)
        if (quint8(all[i]) == 0x50 && quint8(all[i+1]) == 0x4b && quint8(all[i+2]) == 0x05 && quint8(all[i+3]) == 0x06) { eocd = i; break; }
    if (eocd < 0) return false;
    quint16 n = quint16(quint8(all[eocd+10]) | (quint8(all[eocd+11]) << 8));
    quint32 off = quint32(quint8(all[eocd+16]) | (quint8(all[eocd+17]) << 8) | (quint8(all[eocd+18]) << 16) | (quint32(quint8(all[eocd+19])) << 24));
    bool clean = true;
    qint64 pos = off;
    for (int i = 0; i < n; ++i) {
        quint16 flag = quint16(quint8(all[pos+8]) | (quint8(all[pos+9]) << 8));
        if (flag & 0x0001) clean = false;                 // bit0: 加密
        if (flag & 0x0040) clean = false;                 // bit6: 强加密
        quint16 nlen = quint16(quint8(all[pos+28]) | (quint8(all[pos+29]) << 8));
        quint16 elen = quint16(quint8(all[pos+30]) | (quint8(all[pos+31]) << 8));
        quint16 clen = quint16(quint8(all[pos+32]) | (quint8(all[pos+33]) << 8));
        pos += 46 + nlen + elen + clen;
    }
    qDebug() << "[标志位] 无任何加密标志:" << clean << "（共" << n << "个条目）";
    return clean;
}

int main() {
    QString srcAscii = makeAsciiSample("ascii");
    QString srcUtf8  = makeUtf8Sample("utf8");

    bool r1 = selfRoundTrip(srcAscii, "ascii", false);
    bool r2 = selfRoundTrip(srcUtf8,  "utf8",  true);
    bool r3 = roundTripGz();

    QString zipPath = QDir::tempPath() + "/yasuo_out_ascii.zip";
    bool r4 = checkNoEncryptionFlag(zipPath);
    bool r5 = crossCheckWithTar(zipPath, srcAscii);
    bool r6 = crossCheckWithPowerShell(zipPath, srcAscii);

    bool all = r1 && r2 && r3 && r4 && r5 && r6;
    qDebug() << "==== RESULT:" << (all ? "ALL PASS" : "FAIL") << "====";
    return all ? 0 : 1;
}
