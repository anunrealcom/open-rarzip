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

int runCli(const QStringList& args) {
    QTextStream out(stdout);
    if (args.size() < 2) {
        out << "用法: YasuoCompressor --cli <list|test|compress|extract> [参数...]\n";
        return 2;
    }
    const QString cmd = args.at(0);
    IArchiveEngine* engine = Engines::best();
    QString err;

    if (cmd == QLatin1String("list")) {
        ArchiveInfo info;
        if (!engine->list(args.at(1), args.size() > 2 ? args.at(2) : QString(), info, err)) {
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
        const bool ok = engine->test(args.at(1), args.size() > 2 ? args.at(2) : QString(),
                                     {}, {}, err);
        out << (ok ? QStringLiteral("完整性测试通过") : QStringLiteral("失败: ") + err) << "\n";
        return ok ? 0 : 1;
    }
    if (cmd == QLatin1String("compress")) {
        if (args.size() < 3) { out << "用法: --cli compress <输出归档> <文件/文件夹...>\n"; return 2; }
        CompressOptions opt;
        opt.archivePath = args.at(1);
        for (int i = 2; i < args.size(); ++i) opt.inputs.append(args.at(i));
        opt.format = formatFromSuffix(opt.archivePath);
        if (opt.format == Format::Unknown) opt.format = Format::Zip;
        const bool ok = engine->compress(opt, {}, {}, err);
        out << (ok ? QStringLiteral("已创建: ") + opt.archivePath : QStringLiteral("失败: ") + err) << "\n";
        return ok ? 0 : 1;
    }
    if (cmd == QLatin1String("extract")) {
        if (args.size() < 2) { out << "用法: --cli extract <归档> [目标目录]\n"; return 2; }
        ExtractOptions opt;
        opt.archivePath = args.at(1);
        opt.destDir = args.size() > 2
            ? args.at(2)
            : QFileInfo(args.at(1)).absolutePath() + QLatin1Char('/') +
              QFileInfo(args.at(1)).completeBaseName();
        opt.overwrite = OverwritePolicy::Rename;
        const bool ok = engine->extract(opt, {}, {}, err);
        out << (ok ? QStringLiteral("已解压到: ") + opt.destDir : QStringLiteral("失败: ") + err) << "\n";
        return ok ? 0 : 1;
    }
    out << "未知命令: " << cmd << "\n";
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("YasuoCompressor"));
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
