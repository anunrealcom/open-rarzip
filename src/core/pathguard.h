#pragma once

#include "archivetypes.h"

namespace yasuo {
namespace PathGuard {

// 把归档条目名归一化；拒绝绝对路径、盘符、UNC、越界 ".."、控制字符与 Windows 保留名。
// 返回 false 时该条目必须跳过（并记录告警），绝不能静默改写后再落盘。
bool sanitize(const QString& entry, QString& normalized, QString& reason);

// 在 destDir 内解析出安全的本地绝对路径。
bool resolve(const QString& destDir, const QString& entry, QString& out, QString& reason);

// 按覆盖策略得到最终写入路径；skip=true 表示应跳过该条目。
// entryTime 为归档内条目的修改时间，OverwriteIfNewer 依赖它做真实比较；
// 时间缺失时保守跳过，避免用旧文件覆盖新文件。
QString decide(const QString& desiredPath, OverwritePolicy policy, bool& skip,
               const QDateTime& entryTime = QDateTime());

// 创建父目录；失败原因写入 reason。
bool ensureParent(const QString& filePath, QString& reason);

} // namespace PathGuard
} // namespace yasuo
