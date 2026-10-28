#include "mainwindow.h"
#include "archiveworker.h"
#include "archiveengine.h"

#include <QApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QSlider>
#include <QFileDialog>
#include <QFileInfo>
#include <QSet>
#include <QStandardItemModel>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDir>
#include <QMessageBox>
#include <QGroupBox>
#include <QMouseEvent>
#include <QPoint>
#include <QGraphicsDropShadowEffect>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#endif

// 自定义标题栏：负责拖拽移动与双击最大化（按钮由 MainWindow 注入）
class TitleBar : public QWidget {
public:
    explicit TitleBar(QWidget* win, QWidget* parent = nullptr) : QWidget(parent), m_win(win) {
        setObjectName(QStringLiteral("titleBar"));
        barLayout = new QHBoxLayout(this);
        barLayout->setContentsMargins(14, 0, 10, 0);
        barLayout->setSpacing(4);
    }
    QHBoxLayout* barLayout = nullptr;
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            m_dragPos = e->globalPosition().toPoint() - m_win->frameGeometry().topLeft();
            m_dragging = true;
        }
        QWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_dragging && (e->buttons() & Qt::LeftButton)) {
            // 最大化状态下拖拽先恢复窗口
            MainWindow* w = qobject_cast<MainWindow*>(m_win);
            if (w && w->isMaximized())
                w->showNormal();
            m_win->move(e->globalPosition().toPoint() - m_dragPos);
        }
        QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        m_dragging = false;
        QWidget::mouseReleaseEvent(e);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            MainWindow* w = qobject_cast<MainWindow*>(m_win);
            if (w) w->onToggleMaximize();
        }
        QWidget::mouseDoubleClickEvent(e);
    }
private:
    QWidget* m_win;
    QPoint m_dragPos;
    bool m_dragging = false;
};

// 支持从资源管理器拖入文件/文件夹的列表控件
class FileDropList : public QListWidget {
    Q_OBJECT
public:
    explicit FileDropList(QWidget* parent = nullptr) : QListWidget(parent) {
        setAcceptDrops(true);
    }
signals:
    void filesDropped(const QStringList& paths);
protected:
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
        else QListWidget::dragEnterEvent(e);
    }
    void dragMoveEvent(QDragMoveEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
        else QListWidget::dragMoveEvent(e);
    }
    void dropEvent(QDropEvent* e) override {
        if (e->mimeData()->hasUrls()) {
            QStringList paths;
            for (const QUrl& u : e->mimeData()->urls()) {
                if (u.isLocalFile()) paths.append(u.toLocalFile());
            }
            if (!paths.isEmpty()) emit filesDropped(paths);
            e->acceptProposedAction();
        } else {
            QListWidget::dropEvent(e);
        }
    }
};

