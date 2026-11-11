#include "mainwindow.h"
#include "archivebrowser.h"
#include "theme.h"
#include "iengine.h"
#include "accelerator.h"
#include "shellintegration.h"

#include <QApplication>
#include <QStackedWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QButtonGroup>
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QStatusBar>
#include <QIcon>
#include <QPixmap>
#include <QButtonGroup>
#include <QAbstractItemView>
#include <QToolButton>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#endif

using namespace yasuo;

namespace {

class TitleBar : public QWidget {
public:
    explicit TitleBar(QWidget* win, QWidget* parent = nullptr)
        : QWidget(parent), m_win(win) {}
    QHBoxLayout* layout = nullptr;
protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            m_drag = e->globalPosition().toPoint() - m_win->frameGeometry().topLeft();
            m_down = true;
        }
        QWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_down && (e->buttons() & Qt::LeftButton)) {
            auto* w = qobject_cast<MainWindow*>(m_win);
            if (w && w->isMaximized()) w->showNormal();
            m_win->move(e->globalPosition().toPoint() - m_drag);
        }
        QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent*) override { m_down = false; }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            if (auto* w = qobject_cast<MainWindow*>(m_win))
                w->isMaximized() ? w->showNormal() : w->showMaximized();
        }
        QWidget::mouseDoubleClickEvent(e);
    }
private:
    QWidget* m_win;
    QPoint m_drag;
    bool m_down = false;
};

QString defaultSuffix(Format f) { return formatSuffix(f); }

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowSystemMenuHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setMinimumSize(980, 660);
    resize(1120, 720);
    setAcceptDrops(true);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.ico")));
    setWindowTitle(QStringLiteral("压索 · Yasuo Compressor"));
    Theme::apply(Theme::isDark());
    buildUi();
    refreshEngineInfo();
    refreshShellStatus();
    syncCapabilityUI();

    m_remoteTimer = new QTimer(this);
    m_remoteTimer->setSingleShot(true);
    m_remoteTimer->setInterval(280);
    connect(m_remoteTimer, &QTimer::timeout, this, &MainWindow::onPendingRemoteArgs);
}

MainWindow::~MainWindow() {
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(2000);
    }
}

