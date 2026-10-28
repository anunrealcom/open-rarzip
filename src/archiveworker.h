#ifndef ARCHIVEWORKER_H
#define ARCHIVEWORKER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>

#include "archiveengine.h"

// 在独立线程中执行压缩/解压，避免阻塞 UI。
class ArchiveWorker : public QObject {
    Q_OBJECT
public:
    explicit ArchiveWorker(QObject* parent = nullptr);

    void setCompress(const CompressOptions& opt);
    void setExtract(const QString& archivePath, const QString& destDir);

public slots:
    void run();
    void cancel();

signals:
    void progress(qint64 done, qint64 total, const QString& current);
    void finished(bool ok, const QString& message);

private:
    enum Mode { Compress, Extract } m_mode = Compress;
    CompressOptions m_opt;
    QString m_archivePath;
    QString m_destDir;
    std::atomic<bool> m_cancel{false};
};

#endif // ARCHIVEWORKER_H
