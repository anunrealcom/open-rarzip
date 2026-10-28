#include "archiveworker.h"

ArchiveWorker::ArchiveWorker(QObject* parent) : QObject(parent) {}

void ArchiveWorker::setCompress(const CompressOptions& opt) {
    m_mode = Compress;
    m_opt = opt;
}

void ArchiveWorker::setExtract(const QString& archivePath, const QString& destDir) {
    m_mode = Extract;
    m_archivePath = archivePath;
    m_destDir = destDir;
}

void ArchiveWorker::cancel() {
    m_cancel.store(true);
}

void ArchiveWorker::run() {
    QString err;
    bool ok = false;
    CancelFn cancel = [this]() { return m_cancel.load(); };
    ProgressFn progress = [this](qint64 done, qint64 total, const QString& cur) {
        emit this->progress(done, total, cur);
    };

    if (m_mode == Compress) {
        ok = ArchiveEngine::compress(m_opt, progress, cancel, err);
    } else {
        ok = ArchiveEngine::extract(m_archivePath, m_destDir, progress, cancel, err);
    }

    if (m_cancel.load() && ok == false && err.isEmpty())
        err = QStringLiteral("已取消");
    emit finished(ok, err);
}