QWidget* MainWindow::buildTitleBar() {
    auto* bar = new TitleBar(this);
    bar->setObjectName(QStringLiteral("card"));
    bar->setFixedHeight(58);
    auto* h = new QHBoxLayout(bar);
    bar->layout = h;
    h->setContentsMargins(16, 0, 12, 0);
    h->setSpacing(10);

    auto* logo = new QLabel;
    const QPixmap pm(QStringLiteral(":/icons/logo.png"));
    if (!pm.isNull()) logo->setPixmap(pm.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else logo->setText(QStringLiteral("压"));
    logo->setFixedSize(34, 34);
    h->addWidget(logo);

    auto* brand = new QLabel(QStringLiteral("压索  Yasuo Compressor"));
    brand->setObjectName(QStringLiteral("brandLabel"));
    h->addWidget(brand);
    auto* sub = new QLabel(QStringLiteral("开源 · 免费 · 全格式"));
    sub->setObjectName(QStringLiteral("hintLabel"));
    h->addWidget(sub);
    h->addStretch(1);

    m_themeToggle = new QPushButton(Theme::isDark() ? QStringLiteral("浅色") : QStringLiteral("深色"));
    m_themeToggle->setObjectName(QStringLiteral("winBtn"));
    m_themeToggle->setFixedSize(52, 28);
    h->addWidget(m_themeToggle);

    m_minBtn = new QPushButton(QStringLiteral("—"));
    m_maxBtn = new QPushButton(QStringLiteral("□"));
    m_closeBtn = new QPushButton(QStringLiteral("✕"));
    for (QPushButton* b : { m_minBtn, m_maxBtn, m_closeBtn }) {
        b->setObjectName(b == m_closeBtn ? QStringLiteral("closeBtn") : QStringLiteral("winBtn"));
        b->setFixedSize(34, 28);
    }
    h->addWidget(m_minBtn); h->addWidget(m_maxBtn); h->addWidget(m_closeBtn);

    connect(m_themeToggle, &QPushButton::clicked, this, &MainWindow::onToggleTheme);
    connect(m_minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(m_maxBtn, &QPushButton::clicked, this, [this]() {
        isMaximized() ? showNormal() : showMaximized();
        m_maximized = isMaximized();
    });
    connect(m_closeBtn, &QPushButton::clicked, this, &QWidget::close);
    return bar;
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("pageRoot"));
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(12, 12, 12, 8);
    root->setSpacing(10);
    root->addWidget(buildTitleBar());

    auto* body = new QHBoxLayout;
    body->setSpacing(12);

    // 左侧导航
    auto* nav = new QWidget;
    nav->setObjectName(QStringLiteral("card"));
    nav->setFixedWidth(148);
    auto* navLayout = new QVBoxLayout(nav);
    navLayout->setContentsMargins(10, 14, 10, 14);
    navLayout->setSpacing(6);
    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);
    const QStringList navItems = {
        QStringLiteral("  压缩文件"), QStringLiteral("  归档浏览"),
        QStringLiteral("  任务进度"), QStringLiteral("  设置")
    };
    for (int i = 0; i < navItems.size(); ++i) {
        auto* b = new QPushButton(navItems.at(i));
        b->setObjectName(QStringLiteral("navBtn"));
        b->setCheckable(true);
        b->setMinimumHeight(42);
        m_navGroup->addButton(b, i);
        navLayout->addWidget(b);
    }
    m_navGroup->button(0)->setChecked(true);
    navLayout->addStretch(1);
    auto* navHint = new QLabel(QStringLiteral("支持拖入文件\nCtrl + 多选"));
    navHint->setObjectName(QStringLiteral("hintLabel"));
    navHint->setAlignment(Qt::AlignCenter);
    navLayout->addWidget(navHint);
    body->addWidget(nav);

    m_stack = new QStackedWidget;
    m_stack->addWidget(buildCompressPage());   // 0
    m_browser = new ArchiveBrowser;            // 1
    m_stack->addWidget(m_browser);
    m_stack->addWidget(buildTaskPage());       // 2
    m_stack->addWidget(buildSettingsPage());   // 3
    body->addWidget(m_stack, 1);
    root->addLayout(body, 1);

    auto* bar = new QStatusBar;
    m_statusEngine = new QLabel;
    m_statusAccel = new QLabel;
    m_statusEngine->setObjectName(QStringLiteral("hintLabel"));
    m_statusAccel->setObjectName(QStringLiteral("hintLabel"));
    bar->addWidget(m_statusEngine, 1);
    bar->addPermanentWidget(m_statusAccel);
    setStatusBar(bar);
    m_statusEngine->setText(Engines::best()->caps().displayName);
    m_statusAccel->setText(Accelerator::statusText());

    connect(m_navGroup, &QButtonGroup::idClicked, this, &MainWindow::onNavChanged);
    connect(m_browser, &ArchiveBrowser::requestExtract, this, &MainWindow::onExtractRequested);
    connect(m_browser, &ArchiveBrowser::requestTest, this, &MainWindow::onTestRequested);
    connect(m_browser, &ArchiveBrowser::requestRemove, this, &MainWindow::onRemoveRequested);
    connect(m_browser, &ArchiveBrowser::statusMessage,
            this, [this](const QString& s) { appendLog(s); });
}

