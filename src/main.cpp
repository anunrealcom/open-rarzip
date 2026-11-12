#include "mainwindow.h"
#include "theme.h"
#include "iengine.h"
#include "shellintegration.h"

#include <QApplication>
#include <QTimer>
#include <QTextStream>
#include <QFileInfo>
#include <QIcon>

using namespace yasuo;

namespace {

struct CliOptions {
    QString password;
    Format format = Format::Auto;
    bool formatSet = false;
    Encryption encryption = Encryption::None;
    bool encryptionSet = false;
    int level = -1;                       // -1 = 引擎默认
    OverwritePolicy overwrite = OverwritePolicy::Rename;
    bool forceZip64 = false;
    bool help = false;
    QStringList positional;
};

void printCliUsage(QTextStream& out) {
    out << "用法: 一幻压缩 --cli <list|test|compress|extract> [选项] [参数...]\n"
           "\n命令:\n"
           "  list    <归档> [密码]            列出归档内容\n"
           "  test    <归档> [密码]            完整性测试\n"
           "  compress <输出归档> <文件...>      创建归档\n"
           "  extract <归档> [目标目录]          解压归档\n"
           "\n选项:\n"
           "  -p, --password <密码>             归档密码（加密包必填）\n"
           "      --format <格式>               输出格式: zip/7z/tar/tar.gz/tar.bz2/tar.xz/tar.zst/gz/bz2/xz/zst\n"
           "      --encryption <方式>           ZIP 加密: aes256(默认)/aes128/zipcrypto\n"
           "      --level <0..9>                压缩级别\n"
           "      --overwrite <策略>            解压覆盖: ask/overwrite/skip/rename(默认)/newer\n"
           "      --zip64                      强制生成 ZIP64 记录（大归档压测）\n"
           "  -h, --help                        显示本帮助\n";
}

// 解析选项与位置参数。未知选项一律报错，避免静默忽略导致行为与预期不符。
bool parseCliArgs(const QStringList& raw, CliOptions& o, QString& err) {
    for (int i = 0; i < raw.size(); ++i) {
        const QString a = raw.at(i);
        const bool isOpt = a.startsWith(QLatin1String("--")) ||
                           (a.size() == 2 && a.startsWith(QLatin1Char('-')));
        if (!isOpt) { o.positional.append(a); continue; }

        auto needVal = [&](QString& outVal) -> bool {
            if (i + 1 >= raw.size()) {
                err = QStringLiteral("选项 %1 缺少取值").arg(a);
                return false;
            }
            outVal = raw.at(++i);
            return true;
        };

        if (a == QLatin1String("-p") || a == QLatin1String("--password")) {
            if (!needVal(o.password)) return false;
        } else if (a == QLatin1String("--format")) {
            QString v;
            if (!needVal(v)) return false;
            o.format = formatFromName(v);
            if (o.format == Format::Unknown) {
                err = QStringLiteral("未知格式: %1").arg(v);
                return false;
            }
            o.formatSet = true;
        } else if (a == QLatin1String("--encryption")) {
            QString v;
            if (!needVal(v)) return false;
            o.encryption = encryptionFromName(v);
            o.encryptionSet = true;
        } else if (a == QLatin1String("--level")) {
            QString v;
            if (!needVal(v)) return false;
            bool ok = false;
            const int lv = v.toInt(&ok);
            if (!ok || lv < 0 || lv > 9) { err = QStringLiteral("--level 需为 0..9"); return false; }
            o.level = lv;
        } else if (a == QLatin1String("--overwrite")) {
            QString v;
            if (!needVal(v)) return false;
            const QString s = v.toLower();
            if (s == QLatin1String("overwrite")) o.overwrite = OverwritePolicy::Overwrite;
            else if (s == QLatin1String("skip")) o.overwrite = OverwritePolicy::Skip;
            else if (s == QLatin1String("rename")) o.overwrite = OverwritePolicy::Rename;
            else if (s == QLatin1String("newer")) o.overwrite = OverwritePolicy::OverwriteIfNewer;
            else if (s == QLatin1String("ask")) o.overwrite = OverwritePolicy::Ask;
            else { err = QStringLiteral("未知覆盖策略: %1").arg(v); return false; }
        } else if (a == QLatin1String("--zip64")) {
            o.forceZip64 = true;
        } else if (a == QLatin1String("--help") || a == QLatin1String("-h")) {
            o.help = true;
        } else {
            err = QStringLiteral("未知选项: %1").arg(a);
            return false;
        }
    }
    return true;
}

int runCli(const QStringList& rawArgs) {
    QTextStream out(stdout);
    CliOptions o;
    QString err;
    if (!parseCliArgs(rawArgs, o, err)) { out << "参数错误: " << err << "\n"; return 2; }

    const QStringList& args = o.positional;
    if (o.help) { printCliUsage(out); return 0; }
    if (args.isEmpty()) { printCliUsage(out); return 2; }

    const QString cmd = args.at(0);

    if (cmd == QLatin1String("list")) {
        if (args.size() < 2) { out << "用法: --cli list <归档> [密码]\n"; return 2; }
        // 密码优先取 -p/--password；兼容旧的 list <归档> <密码> 位置写法
        QString pw = o.password;
        if (pw.isEmpty() && args.size() > 2) pw = args.at(2);
        IArchiveEngine* engine = Engines::best(args.at(1));
        ArchiveInfo info;
        if (!engine->list(args.at(1), pw, info, err)) {
            out << "失败: " << err << "\n"; return 1;
        }
        out << "格式: " << info.formatName << "  条目: " << info.entryCount
            << "  原始: " << humanBytes(info.totalSize) << "\n";
        for (const EntryInfo& e : info.entries)
            out << (e.isDir ? "[DIR] " : "      ") << e.path
                << "  " << humanBytes(e.size) << (e.encrypted ? "  [加密]" : "") << "\n";
        return 0;
    }
    if (cmd == QLatin1String("test")) {
        if (args.size() < 2) { out << "用法: --cli test <归档> [密码]\n"; return 2; }
        QString pw = o.password;
        if (pw.isEmpty() && args.size() > 2) pw = args.at(2);
        IArchiveEngine* engine = Engines::best(args.at(1));
        const bool ok = engine->test(args.at(1), pw, {}, {}, err);
        out << (ok ? QStringLiteral("完整性测试通过") : QStringLiteral("失败: ") + err) << "\n";
        return ok ? 0 : 1;
    }
    if (cmd == QLatin1String("compress")) {
        if (args.size() < 3) { out << "用法: --cli compress <输出归档> <文件/文件夹...>\n"; return 2; }
        CompressOptions opt;
        opt.archivePath = args.at(1);
        for (int i = 2; i < args.size(); ++i) opt.inputs.append(args.at(i));
        opt.format = o.formatSet ? o.format : formatFromSuffix(opt.archivePath);
        if (opt.format == Format::Unknown) opt.format = Format::Zip;
        opt.password = o.password;
        if (!opt.password.isEmpty())
            opt.encryption = o.encryptionSet ? o.encryption : Encryption::Aes256;
        if (o.level >= 0) opt.level = o.level;
        opt.forceZip64 = o.forceZip64;
        // 创建归档必须走可写主引擎（7-Zip 后端只读）
        IArchiveEngine* engine = Engines::best();
        const bool ok = engine->compress(opt, {}, {}, err);
        out << (ok ? QStringLiteral("已创建: ") + opt.archivePath : QStringLiteral("失败: ") + err) << "\n";
        return ok ? 0 : 1;
    }
    if (cmd == QLatin1String("extract")) {
        if (args.size() < 2) { out << "用法: --cli extract <归档> [目标目录] [--password <密码>]\n"; return 2; }
        ExtractOptions opt;
        opt.archivePath = args.at(1);
        opt.destDir = args.size() > 2
            ? args.at(2)
            : QFileInfo(args.at(1)).absolutePath() + QLatin1Char('/') +
              QFileInfo(args.at(1)).completeBaseName();
        opt.password = o.password;
        opt.overwrite = o.overwrite;
        IArchiveEngine* engine = Engines::best(opt.archivePath);
        const bool ok = engine->extract(opt, {}, {}, err);
        out << (ok ? QStringLiteral("已解压到: ") + opt.destDir : QStringLiteral("失败: ") + err) << "\n";
        return ok ? 0 : 1;
    }
    out << "未知命令: " << cmd << "\n";
    printCliUsage(out);
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("YasuoCompressor"));
    QApplication::setApplicationDisplayName(QStringLiteral("一幻压缩"));
    QApplication::setApplicationVersion(QStringLiteral("2.0.0"));
    QApplication::setOrganizationName(QStringLiteral("Yasuo"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    const QStringList args = app.arguments().mid(1);

    // 安装器静默调用：注册/注销右键菜单，不启动界面
    if (args.contains(QLatin1String("--install-shell"))) {
        QString err;
        const bool ok = ShellIntegration::install(err);
        QTextStream(stdout) << (ok ? "shell integration installed" : "failed: " + err) << "\n";
        return ok ? 0 : 1;
    }
    if (args.contains(QLatin1String("--uninstall-shell"))) {
        QString err;
        const bool ok = ShellIntegration::uninstall(err);
        QTextStream(stdout) << (ok ? "shell integration removed" : "failed: " + err) << "\n";
        return ok ? 0 : 1;
    }
    const int cliIndex = args.indexOf(QLatin1String("--cli"));
    if (cliIndex >= 0) return runCli(args.mid(cliIndex + 1));

    SingleInstance instance;
    if (!instance.tryBecomePrimary()) {
        // 已有主实例：转发参数后退出（多选文件由主实例聚合）
        if (instance.forward(args)) return 0;
        QTextStream(stderr) << "无法连接主实例，且监听端口已被占用\n";
        return 1;
    }

    Theme::apply(Theme::isDark());
    MainWindow w;
    QObject::connect(&instance, &SingleInstance::argumentsReceived,
                     &w, &MainWindow::handleRemoteArgs);
    w.show();
    QTimer::singleShot(0, &w, &MainWindow::installShellOnFirstRun);
    if (!args.isEmpty())
        QTimer::singleShot(50, &w, [&w, args]() { w.handleRemoteArgs(args); });
    return app.exec();
}
