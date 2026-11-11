#pragma once

#include <QWidget>
#include <QAbstractTableModel>
#include <QVector>
#include <QSet>
#include "archivetypes.h"

class QTreeView;
class QLineEdit;
class QLabel;
class QPushButton;
class QPlainTextEdit;

namespace yasuo {

class ArchiveModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit ArchiveModel(QObject* parent = nullptr);

    void setEntries(const QVector<EntryInfo>& entries);
    void setFilter(const QString& text);
    void checkAll(bool checked);
    QStringList checkedNames() const;
    EntryInfo entryAt(const QModelIndex& index) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
    void rebuildFilter();
    QVector<EntryInfo> m_entries;
    QVector<int> m_visible;
    QSet<int> m_checked;
    QString m_filter;
};

class ArchiveBrowser : public QWidget {
    Q_OBJECT
public:
    explicit ArchiveBrowser(QWidget* parent = nullptr);

public slots:
    void openArchive(const QString& path);
    void refresh();

signals:
    void requestExtract(const QString& archivePath, const QStringList& entries,
                        const QString& password);
    void requestTest(const QString& archivePath, const QString& password);
    void requestRemove(const QString& archivePath, const QStringList& entries,
                       const QString& password);
    void statusMessage(const QString& text);

private slots:
    void onPickArchive();
    void onExtractSelected();
    void onExtractAll();
    void onTest();
    void onDeleteSelected();
    void onSearchChanged(const QString& text);
    void onDoubleClicked(const QModelIndex& index);

private:
    void preview(const EntryInfo& entry);
    void updateSummary();

    QLineEdit* m_pathEdit = nullptr;
    QLineEdit* m_passwordEdit = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QTreeView* m_view = nullptr;
    ArchiveModel* m_model = nullptr;
    QLabel* m_summary = nullptr;
    QPlainTextEdit* m_preview = nullptr;
    QPushButton* m_extractBtn = nullptr;
    QPushButton* m_deleteBtn = nullptr;
    ArchiveInfo m_info;
};

} // namespace yasuo