QWidget* MainWindow::buildCompressPage() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("创建压缩包"));
    title->setObjectName(QStringLiteral("titleLabel"));
    v->addWidget(title);

    auto* top = new QHBoxLayout;
    top->setSpacing(12);
    auto* listCard = new QWidget;
    listCard->setObjectName(QStringLiteral("card"));
    auto* lc = new QVBoxLayout(listCard);
    lc->setContentsMargins(14, 14, 14, 14);
    auto* listTitle = new QLabel(QStringLiteral("待压缩的文件与文件夹（可直接拖入窗口）"));
    listTitle->setObjectName(QStringLiteral("subLabel"));
    lc->addWidget(listTitle);
    m_fileList = new QListWidget;
    m_fileList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_fileList->setMinimumHeight(180);
    lc->addWidget(m_fileList);
    auto* row = new QHBoxLayout;
    auto* addF = new QPushButton(QStringLiteral("添加文件"));
    auto* addD = new QPushButton(QStringLiteral("添加文件夹"));
    auto* rm = new QPushButton(QStringLiteral("移除"));
    auto* clr = new QPushButton(QStringLiteral("清空"));
    row->addWidget(addF); row->addWidget(addD); row->addWidget(rm); row->addWidget(clr);
    row->addStretch(1);
    lc->addLayout(row);
    top->addWidget(listCard, 3);

    auto* optCard = new QWidget;
    optCard->setObjectName(QStringLiteral("card"));
    auto* oc = new QVBoxLayout(optCard);
    oc->setContentsMargins(14, 14, 14, 14);
    oc->setSpacing(9);
    auto* optTitle = new QLabel(QStringLiteral("压缩选项"));
    optTitle->setObjectName(QStringLiteral("subLabel"));
    oc->addWidget(optTitle);

    m_formatCombo = new QComboBox;
    for (Format f : Engines::best()->caps().writeFormats)
        m_formatCombo->addItem(formatName(f), static_cast<int>(f));
    oc->addWidget(new QLabel(QStringLiteral("格式")));
    oc->addWidget(m_formatCombo);

    auto* levelRow = new QHBoxLayout;
    levelRow->addWidget(new QLabel(QStringLiteral("级别")));
    m_levelSlider = new QSlider(Qt::Horizontal);
    m_levelSlider->setRange(0, 9);
    m_levelSlider->setValue(6);
    m_levelLabel = new QLabel(QStringLiteral("6"));
    m_levelLabel->setFixedWidth(18);
    levelRow->addWidget(m_levelSlider, 1);
    levelRow->addWidget(m_levelLabel);
    oc->addLayout(levelRow);

    auto* threadRow = new QHBoxLayout;
    threadRow->addWidget(new QLabel(QStringLiteral("编码线程")));
    m_threadsSpin = new QSpinBox;
    m_threadsSpin->setRange(0, 64);
    m_threadsSpin->setValue(0);
    m_threadsSpin->setSpecialValueText(QStringLiteral("自动"));
    threadRow->addWidget(m_threadsSpin, 1);
    oc->addLayout(threadRow);

    oc->addWidget(new QLabel(QStringLiteral("密码（仅 ZIP 支持）")));
    m_passwordEdit = new QLineEdit;
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(QStringLiteral("留空则不加密"));
    oc->addWidget(m_passwordEdit);

    m_encryptionCombo = new QComboBox;
    m_encryptionCombo->addItem(encryptionName(Encryption::Aes256), static_cast<int>(Encryption::Aes256));
    m_encryptionCombo->addItem(encryptionName(Encryption::Aes128), static_cast<int>(Encryption::Aes128));
    m_encryptionCombo->addItem(encryptionName(Encryption::ZipCrypto), static_cast<int>(Encryption::ZipCrypto));
    oc->addWidget(m_encryptionCombo);
    oc->addStretch(1);
    top->addWidget(optCard, 2);
    v->addLayout(top, 1);

    auto* adv = new QGroupBox(QStringLiteral("高级"));
    adv->setCheckable(false);
    auto* grid = new QGridLayout(adv);
    grid->setContentsMargins(14, 16, 14, 12);
    grid->setSpacing(8);
    grid->addWidget(new QLabel(QStringLiteral("排除规则")), 0, 0);
    m_excludeEdit = new QLineEdit(QStringLiteral("*.tmp *.log"));
    m_excludeEdit->setToolTip(QStringLiteral("空格分隔的 glob 片段，例如 *.tmp Thumbs.db"));
    grid->addWidget(m_excludeEdit, 0, 1);
    m_verifyCheck = new QCheckBox(QStringLiteral("压缩后自检"));
    m_verifyCheck->setChecked(true);
    grid->addWidget(m_verifyCheck, 0, 2);
    m_deleteCheck = new QCheckBox(QStringLiteral("完成后删除源文件"));
    m_deleteCheck->setToolTip(QStringLiteral("危险操作：仅删除顶层文件，不会删除文件夹"));
    grid->addWidget(m_deleteCheck, 1, 0);
    m_solidCheck = new QCheckBox(QStringLiteral("固实压缩"));
    grid->addWidget(m_solidCheck, 1, 1);
    m_encryptNamesCheck = new QCheckBox(QStringLiteral("加密文件名"));
    grid->addWidget(m_encryptNamesCheck, 1, 2);
    v->addWidget(adv);

    auto* outRow = new QHBoxLayout;
    outRow->addWidget(new QLabel(QStringLiteral("输出")));
    m_outputEdit = new QLineEdit;
    m_outputEdit->setPlaceholderText(QStringLiteral("留空则根据第一个输入自动生成"));
    auto* browse = new QPushButton(QStringLiteral("浏览…"));
    outRow->addWidget(m_outputEdit, 1);
    outRow->addWidget(browse);
    v->addLayout(outRow);

    m_capHint = new QLabel;
    m_capHint->setObjectName(QStringLiteral("hintLabel"));
    m_capHint->setWordWrap(true);
    v->addWidget(m_capHint);

    auto* actionRow = new QHBoxLayout;
    actionRow->addStretch(1);
    m_startBtn = new QPushButton(QStringLiteral("开始压缩"));
    m_startBtn->setObjectName(QStringLiteral("primary"));
    m_startBtn->setMinimumWidth(160);
    actionRow->addWidget(m_startBtn);
    v->addLayout(actionRow);

    connect(addF, &QPushButton::clicked, this, &MainWindow::onAddFiles);
    connect(addD, &QPushButton::clicked, this, &MainWindow::onAddFolder);
    connect(rm, &QPushButton::clicked, this, &MainWindow::onRemoveSelected);
    connect(clr, &QPushButton::clicked, this, &MainWindow::onClearFiles);
    connect(browse, &QPushButton::clicked, this, &MainWindow::onBrowseOutput);
    connect(m_formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onFormatChanged);
    connect(m_levelSlider, &QSlider::valueChanged, this,
            [this](int n) { m_levelLabel->setText(QString::number(n)); });
    connect(m_startBtn, &QPushButton::clicked, this, &MainWindow::onStartCompress);
    return page;
}

