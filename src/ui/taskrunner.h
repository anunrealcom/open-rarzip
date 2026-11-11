#pragma once

#include <QObject>
#include <atomic>
#include "archivetypes.h"

namespace yasuo {

struct TaskRequest {
    JobKind kind = JobKind::Compress;
    CompressOptions compress;
    ExtractOptions extract;
    QString archivePath;   // Test / Remove 用
    QString password;      // Test / Remove 用
    QStringList removeEntries; // Remove 用
};

class TaskRunner : public QObject {
    Q_OBJECT
public:
    explicit TaskRunner(QObject* parent = nullptr);

    void setRequest(const TaskRequest& req);
    void cancel();

public slots:
    void run();

signals:
    void progress(qint64 done, qint64 total, const QString& current);
    void finished(bool ok, const QString& message, qint64 elapsedMs);

private:
    TaskRequest m_req;
    std::atomic<bool> m_cancel{false};
};

} // namespace yasuo
