#include "archivebrowser.h"
#include "iengine.h"
#include "pathguard.h"

#include <QTreeView>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QHeaderView>
#include <QDateTime>
#include <QApplication>

namespace yasuo {

ArchiveModel::ArchiveModel(QObject* parent) : QAbstractTableModel(parent) {}

void ArchiveModel::setEntries(const QVector<EntryInfo>& entries) {
    beginResetModel();
    m_entries = entries;
    m_checked.clear();
    rebuildFilter();
    endResetModel();
}

void ArchiveModel::rebuildFilter() {
    m_visible.clear();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_filter.isEmpty() ||
            m_entries.at(i).path.contains(m_filter, Qt::CaseInsensitive))
            m_visible.append(i);
    }
}

void ArchiveModel::setFilter(const QString& text) {
    beginResetModel();
    m_filter = text;
    rebuildFilter();
    endResetModel();
}

void ArchiveModel::checkAll(bool checked) {
    m_checked.clear();
    if (checked)
        for (int idx : m_visible) m_checked.insert(idx);
    emit dataChanged(index(0, 0), index(rowCount() - 1, 0));
}

QStringList ArchiveModel::checkedNames() const {
    QStringList names;
    for (int idx : m_checked)
        if (idx >= 0 && idx < m_entries.size()) names.append(m_entries.at(idx).path);
    return names;
}

EntryInfo ArchiveModel::entryAt(const QModelIndex& index) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_visible.size())
        return EntryInfo{};
    return m_entries.at(m_visible.at(index.row()));
}

int ArchiveModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : m_visible.size();
}

int ArchiveModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 4;
}

QVariant ArchiveModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    const int src = m_visible.at(index.row());
    const EntryInfo& e = m_entries.at(src);

    if (role == Qt::CheckStateRole && index.column() == 0)
        return m_checked.contains(src) ? Qt::Checked : Qt::Unchecked;

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return e.path;
        case 1: return e.isDir ? QStringLiteral("<目录>") : humanBytes(e.size);
        case 2: return e.compressedSize > 0 ? humanBytes(e.compressedSize) : QStringLiteral("—");
        case 3: return e.modified.isValid() ? e.modified.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                           : QStringLiteral("—");
        }
    }
    if (role == Qt::ToolTipRole)
        return QStringLiteral("%1\n原始 %2 · 压缩后 %3%4")
            .arg(e.path, humanBytes(e.size), humanBytes(e.compressedSize),
                 e.encrypted ? QStringLiteral(" · 已加密") : QString());
    if (role == Qt::TextAlignmentRole && (index.column() == 1 || index.column() == 2))
        return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
    return {};
}

bool ArchiveModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::CheckStateRole || index.column() != 0 || !index.isValid()) return false;
    const int src = m_visible.at(index.row());
    if (value.toInt() == Qt::Checked) m_checked.insert(src);
    else m_checked.remove(src);
    emit dataChanged(index, index);
    return true;
}

QVariant ArchiveModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case 0: return QStringLiteral("名称（勾选后仅解压所选）");
    case 1: return QStringLiteral("原始大小");
    case 2: return QStringLiteral("压缩后");
    case 3: return QStringLiteral("修改时间");
    }
    return {};
}

Qt::ItemFlags ArchiveModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == 0) f |= Qt::ItemIsUserCheckable;
    return f;
}