QWidget* MainWindow::buildTaskPage() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("任务进度"));
    title->setObjectName(QStringLiteral("titleLabel"));
    v->addWidget(title);

    auto* card = new QWidget;
    card->setObjectName(QStringLiteral("card"));
    auto* c = new QVBoxLayout(card);
    c->setContentsMargins(16, 16, 16, 16);
    c->setSpacing(10);
    m_taskStatus = new QLabel(QStringLiteral("当前没有运行中的任务"));
    m_taskStatus->setObjectName(QStringLiteral("titleLabel"));
    c->addWidget(m_taskStatus);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setValue(0);
    c->addWidget(m_progress);
    m_taskSpeed = new QLabel(QStringLiteral("速度 — · 剩余 —"));
    m_taskSpeed->setObjectName(QStringLiteral("subLabel"));
    c->addWidget(m_taskSpeed);
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    m_cancelBtn = new QPushButton(QStringLiteral("取消任务"));
    m_cancelBtn->setObjectName(QStringLiteral("danger"));
    m_cancelBtn->setEnabled(false);
    btnRow->addWidget(m_cancelBtn);
    c->addLayout(btnRow);
    v->addWidget(card);

    auto* logTitle = new QLabel(QStringLiteral("操作日志"));
    logTitle->setObjectName(QStringLiteral("subLabel"));
    v->addWidget(logTitle);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    v->addWidget(m_log, 1);
    connect(m_cancelBtn, &QPushButton::clicked, this, &MainWindow::onCancelTask);
    return page;
}

QWidget* MainWindow::buildSettingsPage() {
    auto* page = new QWidget;
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(12);
    auto* title = new QLabel(QStringLiteral("设置与关于"));
    title->setObjectName(QStringLiteral("titleLabel"));
    v->addWidget(title);

    auto* two = new QHBoxLayout;
    two->setSpacing(12);

    auto* left = new QWidget;
    left->setObjectName(QStringLiteral("card"));
    auto* lc = new QVBoxLayout(left);
    lc->setContentsMargins(16, 16, 16, 16);
    lc->setSpacing(10);
    lc->addWidget(new QLabel(QStringLiteral("加速引擎")));
    m_accelCombo = new QComboBox;
    lc->addWidget(m_accelCombo);
    m_accelDetail = new QLabel;
    m_accelDetail->setObjectName(QStringLiteral("hintLabel"));
    m_accelDetail->setWordWrap(true);
    lc->addWidget(m_accelDetail);
    auto* bench = new QPushButton(QStringLiteral("运行校验吞吐基准"));
    lc->addWidget(bench);
    m_benchResult = new QLabel(QStringLiteral("尚未运行"));
    m_benchResult->setObjectName(QStringLiteral("subLabel"));
    lc->addWidget(m_benchResult);
    lc->addSpacing(8);
    lc->addWidget(new QLabel(QStringLiteral("资源管理器集成")));
    m_shellStatus = new QLabel;
    m_shellStatus->setObjectName(QStringLiteral("subLabel"));
    m_shellStatus->setWordWrap(true);
    lc->addWidget(m_shellStatus);
    auto* shellRow = new QHBoxLayout;
    auto* install = new QPushButton(QStringLiteral("安装右键菜单"));
    install->setObjectName(QStringLiteral("primary"));
    auto* uninstall = new QPushButton(QStringLiteral("卸载"));
    shellRow->addWidget(install);
    shellRow->addWidget(uninstall);
    lc->addLayout(shellRow);
    lc->addStretch(1);
    two->addWidget(left, 1);

    auto* right = new QWidget;
    right->setObjectName(QStringLiteral("card"));
    auto* rc = new QVBoxLayout(right);
    rc->setContentsMargins(16, 16, 16, 16);
    rc->setSpacing(10);
    rc->addWidget(new QLabel(QStringLiteral("当前引擎能力")));
    m_engineInfo = new QLabel;
    m_engineInfo->setObjectName(QStringLiteral("subLabel"));
    m_engineInfo->setWordWrap(true);
    rc->addWidget(m_engineInfo);
    rc->addSpacing(8);
    rc->addWidget(new QLabel(QStringLiteral("关于")));
    auto* about = new QLabel(QStringLiteral(
        "压索 Yasuo Compressor · 开源免费压缩软件\n"
        "MIT 许可证 · Qt 6 (LGPLv3) · libarchive (BSD)\n\n"
        "RAR 为 RARLAB 专有格式：本软件仅解压 RAR，不会生成 RAR，"
        "这是遵守 unRAR 许可的主动设计。要创建高压缩率归档请使用 7Z 或 ZIP。\n\n"
        "GPU 加速以可选外部插件形式提供，主程序不捆绑任何专有 SDK。"));
    about->setWordWrap(true);
    about->setObjectName(QStringLiteral("subLabel"));
    rc->addWidget(about);
    rc->addStretch(1);
    two->addWidget(right, 1);
    v->addLayout(two, 1);

    for (const AccelBackend& b : Accelerator::backends())
        m_accelCombo->addItem(b.name + (b.available ? QString() : QStringLiteral("（不可用）")), b.id);
    const int idx = m_accelCombo->findData(Accelerator::activeId());
    if (idx >= 0) m_accelCombo->setCurrentIndex(idx);
    m_accelDetail->setText(Accelerator::active().detail);

    connect(m_accelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::onAccelChanged);
    connect(bench, &QPushButton::clicked, this, &MainWindow::onBenchmark);
    connect(install, &QPushButton::clicked, this, &MainWindow::onInstallShell);
    connect(uninstall, &QPushButton::clicked, this, &MainWindow::onUninstallShell);
    return page;
}

