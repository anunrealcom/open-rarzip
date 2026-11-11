#pragma once

#include <QMainWindow>
#include <QStringList>
#include <QVector>

#include "archivetypes.h"
#include "taskrunner.h"

class QStackedWidget;
class QListWidget;
class QLineEdit;
class QComboBox;
class QPushButton;
class QProgressBar;
class QLabel;
class QSlider;
class QSpinBox;
class QCheckBox;
class QButtonGroup;
class QPlainTextEdit;
class QThread;
class QTimer;

namespace yasuo {

class ArchiveBrowser;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

public slots:
    void handleRemoteArgs(const QStringList& args);
    void quickCompressFiles(const QStringList& paths);
    void quickExtractArchive(const QString& path);
    void showArchive(const QString& path);
    void installShellOnFirstRun();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

private slots:
    void onNavChanged(int id);
    void onAddFiles();
    void onAddFolder();
    void onRemoveSelected();
    void onClearFiles();
    void onBrowseOutput();
    void onFormatChanged(int index);
    void onStartCompress();
    void onProgress(qint64 done, qint64 total, const QString& current);
    void onFinished(bool ok, const QString& message, qint64 elapsedMs);
    void onCancelTask();
    void onToggleTheme();
    void onAccelChanged(int index);
    void onBenchmark();
    void onInstallShell();
    void onUninstallShell();
    void onExtractRequested(const QString& archive, const QStringList& entries,
                            const QString& password);
    void onTestRequested(const QString& archive, const QString& password);
    void onRemoveRequested(const QString& archive, const QStringList& entries,
                           const QString& password);
    void onPendingRemoteArgs();

private:
    void buildUi();
    QWidget* buildCompressPage();
    QWidget* buildTaskPage();
    QWidget* buildSettingsPage();
    QWidget* buildTitleBar();
    void startTask(const TaskRequest& req);
    void setBusy(bool busy);
    void appendLog(const QString& text);
    void refreshEngineInfo();
    void refreshShellStatus();
    Format currentFormat() const;
    void syncCapabilityUI();

    // 标题栏
    QPushButton* m_minBtn = nullptr;
    QPushButton* m_maxBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QLabel* m_themeBtn = nullptr;
    QPushButton* m_themeToggle = nullptr;

    // 导航
    QButtonGroup* m_navGroup = nullptr;
    QStackedWidget* m_stack = nullptr;

    // 压缩页
    QListWidget* m_fileList = nullptr;
    QLineEdit* m_outputEdit = nullptr;
    QComboBox* m_formatCombo = nullptr;
    QSlider* m_levelSlider = nullptr;
    QLabel* m_levelLabel = nullptr;
    QSpinBox* m_threadsSpin = nullptr;
    QLineEdit* m_passwordEdit = nullptr;
    QComboBox* m_encryptionCombo = nullptr;
    QLineEdit* m_excludeEdit = nullptr;
    QCheckBox* m_verifyCheck = nullptr;
    QCheckBox* m_deleteCheck = nullptr;
    QCheckBox* m_solidCheck = nullptr;
    QCheckBox* m_encryptNamesCheck = nullptr;
    QLabel* m_capHint = nullptr;
    QPushButton* m_startBtn = nullptr;

    // 浏览页
    ArchiveBrowser* m_browser = nullptr;

    // 任务页
    QProgressBar* m_progress = nullptr;
    QLabel* m_taskStatus = nullptr;
    QLabel* m_taskSpeed = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QPushButton* m_cancelBtn = nullptr;

    // 设置页
    QComboBox* m_accelCombo = nullptr;
    QLabel* m_accelDetail = nullptr;
    QLabel* m_shellStatus = nullptr;
    QLabel* m_engineInfo = nullptr;
    QLabel* m_benchResult = nullptr;

    // 状态栏
    QLabel* m_statusEngine = nullptr;
    QLabel* m_statusAccel = nullptr;

    // 任务执行
    QThread* m_thread = nullptr;
    TaskRunner* m_runner = nullptr;
    bool m_busy = false;
    JobKind m_currentJob = JobKind::Compress;
    qint64 m_taskStartMs = 0;
    qint64 m_lastDone = 0;

    // 右键菜单多选聚合
    QTimer* m_remoteTimer = nullptr;
    QStringList m_remoteCompressPaths;
    QStringList m_remoteOpenPaths;
    bool m_maximized = false;
};

} // namespace yasuo
