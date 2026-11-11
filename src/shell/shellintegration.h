#pragma once

#include <QObject>
#include <QStringList>

class QLocalServer;

namespace yasuo {

// Windows 资源管理器右键菜单 / 打开方式集成。
// 使用 HKCU 注册表，无需管理员权限，安装与卸载都可逆。
class ShellIntegration {
public:
    static bool isInstalled();
    static bool install(QString& err);
    static bool uninstall(QString& err);
    static QStringList menuLabels();
    static QStringList archiveExtensions();
    static QString statusText();
};

// 单实例 + 命令行转发。
// 多选文件时 Windows 会按 MultiSelectModel=Player 逐个启动进程，
// 每个进程把路径推给主实例，主实例聚合后一次性打开，避免弹出 N 个窗口。
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(QObject* parent = nullptr);
    ~SingleInstance() override;

    // true = 当前进程为主实例并已开始监听
    bool tryBecomePrimary();
    // true = 参数已成功转发给主实例（当前进程应立即退出）
    bool forward(const QStringList& args);

    static QString serverName();

signals:
    void argumentsReceived(const QStringList& args);

private slots:
    void onNewConnection();

private:
    QLocalServer* m_server = nullptr;
};

} // namespace yasuo