void MainWindow::onNavChanged(int id) {
    m_stack->setCurrentIndex(id);
}

void MainWindow::onAddFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("选择文件"));
    for (const QString& f : files)
        if (m_fileList->findItems(f, Qt::MatchExactly).isEmpty()) m_fileList->addItem(f);
}

void MainWindow::onAddFolder() {
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("选择文件夹"));
    if (!d.isEmpty() && m_fileList->findItems(d, Qt::MatchExactly).isEmpty())
        m_fileList->addItem(d);
}

void MainWindow::onRemoveSelected() {
    for (QListWidgetItem* it : m_fileList->selectedItems()) delete it;
}

void MainWindow::onClearFiles() { m_fileList->clear(); }

void MainWindow::onBrowseOutput() {
    const Format f = currentFormat();
    const QString filter = QStringLiteral("%1 (*.%2);;所有文件 (*.*)").arg(formatName(f), defaultSuffix(f));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("输出归档"), QString(), filter);
    if (!path.isEmpty()) m_outputEdit->setText(path);
}

Format MainWindow::currentFormat() const {
    if (!m_formatCombo || m_formatCombo->currentIndex() < 0) return Format::Zip;
    return static_cast<Format>(m_formatCombo->currentData().toInt());
}

void MainWindow::onFormatChanged(int) {
    const QString cur = m_outputEdit->text();
    if (!cur.isEmpty()) {
        QFileInfo fi(cur);
        QString base = fi.path() + QLatin1Char('/') + fi.completeBaseName();
        m_outputEdit->setText(base + QLatin1Char('.') + defaultSuffix(currentFormat()));
    }
    syncCapabilityUI();
}

void MainWindow::syncCapabilityUI() {
    const EngineCaps c = Engines::best()->caps();
    const Format f = currentFormat();
    const bool zipCrypto = c.encryption && f == Format::Zip;
    m_passwordEdit->setEnabled(zipCrypto);
    m_encryptionCombo->setEnabled(zipCrypto);
    m_solidCheck->setEnabled(c.solid);
    m_encryptNamesCheck->setEnabled(false);
    m_threadsSpin->setEnabled(c.multiThread);
    m_encryptNamesCheck->setToolTip(QStringLiteral("加密文件名需要 7-Zip SDK 后端（尚未启用）"));
    m_solidCheck->setToolTip(c.solid ? QString() : QStringLiteral("固实压缩需要 7-Zip SDK 后端"));

    QStringList hints;
    hints << QStringLiteral("输出为标准开放格式，WinRAR / 7-Zip / 系统资源管理器均可直接打开");
    if (!zipCrypto && !m_passwordEdit->text().isEmpty())
        hints << QStringLiteral("当前格式不支持加密，密码将被忽略");
    if (f == Format::SevenZip)
        hints << QStringLiteral("7Z 使用 LZMA2；如需 AES-256 加密请选择 ZIP");
    if (!c.multiThread)
        hints << QStringLiteral("当前引擎未启用多线程编码");
    m_capHint->setText(hints.join(QStringLiteral("  ·  ")));
}

