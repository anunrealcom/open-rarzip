#include "taskrunner.h"
#include "iengine.h"
#include "accelerator.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QDir>

namespace yasuo {

TaskRunner::TaskRunner(QObject* parent) : QObject(parent) {}

void TaskRunner::setRequest(const TaskRequest& req) {
    m_req = req;
    m_cancel.store(false);
}

void TaskRunner::cancel() {
    m_cancel.store(true);
}

void TaskRunner::run() {
    QElapsedTimer timer;
    timer.start();
    QString err;
    bool ok = false;
    IArchiveEngine* engine = Engines::best();

    const CancelFn cancel = [this]() { return m_cancel.load(); };
    const ProgressFn progress = [this](qint64 done, qint64 total, const QString& cur) {
        emit this->progress(done, total, cur);
    };

    switch (m_req.kind) {
    case JobKind::Compress:
        ok = engine->compress(m_req.compress, progress, cancel, err);
        break;
    case JobKind::Extract:
        ok = engine->extract(m_req.extract, progress, cancel, err);
        break;
    case JobKind::Test:
        ok = engine->test(m_req.archivePath, m_req.password, progress, cancel, err);
        break;
    case JobKind::Remove:
        ok = engine->removeEntries(m_req.archivePath, m_req.removeEntries,
                                   m_req.password, err);
        break;
    case JobKind::Browse:
        ok = true;
        break;
    }

    if (!ok && err.isEmpty())
        err = m_cancel.load() ? QStringLiteral("已取消") : QStringLiteral("未知错误");

    QString message = err;
    if (ok) {
        switch (m_req.kind) {
        case JobKind::Compress: {
            const QFileInfo fi(m_req.compress.archivePath);
            message = QStringLiteral("已生成 %1（%2）").arg(fi.fileName(), humanBytes(fi.size()));
            break;
        }
        case JobKind::Extract:
            message = QStringLiteral("已解压到 %1").arg(QDir::toNativeSeparators(m_req.extract.destDir));
            break;
        case JobKind::Test:
            message = QStringLiteral("完整性测试通过，归档可完整读取");
            break;
        case JobKind::Remove:
            message = QStringLiteral("已从归档删除 %1 个条目")
                          .arg(m_req.removeEntries.size());
            break;
        default:
            message = QStringLiteral("完成");
            break;
        }
    }
    emit finished(ok, message, timer.elapsed());
}

} // namespace yasuo