ArchiveBrowser::ArchiveBrowser(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 18);
    root->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("归档浏览与选择性解压"));
    title->setObjectName(QStringLiteral("titleLabel"));
    root->addWidget(title);

    auto* pathRow = new QHBoxLayout;
    m_pathEdit = new QLineEdit;
    m_pathEdit->setPlaceholderText(QStringLiteral("选择或拖入归档文件（ZIP / 7Z / RAR / TAR / GZ…）"));
    auto* pick = new QPushButton(QStringLiteral("打开归档"));
    auto* refresh = new QPushButton(QStringLiteral("刷新"));
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(pick);
    pathRow->addWidget(refresh);
    root->addLayout(pathRow);

    auto* optRow = new QHBoxLayout;
    m_searchEdit = new QLineEdit;
    m_searchEdit->setPlaceholderText(QStringLiteral("筛选条目名称…"));
    m_passwordEdit = new QLineEdit;
    m_passwordEdit->setPlaceholderText(QStringLiteral("密码（加密归档填写）"));
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setMaximumWidth(220);
    optRow->addWidget(m_searchEdit, 1);
    optRow->addWidget(m_passwordEdit);
    root->addLayout(optRow);

    m_summary = new QLabel(QStringLiteral("尚未打开归档"));
    m_summary->setObjectName(QStringLiteral("subLabel"));
    root->addWidget(m_summary);

    m_model = new ArchiveModel(this);
    m_view = new QTreeView;
    m_view->setModel(m_model);
    m_view->setAlternatingRowColors(true);
    m_view->setRootIsDecorated(false);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setSortingEnabled(false);
    m_view->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_view->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_view->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_view->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_view->setMinimumHeight(220);
    root->addWidget(m_view, 1);

    m_preview = new QPlainTextEdit;
    m_preview->setReadOnly(true);
    m_preview->setMaximumHeight(96);
    m_preview->setPlaceholderText(QStringLiteral("双击条目可快速预览文本内容"));
    root->addWidget(m_preview);

    auto* btnRow = new QHBoxLayout;
    auto* all = new QPushButton(QStringLiteral("全选"));
    auto* none = new QPushButton(QStringLiteral("清空选择"));
    m_extractBtn = new QPushButton(QStringLiteral("解压所选"));
    m_extractBtn->setObjectName(QStringLiteral("primary"));
    auto* extractAll = new QPushButton(QStringLiteral("解压全部"));
    auto* testBtn = new QPushButton(QStringLiteral("测试完整性"));
    m_deleteBtn = new QPushButton(QStringLiteral("删除所选"));
    m_deleteBtn->setEnabled(false);
    m_deleteBtn->setToolTip(QStringLiteral("从归档中永久删除勾选或当前选中的条目"));
    btnRow->addWidget(all);
    btnRow->addWidget(none);
    btnRow->addStretch(1);
    btnRow->addWidget(testBtn);
    btnRow->addWidget(m_deleteBtn);
    btnRow->addWidget(extractAll);
    btnRow->addWidget(m_extractBtn);
    root->addLayout(btnRow);

    connect(pick, &QPushButton::clicked, this, &ArchiveBrowser::onPickArchive);
    connect(refresh, &QPushButton::clicked, this, &ArchiveBrowser::refresh);
    connect(m_pathEdit, &QLineEdit::editingFinished, this, [this]() { openArchive(m_pathEdit->text()); });
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ArchiveBrowser::onSearchChanged);
    connect(m_view, &QTreeView::doubleClicked, this, &ArchiveBrowser::onDoubleClicked);
    connect(all, &QPushButton::clicked, this, [this]() { m_model->checkAll(true); });
    connect(none, &QPushButton::clicked, this, [this]() { m_model->checkAll(false); });
    connect(m_extractBtn, &QPushButton::clicked, this, &ArchiveBrowser::onExtractSelected);
    connect(extractAll, &QPushButton::clicked, this, &ArchiveBrowser::onExtractAll);
    connect(testBtn, &QPushButton::clicked, this, &ArchiveBrowser::onTest);
    connect(m_deleteBtn, &QPushButton::clicked, this, &ArchiveBrowser::onDeleteSelected);
}

void ArchiveBrowser::openArchive(const QString& path) {
    if (path.trimmed().isEmpty()) return;
    m_pathEdit->setText(path);
    if (!QFileInfo::exists(path)) {
        m_summary->setText(QStringLiteral("文件不存在: %1").arg(path));
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    ArchiveInfo info;
    const bool ok = Engines::best(path)->list(path, m_passwordEdit->text(), info, err);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        m_summary->setText(QStringLiteral("打开失败: %1").arg(err));
        m_model->setEntries({});
        m_deleteBtn->setEnabled(false);
        emit statusMessage(QStringLiteral("打开归档失败: %1").arg(err));
        return;
    }
    m_info = info;
    m_model->setEntries(info.entries);
    // 包内删除只支持可重写格式；加密归档与 RAR（只读）必须禁用，避免静默降级
    m_deleteBtn->setEnabled(Engines::best(path)->caps().editable && !info.encrypted &&
                            info.format != Format::Rar &&
                            info.format != Format::Unknown);
    updateSummary();
    emit statusMessage(QStringLiteral("已打开 %1 · %2 个条目").arg(QFileInfo(path).fileName()).arg(info.entryCount));
}

void ArchiveBrowser::refresh() { openArchive(m_pathEdit->text()); }