void MainWindow::onStartCompress() {
    QStringList inputs;
    for (int i = 0; i < m_fileList->count(); ++i) inputs.append(m_fileList->item(i)->text());
    if (inputs.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先添加文件或文件夹"));
        return;
    }
    const Format f = currentFormat();
    QString out = m_outputEdit->text().trimmed();
    if (out.isEmpty()) {
        QFileInfo first(inputs.first());
        out = first.absolutePath() + QLatin1Char('/') +
              (first.isDir() ? first.fileName() : first.completeBaseName()) +
              QLatin1Char('.') + defaultSuffix(f);
        m_outputEdit->setText(out);
    } else if (QFileInfo(out).suffix().isEmpty()) {
        out += QLatin1Char('.') + defaultSuffix(f);
        m_outputEdit->setText(out);
    }
    if (QFileInfo::exists(out)) {
        const auto ret = QMessageBox::question(this, QStringLiteral("覆盖确认"),
            QStringLiteral("文件已存在，是否覆盖？\n%1").arg(out));
        if (ret != QMessageBox::Yes) return;
    }

    CompressOptions opt;
    opt.archivePath = out;
    opt.inputs = inputs;
    opt.format = f;
    opt.level = m_levelSlider->value();
    opt.threads = m_threadsSpin->value();
    opt.password = m_passwordEdit->isEnabled() ? m_passwordEdit->text() : QString();
    opt.encryption = static_cast<Encryption>(m_encryptionCombo->currentData().toInt());
    opt.excludes = m_excludeEdit->text().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    opt.verifyAfter = m_verifyCheck->isChecked();
    opt.deleteAfter = m_deleteCheck->isChecked();
    opt.solid = m_solidCheck->isEnabled() && m_solidCheck->isChecked();

    TaskRequest req;
    req.kind = JobKind::Compress;
    req.compress = opt;
    startTask(req);
    m_stack->setCurrentIndex(2);
}

void MainWindow::startTask(const TaskRequest& req) {
    if (m_busy) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("已有任务在运行，请先完成或取消"));
        return;
    }
    m_currentJob = req.kind;
    m_thread = new QThread(this);
    m_runner = new TaskRunner;
    m_runner->setRequest(req);
    m_runner->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_runner, &QObject::deleteLater);
    connect(m_runner, &TaskRunner::progress, this, &MainWindow::onProgress);
    connect(m_runner, &TaskRunner::finished, this, &MainWindow::onFinished);
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    setBusy(true);
    m_progress->setValue(0);
    m_taskStatus->setText(req.kind == JobKind::Compress ? QStringLiteral("正在压缩…")
                          : req.kind == JobKind::Extract ? QStringLiteral("正在解压…")
                          : QStringLiteral("正在测试归档…"));
    m_taskSpeed->setText(QStringLiteral("速度 — · 剩余 —"));
    appendLog(QStringLiteral("开始：%1").arg(m_taskStatus->text()));
    m_taskStartMs = QDateTime::currentMSecsSinceEpoch();
    m_lastDone = 0;
    m_thread->start();
    QMetaObject::invokeMethod(m_runner, &TaskRunner::run, Qt::QueuedConnection);
}

void MainWindow::setBusy(bool busy) {
    m_busy = busy;
    m_startBtn->setEnabled(!busy);
    m_cancelBtn->setEnabled(busy);
    m_startBtn->setText(busy ? QStringLiteral("任务进行中") : QStringLiteral("开始压缩"));
}

void MainWindow::onProgress(qint64 done, qint64 total, const QString& current) {
    qint64 raw = total > 0 ? done * 1000 / total : 0;
    raw = qMax<qint64>(0, qMin<qint64>(1000, raw));
    const int pct = static_cast<int>(raw);
    m_progress->setValue(pct);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double elapsed = qMax<qint64>(1, now - m_taskStartMs) / 1000.0;
    const double mbps = (static_cast<double>(done) / (1024.0 * 1024.0)) / elapsed;
    const double remain = mbps > 0.01 ? (static_cast<double>(total - done) / (1024.0 * 1024.0)) / mbps : 0;
    m_taskSpeed->setText(QStringLiteral("速度 %1 MB/s · 剩余 %2 秒 · %3")
        .arg(mbps, 0, 'f', 1).arg(remain, 0, 'f', 0).arg(current));
    m_statusAccel->setText(QStringLiteral("%1 · %2 MB/s").arg(Accelerator::statusText()).arg(mbps, 0, 'f', 1));
}

void MainWindow::onFinished(bool ok, const QString& message, qint64 elapsedMs) {
    m_progress->setValue(ok ? 1000 : m_progress->value());
    m_taskStatus->setText(ok ? QStringLiteral("完成") : QStringLiteral("失败"));
    m_taskSpeed->setText(QStringLiteral("耗时 %1 秒").arg(elapsedMs / 1000.0, 0, 'f', 2));
    appendLog(QStringLiteral("%1：%2").arg(ok ? QStringLiteral("完成") : QStringLiteral("失败"), message));
    setBusy(false);
    if (m_thread) {
        m_thread->quit();
        m_thread->wait(3000);
        m_thread = nullptr;
    }
    m_runner = nullptr;
    m_statusAccel->setText(Accelerator::statusText());
    if (ok && m_currentJob == JobKind::Remove)
        m_browser->refresh(); // 包内删除成功后重新列出条目
    if (!ok) QMessageBox::warning(this, QStringLiteral("任务失败"), message);
}