static QString humanBytes(qint64 bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    return QString::number(v, 'f', v >= 100 ? 0 : 1) + " " + units[i];
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    // 无边框 + 透明背景（配合卡片圆角与阴影）
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setMinimumSize(720, 540);
    resize(800, 600);

    // 整体布局：留透明边距作为阴影/缩放热区
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* cLayout = new QVBoxLayout(central);
    cLayout->setContentsMargins(14, 14, 14, 14);
    cLayout->setSpacing(0);

    // 白色卡片
    auto* frame = new QWidget;
    frame->setObjectName(QStringLiteral("frame"));
    auto* fLayout = new QVBoxLayout(frame);
    fLayout->setContentsMargins(0, 0, 0, 0);
    fLayout->setSpacing(0);

    QGraphicsDropShadowEffect* shadow = new QGraphicsDropShadowEffect(frame);
    shadow->setBlurRadius(28);
    shadow->setColor(QColor(0, 0, 0, 60));
    shadow->setOffset(0, 6);
    frame->setGraphicsEffect(shadow);

    // ---- 标题栏 ----
    m_titleBar = new TitleBar(this, frame);
    m_titleBar->setFixedHeight(46);
    auto* icon = new QLabel(QStringLiteral("压"));
    icon->setObjectName(QStringLiteral("appIcon"));
    auto* title = new QLabel(QStringLiteral("压索 · Yasuo Compressor"));
    title->setObjectName(QStringLiteral("appTitle"));
    m_titleBar->barLayout->addWidget(icon);
    m_titleBar->barLayout->addWidget(title);
    m_titleBar->barLayout->addStretch(1);
    m_minBtn = new QPushButton(QStringLiteral("—"));   m_minBtn->setObjectName(QStringLiteral("titleBtn"));
    m_maxBtn = new QPushButton(QStringLiteral("▢"));   m_maxBtn->setObjectName(QStringLiteral("titleBtn"));
    m_closeBtn = new QPushButton(QStringLiteral("✕")); m_closeBtn->setObjectName(QStringLiteral("closeBtn"));
    m_minBtn->setFixedSize(34, 26); m_maxBtn->setFixedSize(34, 26); m_closeBtn->setFixedSize(34, 26);
    m_titleBar->barLayout->addWidget(m_minBtn);
    m_titleBar->barLayout->addWidget(m_maxBtn);
    m_titleBar->barLayout->addWidget(m_closeBtn);
    fLayout->addWidget(m_titleBar);

    // ---- 内容区 ----
    auto* content = new QWidget;
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(22, 16, 22, 18);
    v->setSpacing(12);

    // 模式
    auto* modeRow = new QHBoxLayout;
    modeRow->addWidget(new QLabel(QStringLiteral("模式:")));
    m_modeCombo = new QComboBox;
    m_modeCombo->addItems({QStringLiteral("压缩"), QStringLiteral("解压")});
    m_modeCombo->setMinimumWidth(120);
    modeRow->addWidget(m_modeCombo);
    modeRow->addStretch(1);
    v->addLayout(modeRow);

    // 文件列表
    auto* fileBox = new QGroupBox(QStringLiteral("待压缩的文件 / 文件夹"));
    fileBox->setObjectName(QStringLiteral("cardBox"));
    auto* fileLayout = new QVBoxLayout(fileBox);
    m_fileList = new FileDropList;
    m_fileList->setMinimumHeight(120);
    fileLayout->addWidget(m_fileList);
    auto* fileBtnRow = new QHBoxLayout;
    auto* addFileBtn = new QPushButton(QStringLiteral("添加文件"));
    auto* addDirBtn  = new QPushButton(QStringLiteral("添加文件夹"));
    auto* rmBtn      = new QPushButton(QStringLiteral("移除"));
    auto* clrBtn     = new QPushButton(QStringLiteral("清空"));
    fileBtnRow->addWidget(addFileBtn);
    fileBtnRow->addWidget(addDirBtn);
    fileBtnRow->addWidget(rmBtn);
    fileBtnRow->addWidget(clrBtn);
    fileBtnRow->addStretch(1);
    fileLayout->addLayout(fileBtnRow);
    v->addWidget(fileBox);

    // 归档路径
    auto* archiveRow = new QHBoxLayout;
    m_archiveEdit = new QLineEdit;
    auto* archiveBtn = new QPushButton(QStringLiteral("浏览..."));
    archiveRow->addWidget(new QLabel(QStringLiteral("归档:")));
    archiveRow->addWidget(m_archiveEdit, 1);
    archiveRow->addWidget(archiveBtn);
    v->addLayout(archiveRow);

    // 解压目标
    m_destEdit = new QLineEdit;
    auto* destRow = new QHBoxLayout;
    auto* destBtn = new QPushButton(QStringLiteral("浏览..."));
    destRow->addWidget(new QLabel(QStringLiteral("目标:")));
    destRow->addWidget(m_destEdit, 1);
    destRow->addWidget(destBtn);
    v->addLayout(destRow);

    // 格式 + 级别
    auto* optRow = new QHBoxLayout;
    optRow->addWidget(new QLabel(QStringLiteral("格式:")));
    m_formatCombo = new QComboBox;
    m_formatCombo->addItems({
        QStringLiteral("ZIP"), QStringLiteral("7Z"), QStringLiteral("TAR"),
        QStringLiteral("TAR.GZ"), QStringLiteral("TAR.BZ2"), QStringLiteral("TAR.XZ"), QStringLiteral("TAR.ZST"),
        QStringLiteral("GZ"), QStringLiteral("BZ2"), QStringLiteral("XZ"), QStringLiteral("ZST")
    });
    m_formatCombo->setMinimumWidth(110);
    // 仅启用当前编译进来的引擎真正支持的格式，其余灰显并加提示
    {
        const QSet<QString> supported(ArchiveEngine::supportedFormats().toList().toSet());
        for (int i = 0; i < m_formatCombo->count(); ++i) {
            const QString label = m_formatCombo->itemText(i);
            bool ok = supported.contains(label);
            m_formatCombo->setItemData(i, ok ? QVariant() : QStringLiteral("当前引擎不支持该格式（需 libarchive）"), Qt::ToolTipRole);
            if (auto* model = qobject_cast<QStandardItemModel*>(m_formatCombo->model()))
                if (auto* item = model->item(i)) item->setEnabled(ok);
        }
    }
    optRow->addWidget(m_formatCombo);
    optRow->addWidget(new QLabel(QStringLiteral("级别:")));
    m_levelSlider = new QSlider(Qt::Horizontal);
    m_levelSlider->setRange(0, 9);
    m_levelSlider->setValue(6);
    m_levelSlider->setFixedWidth(170);
    m_levelLabel = new QLabel(QStringLiteral("6"));
    m_levelLabel->setFixedWidth(16);
    optRow->addWidget(m_levelSlider);
    optRow->addWidget(m_levelLabel);
    optRow->addStretch(1);
    v->addLayout(optRow);

    // 兼容性说明（本程序不产出私有加密包）
    m_hintLabel = new QLabel;
    m_hintLabel->setObjectName(QStringLiteral("hintLabel"));
    m_hintLabel->setWordWrap(true);
    v->addWidget(m_hintLabel);

    // 执行按钮
    m_startBtn = new QPushButton(QStringLiteral("开始压缩"));
    m_startBtn->setObjectName(QStringLiteral("startBtn"));
    m_startBtn->setMinimumHeight(40);
    v->addWidget(m_startBtn);

    // 进度 + 状态
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    v->addWidget(m_progress);
    m_status = new QLabel(QStringLiteral("就绪"));
    m_status->setObjectName(QStringLiteral("statusLabel"));
    v->addWidget(m_status);

    fLayout->addWidget(content, 1);
    cLayout->addWidget(frame, 1);

    // ---- 样式 ----
    setStyleSheet(R"(
        QWidget#frame { background:#FFFFFF; border-radius:14px; }
        QWidget#titleBar { background:transparent; }
        QLabel#appIcon { background:#1E9FFF; color:#FFFFFF; font-weight:700; font-size:14px;
                         min-width:26px; max-width:26px; min-height:26px; max-height:26px;
                         border-radius:7px; padding:0; qproperty-alignment:AlignCenter; }
        QLabel#appTitle { color:#1A1B1F; font-weight:600; font-size:14px; }
        QLabel#statusLabel { color:#6B6E78; font-size:12px; }
        QLabel#hintLabel { color:#8A8D96; font-size:11px; padding:2px 0 0 0; }
        QPushButton#titleBtn { background:transparent; border:none; color:#5A5C66; font-size:14px; border-radius:6px; }
        QPushButton#titleBtn:hover { background:#EDEFF2; }
        QPushButton#closeBtn { background:transparent; border:none; color:#5A5C66; font-size:13px; border-radius:6px; }
        QPushButton#closeBtn:hover { background:#F25C5C; color:#FFFFFF; }
        QPushButton { background:#F2F3F5; color:#1A1B1F; border:none; border-radius:6px;
                      padding:7px 14px; font-size:13px; }
        QPushButton:hover { background:#E9EBEE; }
        QPushButton#startBtn { background:#1E9FFF; color:#FFFFFF; font-weight:600; font-size:14px; border-radius:8px; }
        QPushButton#startBtn:hover { background:#0F8DE8; }
        QPushButton#startBtn:pressed { background:#0A7BD0; }
        QPushButton#startBtn:disabled { background:#BFE0FB; color:#FFFFFF; }
        QGroupBox#cardBox { background:#FBFBFC; border:1px solid #EEF0F2; border-radius:10px;
                            margin-top:10px; padding-top:14px; font-weight:600; color:#1A1B1F; }
        QGroupBox#cardBox::title { subcontrol-origin:margin; left:14px; padding:0 4px; }
        QLineEdit, QComboBox { background:#FFFFFF; border:1px solid #D8DCE0; border-radius:6px; padding:7px 9px; }
        QLineEdit:focus, QComboBox:focus { border:1px solid #1E9FFF; }
        QListWidget { background:#FFFFFF; border:1px solid #E3E5E8; border-radius:8px; padding:4px; outline:0; }
        QListWidget::item:hover { background:#EAF5FF; }
        QListWidget::item:selected { background:#D6ECFF; color:#1A1B1F; }
        QComboBox QAbstractItemView { background:#FFFFFF; border:1px solid #E3E5E8; selection-background-color:#D6ECFF; }
        QSlider::groove:horizontal { height:4px; background:#E3E5E8; border-radius:2px; }
        QSlider::handle:horizontal { width:14px; height:14px; background:#1E9FFF; border-radius:7px; margin-top:-5px; margin-bottom:-5px; }
        QProgressBar { background:#EEF0F2; border-radius:6px; text-align:center; color:#3A3D47; font-size:12px; }
        QProgressBar::chunk { background:#1E9FFF; border-radius:6px; }
        QCheckBox { color:#1A1B1F; spacing:6px; }
        QLabel { color:#1A1B1F; }
    )");

    // ---- 信号 ----
    connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onModeChanged);
    connect(addFileBtn, &QPushButton::clicked, this, &MainWindow::onAddFiles);
    connect(addDirBtn,  &QPushButton::clicked, this, &MainWindow::onAddFolder);
    connect(rmBtn,      &QPushButton::clicked, this, &MainWindow::onRemove);
    connect(clrBtn,     &QPushButton::clicked, this, &MainWindow::onClear);
    connect(m_fileList,  &FileDropList::filesDropped, this, &MainWindow::onFilesDropped);
    connect(archiveBtn, &QPushButton::clicked, this, &MainWindow::onBrowseArchive);
    connect(destBtn,    &QPushButton::clicked, this, &MainWindow::onBrowseDest);
    connect(m_formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::onFormatChanged);
    connect(m_levelSlider, &QSlider::valueChanged, this, [this](int val){ m_levelLabel->setText(QString::number(val)); });
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_minBtn, &QPushButton::clicked, this, &MainWindow::onMinimize);
    connect(m_maxBtn, &QPushButton::clicked, this, &MainWindow::onToggleMaximize);
    connect(m_closeBtn, &QPushButton::clicked, this, &QMainWindow::close);

    onModeChanged(0);
}

void MainWindow::onMinimize() { showMinimized(); }

void MainWindow::onToggleMaximize() {
    if (m_maximized) {
        showNormal();
        m_maximized = false;
        m_maxBtn->setText(QStringLiteral("▢"));
    } else {
        showMaximized();
        m_maximized = true;
        m_maxBtn->setText(QStringLiteral("❐"));
    }
}

void MainWindow::onModeChanged(int index) {
    bool compress = (index == 0);
    m_fileList->setEnabled(compress);
    m_destEdit->setEnabled(!compress);
    m_formatCombo->setEnabled(compress);
    m_levelSlider->setEnabled(compress);
    m_archiveEdit->setPlaceholderText(compress
        ? QStringLiteral("选择输出归档文件路径")
        : QStringLiteral("选择要解压的归档文件"));
    m_destEdit->setPlaceholderText(QStringLiteral("解压目标文件夹（留空则自动创建）"));
    m_startBtn->setText(compress ? QStringLiteral("开始压缩") : QStringLiteral("开始解压"));
    m_hintLabel->setText(compress
        ? QStringLiteral("输出为标准归档、不加密，WinRAR、7-Zip、资源管理器等任意解压软件均可直接打开。也可把文件/文件夹直接拖入列表。")
        : QStringLiteral("本程序可解压标准归档；带密码的加密包请使用原压缩软件处理。"));
}

void MainWindow::onAddFiles() {
    QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("选择文件"));
    for (const QString& f : files) m_fileList->addItem(f);
}

void MainWindow::onAddFolder() {
    QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择文件夹"));
    if (!dir.isEmpty()) m_fileList->addItem(dir);
}

void MainWindow::onRemove() {
    for (QListWidgetItem* it : m_fileList->selectedItems())
        delete it;
}

void MainWindow::onClear() {
    m_fileList->clear();
}

void MainWindow::onFilesDropped(const QStringList& paths) {
    if (m_modeCombo->currentIndex() != 0) {
        m_status->setText(QStringLiteral("提示：拖入文件仅用于「压缩」模式"));
        return;
    }
    for (const QString& p : paths) {
        if (!p.isEmpty()) m_fileList->addItem(p);
    }
    m_status->setText(QStringLiteral("已添加 %1 项").arg(paths.size()));
}

void MainWindow::onBrowseArchive() {
    if (m_modeCombo->currentIndex() == 0) {
        QString suffix = defaultSuffixFor(m_formatCombo->currentText());
        QString filter = QStringLiteral("%1 (*.%2);;所有文件 (*.*)").arg(m_formatCombo->currentText(), suffix);
        QString path = QFileDialog::getSaveFileName(this, QStringLiteral("输出归档文件"), QString(), filter);
        if (!path.isEmpty()) m_archiveEdit->setText(path);
    } else {
        QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择归档文件"));
        if (!path.isEmpty()) m_archiveEdit->setText(path);
    }
}

void MainWindow::onBrowseDest() {
    QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择目标文件夹"));
    if (!dir.isEmpty()) m_destEdit->setText(dir);
}

void MainWindow::onFormatChanged(int index) {
    Q_UNUSED(index);
    QString cur = m_archiveEdit->text();
    if (!cur.isEmpty() && !QFileInfo::exists(cur)) {
        QFileInfo fi(cur);
        QString base = fi.completeBaseName().isEmpty() ? cur : fi.path() + "/" + fi.completeBaseName();
        m_archiveEdit->setText(base + "." + defaultSuffixFor(m_formatCombo->currentText()));
    }
}

QString MainWindow::defaultSuffixFor(const QString& label) const {
    if (label == "ZIP")     return "zip";
    if (label == "7Z")      return "7z";
    if (label == "TAR")     return "tar";
    if (label == "TAR.GZ")  return "tar.gz";
    if (label == "TAR.BZ2") return "tar.bz2";
    if (label == "TAR.XZ")  return "tar.xz";
    if (label == "TAR.ZST") return "tar.zst";
    if (label == "GZ")      return "gz";
    if (label == "BZ2")     return "bz2";
    if (label == "XZ")      return "xz";
    if (label == "ZST")     return "zst";
    return "zip";
}

QPair<QString, QString> MainWindow::formatFilterFor(const QString& label) const {
    if (label == "ZIP")     return {"zip", "none"};
    if (label == "7Z")      return {"7zip", "none"};
    if (label == "TAR")     return {"pax", "none"};
    if (label == "TAR.GZ")  return {"pax", "gzip"};
    if (label == "TAR.BZ2") return {"pax", "bzip2"};
    if (label == "TAR.XZ")  return {"pax", "xz"};
    if (label == "TAR.ZST") return {"pax", "zstd"};
    if (label == "GZ")      return {"raw", "gzip"};
    if (label == "BZ2")     return {"raw", "bzip2"};
    if (label == "XZ")      return {"raw", "xz"};
    if (label == "ZST")     return {"raw", "zstd"};
    return {"zip", "none"};
}

void MainWindow::setControlsEnabled(bool enabled) {
    m_modeCombo->setEnabled(enabled);
    m_fileList->setEnabled(enabled && m_modeCombo->currentIndex() == 0);
    m_archiveEdit->setEnabled(enabled);
    m_destEdit->setEnabled(enabled && m_modeCombo->currentIndex() == 1);
    m_formatCombo->setEnabled(enabled && m_modeCombo->currentIndex() == 0);
    m_levelSlider->setEnabled(enabled && m_modeCombo->currentIndex() == 0);
    m_minBtn->setEnabled(enabled);
    m_maxBtn->setEnabled(enabled);
    m_closeBtn->setEnabled(enabled);
    m_startBtn->setEnabled(true); // 始终可点：进行中即取消
    m_busy = !enabled;
}

void MainWindow::onStart() {
    if (m_busy) { // 进行中 -> 取消
        if (m_worker) m_worker->cancel();
        return;
    }

    bool compress = (m_modeCombo->currentIndex() == 0);

    if (compress) {
        QStringList inputs;
        for (int i = 0; i < m_fileList->count(); ++i)
            inputs.append(m_fileList->item(i)->text());
        if (inputs.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先添加要压缩的文件或文件夹"));
            return;
        }
        QString archivePath = m_archiveEdit->text().trimmed();
        if (archivePath.isEmpty()) {
            QFileInfo fi(inputs.first());
            archivePath = fi.absolutePath() + "/archive." + defaultSuffixFor(m_formatCombo->currentText());
            m_archiveEdit->setText(archivePath);
        } else {
            QFileInfo fi(archivePath);
            if (fi.suffix().isEmpty())
                archivePath += "." + defaultSuffixFor(m_formatCombo->currentText());
        }
        auto ff = formatFilterFor(m_formatCombo->currentText());
        CompressOptions opt;
        opt.archivePath = archivePath;
        opt.inputs = inputs;
        opt.format = ff.first;
        opt.filter = ff.second;
        opt.level = m_levelSlider->value();

        m_worker = new ArchiveWorker;
        m_thread = new QThread;
        m_worker->moveToThread(m_thread);
        connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        connect(m_worker, &ArchiveWorker::progress, this, &MainWindow::onProgress);
        connect(m_worker, &ArchiveWorker::finished, this, &MainWindow::onFinished);
        m_worker->setCompress(opt);
        setControlsEnabled(false);
        m_progress->setValue(0);
        m_status->setText(QStringLiteral("正在压缩..."));
        m_startBtn->setText(QStringLiteral("取消"));
        m_thread->start();
        QMetaObject::invokeMethod(m_worker, &ArchiveWorker::run, Qt::QueuedConnection);
    } else {
        QString archivePath = m_archiveEdit->text().trimmed();
        if (archivePath.isEmpty() || !QFileInfo::exists(archivePath)) {
            QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请选择有效的归档文件"));
            return;
        }
        QString destDir = m_destEdit->text().trimmed();
        if (destDir.isEmpty()) {
            QFileInfo fi(archivePath);
            destDir = fi.absolutePath() + "/" + fi.completeBaseName();
        }
        m_worker = new ArchiveWorker;
        m_thread = new QThread;
        m_worker->moveToThread(m_thread);
        connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        connect(m_worker, &ArchiveWorker::progress, this, &MainWindow::onProgress);
        connect(m_worker, &ArchiveWorker::finished, this, &MainWindow::onFinished);
        m_worker->setExtract(archivePath, destDir);
        setControlsEnabled(false);
        m_progress->setValue(0);
        m_status->setText(QStringLiteral("正在解压..."));
        m_startBtn->setText(QStringLiteral("取消"));
        m_thread->start();
        QMetaObject::invokeMethod(m_worker, &ArchiveWorker::run, Qt::QueuedConnection);
    }
}

void MainWindow::onProgress(qint64 done, qint64 total, const QString& current) {
    int pct = (total > 0) ? static_cast<int>(done * 1000 / total) : 0;
    m_progress->setValue(pct);
    m_status->setText(QStringLiteral("%1 / %2  ·  %3").arg(humanBytes(done), humanBytes(total), current));
}

void MainWindow::onFinished(bool ok, const QString& message) {
    m_progress->setValue(ok ? 1000 : m_progress->value());
    if (ok)
        m_status->setText(QStringLiteral("完成：%1").arg(message.isEmpty() ? QStringLiteral("操作成功") : message));
    else
        m_status->setText(QStringLiteral("失败：%1").arg(message.isEmpty() ? QStringLiteral("未知错误") : message));

    setControlsEnabled(true);
    m_startBtn->setText(m_modeCombo->currentIndex() == 0 ? QStringLiteral("开始压缩") : QStringLiteral("开始解压"));

    if (m_thread) {
        m_thread->quit();
        m_thread->wait();
        m_thread->deleteLater();
        m_thread = nullptr;
    }
    m_worker = nullptr;

    if (ok)
        QMessageBox::information(this, QStringLiteral("完成"), m_status->text());
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    Q_UNUSED(eventType);
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_NCHITTEST) {
        const int border = 6;
        long x = GET_X_LPARAM(msg->lParam);
        long y = GET_Y_LPARAM(msg->lParam);
        QPoint g(x, y);
        QRect r = frameGeometry();
        bool left   = g.x() <= r.left() + border;
        bool right  = g.x() >= r.right() - border;
        bool top    = g.y() <= r.top() + border;
        bool bottom = g.y() >= r.bottom() - border;
        *result = 0;
        if (left && top)       *result = HTTOPLEFT;
        else if (left && bottom)  *result = HTBOTTOMLEFT;
        else if (right && top)    *result = HTTOPRIGHT;
        else if (right && bottom) *result = HTBOTTOMRIGHT;
        else if (left)  *result = HTLEFT;
        else if (right) *result = HTRIGHT;
        else if (top)   *result = HTTOP;
        else if (bottom)*result = HTBOTTOM;
        if (*result) return true;
    }
    return false;
}
#endif