void ArchiveBrowser::updateSummary() {
    const QString text = QStringLiteral("%1 · %2 个条目 · 原始 %3 · 文件 %4%5")
        .arg(m_info.formatName, QString::number(m_info.entryCount),
             humanBytes(m_info.totalSize), humanBytes(m_info.totalCompressed),
             m_info.encrypted ? QStringLiteral(" · 含加密条目") : QString());
    m_summary->setText(text);
}

void ArchiveBrowser::onPickArchive() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择归档文件"), QString(),
        QStringLiteral("归档文件 (*.zip *.7z *.rar *.tar *.gz *.bz2 *.xz *.zst *.tgz *.txz *.tbz2 *.tzst);;所有文件 (*.*)"));
    if (!path.isEmpty()) openArchive(path);
}

void ArchiveBrowser::onSearchChanged(const QString& text) { m_model->setFilter(text); }

void ArchiveBrowser::onExtractSelected() {
    if (m_info.path.isEmpty()) return;
    const QStringList names = m_model->checkedNames();
    if (names.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("请先勾选要解压的条目，或使用「解压全部」"));
        return;
    }
    emit requestExtract(m_info.path, names, m_passwordEdit->text());
}

void ArchiveBrowser::onExtractAll() {
    if (m_info.path.isEmpty()) return;
    emit requestExtract(m_info.path, {}, m_passwordEdit->text());
}

void ArchiveBrowser::onTest() {
    if (m_info.path.isEmpty()) return;
    emit requestTest(m_info.path, m_passwordEdit->text());
}

void ArchiveBrowser::onDeleteSelected() {
    if (m_info.path.isEmpty() || !m_deleteBtn->isEnabled()) return;
    QStringList names = m_model->checkedNames();
    if (names.isEmpty()) {
        const EntryInfo e = m_model->entryAt(m_view->currentIndex());
        if (!e.path.isEmpty()) names.append(e.path);
    }
    if (names.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"),
            QStringLiteral("请先勾选要删除的条目，或选中表格中的一行"));
        return;
    }
    const auto ret = QMessageBox::question(
        this, QStringLiteral("确认删除"),
        QStringLiteral("将从「%1」中永久删除 %2 个条目（目录会连带其下所有文件），此操作不可撤销。")
            .arg(QFileInfo(m_info.path).fileName()).arg(names.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;
    emit requestRemove(m_info.path, names, m_passwordEdit->text());
}

void ArchiveBrowser::onDoubleClicked(const QModelIndex& index) {
    preview(m_model->entryAt(index));
}

void ArchiveBrowser::preview(const EntryInfo& entry) {
    if (entry.path.isEmpty() || entry.isDir || m_info.path.isEmpty()) return;
    if (entry.encrypted && m_passwordEdit->text().isEmpty()) {
        m_preview->setPlainText(QStringLiteral("该条目已加密，请先在上方填写密码。"));
        return;
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) return;
    ExtractOptions opt;
    opt.archivePath = m_info.path;
    opt.destDir = tmp.path();
    opt.password = m_passwordEdit->text();
    opt.selectedEntries = QStringList{ entry.path };
    opt.overwrite = OverwritePolicy::Overwrite;
    opt.verifyCrc = false;
    QString err;
    if (!Engines::best(opt.archivePath)->extract(opt, {}, {}, err)) {
        m_preview->setPlainText(QStringLiteral("预览失败: %1").arg(err));
        return;
    }
    QString outPath, why;
    if (!PathGuard::resolve(tmp.path(), entry.path, outPath, why)) {
        m_preview->setPlainText(QStringLiteral("预览失败: %1").arg(why));
        return;
    }
    QFile f(outPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray data = f.read(64 * 1024);
    f.close();
    bool printable = true;
    for (char c : data) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x09 || (u > 0x0D && u < 0x20)) { printable = false; break; }
    }
    if (printable) {
        const QString text = QString::fromUtf8(data);
        m_preview->setPlainText(text.left(4000) +
            (data.size() > 4000 ? QStringLiteral("\n…（仅显示前 4KB）") : QString()));
    } else {
        m_preview->setPlainText(
            QStringLiteral("二进制文件：%1\n大小 %2\n前 64 字节（十六进制）:\n%3")
                .arg(entry.path, humanBytes(entry.size),
                     QByteArray(data.left(64).toHex()).insert(32, '\n')));
    }
}

} // namespace yasuo