void MainWindow::onCancelTask() {
    if (m_runner) m_runner->cancel();
    appendLog(QStringLiteral("正在取消任务…"));
}

void MainWindow::appendLog(const QString& text) {
    if (m_log)
        m_log->appendPlainText(QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), text));
}

void MainWindow::onToggleTheme() {
    const bool dark = !Theme::isDark();
    Theme::setDark(dark);
    Theme::apply(dark);
    m_themeToggle->setText(dark ? QStringLiteral("浅色") : QStringLiteral("深色"));
}

void MainWindow::onAccelChanged(int index) {
    const QString id = m_accelCombo->itemData(index).toString();
    for (const AccelBackend& b : Accelerator::backends()) {
        if (b.id != id) continue;
        m_accelDetail->setText(b.detail);
        if (!b.available) {
            m_accelDetail->setText(b.detail + QStringLiteral("\n该后端不可用，已自动保持 CPU 多线程。"));
            return;
        }
        Accelerator::setActiveId(id);
        m_statusAccel->setText(Accelerator::statusText());
        return;
    }
}

void MainWindow::onBenchmark() {
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("选择用于基准测试的文件（只读，不修改）"));
    if (file.isEmpty()) return;
    const int threads = Accelerator::idealThreads();
    const double single = Accelerator::benchmarkCrc(file, 1);
    const double parallel = Accelerator::benchmarkCrc(file, threads);
    m_benchResult->setText(QStringLiteral("单线程 %1 MB/s → %2 线程 %3 MB/s（提升 %4×）")
        .arg(single, 0, 'f', 1).arg(threads).arg(parallel, 0, 'f', 1)
        .arg(single > 0 ? parallel / single : 0, 0, 'f', 2));
    appendLog(QStringLiteral("校验基准：%1").arg(m_benchResult->text()));
}

void MainWindow::refreshEngineInfo() {
    if (m_engineInfo) m_engineInfo->setText(Engines::summary());
    if (m_statusEngine) m_statusEngine->setText(Engines::best()->caps().displayName);
}

void MainWindow::refreshShellStatus() {
    if (m_shellStatus) m_shellStatus->setText(ShellIntegration::statusText() +
        QStringLiteral("\n条目：") + ShellIntegration::menuLabels().join(QStringLiteral(" / ")));
}

void MainWindow::onInstallShell() {
    QString err;
    const bool ok = ShellIntegration::install(err);
    refreshShellStatus();
    QMessageBox::information(this, QStringLiteral("右键菜单"),
        ok ? QStringLiteral("已添加到资源管理器右键菜单，立即生效（若未显示请重启资源管理器）")
           : QStringLiteral("安装失败：%1").arg(err));
}

void MainWindow::onUninstallShell() {
    QString err;
    const bool ok = ShellIntegration::uninstall(err);
    refreshShellStatus();
    QMessageBox::information(this, QStringLiteral("右键菜单"),
        ok ? QStringLiteral("已移除全部右键菜单项") : QStringLiteral("卸载失败：%1").arg(err));
}

void MainWindow::onExtractRequested(const QString& archive, const QStringList& entries,
                                    const QString& password) {
    QFileInfo fi(archive);
    QString dest = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择解压目标文件夹"), fi.absolutePath());
    if (dest.isEmpty()) dest = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName();

    // 目标目录已有内容时才询问覆盖策略，避免每次解压都打断用户。
    OverwritePolicy policy = OverwritePolicy::Rename;
    const QDir targetDir(dest);
    if (targetDir.exists() && !targetDir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        QMessageBox box(this);
        box.setWindowTitle(QStringLiteral("同名文件处理"));
        box.setText(QStringLiteral("目标文件夹已有内容，遇到同名文件时如何处理？"));
        auto* renameBtn = box.addButton(QStringLiteral("自动改名（推荐）"), QMessageBox::AcceptRole);
        auto* overwriteBtn = box.addButton(QStringLiteral("直接覆盖"), QMessageBox::AcceptRole);
        auto* newerBtn = box.addButton(QStringLiteral("仅覆盖较新"), QMessageBox::AcceptRole);
        auto* skipBtn = box.addButton(QStringLiteral("跳过"), QMessageBox::AcceptRole);
        auto* cancelBtn = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(renameBtn);
        box.exec();
        const QAbstractButton* clicked = box.clickedButton();
        if (!clicked || clicked == cancelBtn) return;
        if (clicked == overwriteBtn) policy = OverwritePolicy::Overwrite;
        else if (clicked == newerBtn) policy = OverwritePolicy::OverwriteIfNewer;
        else if (clicked == skipBtn) policy = OverwritePolicy::Skip;
    }

    ExtractOptions opt;
    opt.archivePath = archive;
    opt.destDir = dest;
    opt.password = password;
    opt.selectedEntries = entries;
    opt.overwrite = policy;
    TaskRequest req;
    req.kind = JobKind::Extract;
    req.extract = opt;
    startTask(req);
    m_stack->setCurrentIndex(2);
}

