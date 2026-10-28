#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QStringList>
#include <QThread>

class ArchiveWorker;
class TitleBar;
class QListWidget;
class QLineEdit;
class QComboBox;
class QPushButton;
class QProgressBar;
class QLabel;
class QSlider;
class QWidget;

// 压索 · Yasuo Compressor 主窗口（无边框 + QQ 风格）
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

private slots:
    void onModeChanged(int index);
    void onAddFiles();
    void onAddFolder();
    void onRemove();
    void onClear();
    void onBrowseArchive();
    void onBrowseDest();
    void onFormatChanged(int index);
    void onStart();
    void onProgress(qint64 done, qint64 total, const QString& current);
    void onFinished(bool ok, const QString& message);
    void onMinimize();
    void onFilesDropped(const QStringList& paths);

public slots:
    void onToggleMaximize();

private:
    void setControlsEnabled(bool enabled);
    QString defaultSuffixFor(const QString& formatLabel) const;
    QPair<QString, QString> formatFilterFor(const QString& formatLabel) const;

    // 标题栏
    TitleBar* m_titleBar = nullptr;
    QPushButton* m_minBtn = nullptr;
    QPushButton* m_maxBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;

    QComboBox* m_modeCombo = nullptr;
    QListWidget* m_fileList = nullptr;
    QLineEdit* m_archiveEdit = nullptr;
    QLineEdit* m_destEdit = nullptr;
    QComboBox* m_formatCombo = nullptr;
    QSlider* m_levelSlider = nullptr;
    QLabel* m_levelLabel = nullptr;
    QLabel* m_hintLabel = nullptr;
    QPushButton* m_startBtn = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_status = nullptr;

    QThread* m_thread = nullptr;
    ArchiveWorker* m_worker = nullptr;
    bool m_busy = false;
    bool m_maximized = false;
};

#endif // MAINWINDOW_H
