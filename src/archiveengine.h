#ifndef ARCHIVEENGINE_H
#define ARCHIVEENGINE_H

#include <QString>
#include <QStringList>
#include <functional>
#include <cstdint>

// 进度回调：已处理字节数 / 总字节数 / 当前正在处理的条目名
using ProgressFn = std::function<void(qint64 done, qint64 total, const QString& current)>;

// 取消检查：返回 true 表示用户请求取消
using CancelFn = std::function<bool()>;

struct CompressOptions {
    QString archivePath;   // 输出归档文件路径
    QStringList inputs;    // 待压缩的文件/文件夹路径
    QString format;        // libarchive 格式名: zip / 7zip / pax / raw ...
    QString filter;        // libarchive 过滤名: none / gzip / bzip2 / xz / zstd / lz4
    int level = -1;         // 压缩级别 0..9，-1 表示默认
};

namespace ArchiveEngine {

// 压缩 inputs 到 archivePath。成功返回 true，失败信息写入 err。
bool compress(const CompressOptions& opt, ProgressFn progress, CancelFn cancel, QString& err);

// 解压 archivePath 到 destDir。成功返回 true，失败信息写入 err。
bool extract(const QString& archivePath, const QString& destDir,
             ProgressFn progress, CancelFn cancel, QString& err);

// 根据归档扩展名猜测格式标签（用于 UI 提示）。
QString detectFormatLabel(const QString& archivePath);

// 当前引擎实际支持的格式标签（UI 据此启用/灰显下拉项）。
QVector<QString> supportedFormats();

}

#endif // ARCHIVEENGINE_H
