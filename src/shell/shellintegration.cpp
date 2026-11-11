#include "shellintegration.h"

#include <QCoreApplication>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QDataStream>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace yasuo {
namespace {

struct MenuDef {
    QString root;      // 相对 HKCU\Software\Classes 的子键
    QString label;
    QString verb;
};

QString exeCommand(const QString& verb) {
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    // 注意：%1 是资源管理器的文件路径占位符，必须原样写入注册表
    return QLatin1Char('"') + exe + QStringLiteral("\" ") + verb + QStringLiteral(" \"%1\"");
}

QString iconValue() {
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + QStringLiteral(",0");
}

QVector<MenuDef> menuDefs() {
    QVector<MenuDef> v;
    v.append({ QStringLiteral("*/shell/Yasuo.Compress"),
               QStringLiteral("用 压索 压缩…"), QStringLiteral("--quick-compress") });
    v.append({ QStringLiteral("*/shell/Yasuo.Open"),
               QStringLiteral("用 压索 浏览压缩包"), QStringLiteral("--open") });
    v.append({ QStringLiteral("Directory/shell/Yasuo.Compress"),
               QStringLiteral("用 压索 压缩此文件夹"), QStringLiteral("--quick-compress") });
    return v;
}

#ifdef Q_OS_WIN
// QSettings 无法可靠写入注册表「(默认)」值，而 Shell verb 的 command 必须是默认值，
// 因此这里直接用 Win32 API，保证右键菜单真正可用。
bool regSet(HKEY root, const QString& subKey, const QString& name, const QString& value) {
    HKEY h = nullptr;
    const QString nativeKey = QDir::toNativeSeparators(subKey);
    LONG r = RegCreateKeyExW(root, reinterpret_cast<LPCWSTR>(nativeKey.utf16()), 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &h, nullptr);
    if (r != ERROR_SUCCESS) return false;
    LPCWSTR valueName = nullptr;
    const QString n = name;
    if (!n.isEmpty()) valueName = reinterpret_cast<LPCWSTR>(n.utf16());
    const QString v = value;
    r = RegSetValueExW(h, valueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(v.utf16()),
                       static_cast<DWORD>((v.length() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

bool regDelete(HKEY root, const QString& subKey) {
    const QString nativeKey = QDir::toNativeSeparators(subKey);
    return RegDeleteTreeW(root, reinterpret_cast<LPCWSTR>(nativeKey.utf16())) == ERROR_SUCCESS;
}

bool regHas(HKEY root, const QString& subKey) {
    HKEY h = nullptr;
    const QString nativeKey = QDir::toNativeSeparators(subKey);
    const LONG r = RegOpenKeyExW(root, reinterpret_cast<LPCWSTR>(nativeKey.utf16()),
                                 0, KEY_READ, &h);
    if (r == ERROR_SUCCESS) { RegCloseKey(h); return true; }
    return false;
}
#endif

} // namespace

QStringList ShellIntegration::archiveExtensions() {
    return { QStringLiteral(".zip"), QStringLiteral(".7z"), QStringLiteral(".rar"),
             QStringLiteral(".tar"), QStringLiteral(".gz"),  QStringLiteral(".bz2"),
             QStringLiteral(".xz"),  QStringLiteral(".zst"), QStringLiteral(".tgz"),
             QStringLiteral(".txz"), QStringLiteral(".tbz2"), QStringLiteral(".tzst") };
}

QStringList ShellIntegration::menuLabels() {
    QStringList labels;
    for (const MenuDef& m : menuDefs()) labels.append(m.label);
    labels.append(QStringLiteral("归档文件 → 解压到同名文件夹（12 种扩展名）"));
    labels.append(QStringLiteral("归档文件 → 打开方式：压索"));
    return labels;
}

bool ShellIntegration::isInstalled() {
#ifdef Q_OS_WIN
    return regHas(HKEY_CURRENT_USER, QStringLiteral("Software/Classes/*/shell/Yasuo.Compress"));
#else
    return false;
#endif
}

bool ShellIntegration::install(QString& err) {
#ifdef Q_OS_WIN
    const QString exe = QCoreApplication::applicationFilePath();
    if (!QFileInfo::exists(exe)) { err = QStringLiteral("无法定位主程序"); return false; }
    const QString classes = QStringLiteral("Software/Classes/");

    // 1) 通用右键菜单：所有文件、文件夹
    for (const MenuDef& m : menuDefs()) {
        const QString key = classes + m.root;
        if (!regSet(HKEY_CURRENT_USER, key, QString(), m.label) ||
            !regSet(HKEY_CURRENT_USER, key, QStringLiteral("MUIVerb"), m.label) ||
            !regSet(HKEY_CURRENT_USER, key, QStringLiteral("Icon"), iconValue()) ||
            !regSet(HKEY_CURRENT_USER, key, QStringLiteral("MultiSelectModel"),
                    QStringLiteral("Player")) ||
            !regSet(HKEY_CURRENT_USER, key + QStringLiteral("/command"), QString(),
                    exeCommand(m.verb))) {
            err = QStringLiteral("写入右键菜单失败: %1").arg(m.root);
            return false;
        }
    }

    // 2) 归档扩展名专属「解压到同名文件夹」
    for (const QString& ext : archiveExtensions()) {
        const QString key = classes + QStringLiteral("SystemFileAssociations/%1/shell/Yasuo.Extract").arg(ext);
        regSet(HKEY_CURRENT_USER, key, QString(), QStringLiteral("解压到同名文件夹（压索）"));
        regSet(HKEY_CURRENT_USER, key, QStringLiteral("Icon"), iconValue());
        if (!regSet(HKEY_CURRENT_USER, key + QStringLiteral("/command"), QString(),
                    exeCommand(QStringLiteral("--quick-extract")))) {
            err = QStringLiteral("写入 %1 关联失败").arg(ext);
            return false;
        }
    }

    // 3) 只在「打开方式」中出现，不抢占系统默认程序
    const QString appKey = classes + QStringLiteral("Applications/YasuoCompressor.exe/shell/open/command");
    regSet(HKEY_CURRENT_USER, appKey, QString(),
           QLatin1Char('"') + QDir::toNativeSeparators(exe) + QStringLiteral("\" --open \"%1\""));
    for (const QString& ext : archiveExtensions()) {
        regSet(HKEY_CURRENT_USER,
               QStringLiteral("Software/Microsoft/Windows/CurrentVersion/Explorer/FileExts/%1/OpenWithList").arg(ext),
               QStringLiteral("a"), QStringLiteral("YasuoCompressor.exe"));
    }
    return true;
#else
    err = QStringLiteral("右键菜单集成目前仅支持 Windows");
    return false;
#endif
}

bool ShellIntegration::uninstall(QString& err) {
#ifdef Q_OS_WIN
    const QString classes = QStringLiteral("Software/Classes/");
    for (const MenuDef& m : menuDefs()) regDelete(HKEY_CURRENT_USER, classes + m.root);
    for (const QString& ext : archiveExtensions())
        regDelete(HKEY_CURRENT_USER,
                  classes + QStringLiteral("SystemFileAssociations/%1/shell/Yasuo.Extract").arg(ext));
    regDelete(HKEY_CURRENT_USER, classes + QStringLiteral("Applications/YasuoCompressor.exe"));
    return true;
#else
    err = QStringLiteral("右键菜单集成目前仅支持 Windows");
    return false;
#endif
}

QString ShellIntegration::statusText() {
    return isInstalled()
        ? QStringLiteral("已集成资源管理器右键菜单（%1 类入口）").arg(menuLabels().size())
        : QStringLiteral("未集成右键菜单");
}

QString SingleInstance::serverName() {
    return QStringLiteral("YasuoCompressor_IPC_v2");
}

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {}

SingleInstance::~SingleInstance() {
    if (m_server) {
        m_server->close();
        QLocalServer::removeServer(serverName());
    }
}

bool SingleInstance::tryBecomePrimary() {
    m_server = new QLocalServer(this);
    if (!m_server->listen(serverName())) {
        QLocalServer::removeServer(serverName());
        if (!m_server->listen(serverName())) return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
    return true;
}

bool SingleInstance::forward(const QStringList& args) {
    QLocalSocket socket;
    socket.connectToServer(serverName(), QIODevice::WriteOnly);
    if (!socket.waitForConnected(800)) return false;
    QByteArray block;
    QDataStream ds(&block, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_6_0);
    ds << args;
    socket.write(block);
    socket.flush();
    socket.waitForBytesWritten(800);
    socket.disconnectFromServer();
    return true;
}

void SingleInstance::onNewConnection() {
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket* socket = m_server->nextPendingConnection();
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
            const QByteArray data = socket->readAll();
            QDataStream ds(data);
            ds.setVersion(QDataStream::Qt_6_0);
            QStringList args;
            ds >> args;
            if (!args.isEmpty()) emit argumentsReceived(args);
            socket->deleteLater();
        });
        QTimer::singleShot(3000, socket, &QLocalSocket::deleteLater);
    }
}

} // namespace yasuo