void MainWindow::onTestRequested(const QString& archive, const QString& password) {
    TaskRequest req;
    req.kind = JobKind::Test;
    req.archivePath = archive;
    req.password = password;
    startTask(req);
    m_stack->setCurrentIndex(2);
}

void MainWindow::onRemoveRequested(const QString& archive, const QStringList& entries,
                                   const QString& password) {
    TaskRequest req;
    req.kind = JobKind::Remove;
    req.archivePath = archive;
    req.password = password;
    req.removeEntries = entries;
    startTask(req);
    m_stack->setCurrentIndex(2);
}

void MainWindow::quickCompressFiles(const QStringList& paths) {
    for (const QString& p : paths)
        if (!p.isEmpty() && QFileInfo::exists(p) &&
            m_fileList->findItems(p, Qt::MatchExactly).isEmpty())
            m_fileList->addItem(p);
    m_stack->setCurrentIndex(0);
    m_navGroup->button(0)->setChecked(true);
    raise();
    activateWindow();
}

void MainWindow::quickExtractArchive(const QString& path) {
    if (!QFileInfo::exists(path)) return;
    QFileInfo fi(path);
    ExtractOptions opt;
    opt.archivePath = path;
    opt.destDir = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName();
    opt.overwrite = OverwritePolicy::Rename;
    TaskRequest req;
    req.kind = JobKind::Extract;
    req.extract = opt;
    startTask(req);
    m_stack->setCurrentIndex(2);
    raise();
    activateWindow();
}

void MainWindow::showArchive(const QString& path) {
    m_stack->setCurrentIndex(1);
    m_navGroup->button(1)->setChecked(true);
    m_browser->openArchive(path);
    raise();
    activateWindow();
}

void MainWindow::handleRemoteArgs(const QStringList& args) {
    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args.at(i);
        const QString next = (i + 1 < args.size()) ? args.at(i + 1) : QString();
        if (a == QLatin1String("--quick-compress") && !next.isEmpty())
            m_remoteCompressPaths.append(next);
        else if (a == QLatin1String("--quick-extract") && !next.isEmpty())
            quickExtractArchive(next);
        else if (a == QLatin1String("--open") && !next.isEmpty())
            m_remoteOpenPaths.append(next);
    }
    if (!m_remoteCompressPaths.isEmpty()) m_remoteTimer->start();
    if (!m_remoteOpenPaths.isEmpty()) {
        showArchive(m_remoteOpenPaths.last());
        m_remoteOpenPaths.clear();
    }
}

void MainWindow::onPendingRemoteArgs() {
    if (m_remoteCompressPaths.isEmpty()) return;
    quickCompressFiles(m_remoteCompressPaths);
    m_remoteCompressPaths.clear();
}

void MainWindow::installShellOnFirstRun() {
    QSettings s;
    if (s.value(QStringLiteral("shell/firstRunDone"), false).toBool()) return;
    s.setValue(QStringLiteral("shell/firstRunDone"), true);
    if (ShellIntegration::isInstalled()) return;
    QString err;
    const bool ok = ShellIntegration::install(err);
    refreshShellStatus();
    appendLog(ok ? QStringLiteral("已自动集成资源管理器右键菜单")
                 : QStringLiteral("右键菜单自动集成失败：%1").arg(err));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const QUrl& u : event->mimeData()->urls())
        if (u.isLocalFile()) paths.append(u.toLocalFile());
    if (paths.isEmpty()) return;
    event->acceptProposedAction();
    // 单个归档文件直接打开浏览，其余进入压缩列表
    if (paths.size() == 1 && Engines::best()->detect(paths.first()) != Format::Unknown &&
        formatFromSuffix(paths.first()) != Format::Unknown) {
        showArchive(paths.first());
        return;
    }
    quickCompressFiles(paths);
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray&, void* message, qintptr* result) {
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_NCHITTEST) {
        const int border = 6;
        const QPoint g(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam));
        const QRect r = frameGeometry();
        const bool l = g.x() <= r.left() + border, rr = g.x() >= r.right() - border;
        const bool t = g.y() <= r.top() + border, b = g.y() >= r.bottom() - border;
        *result = 0;
        if (l && t) *result = HTTOPLEFT;
        else if (l && b) *result = HTBOTTOMLEFT;
        else if (rr && t) *result = HTTOPRIGHT;
        else if (rr && b) *result = HTBOTTOMRIGHT;
        else if (l) *result = HTLEFT;
        else if (rr) *result = HTRIGHT;
        else if (t) *result = HTTOP;
        else if (b) *result = HTBOTTOM;
        if (*result) return true;
    }
    return false;
}
#endif
