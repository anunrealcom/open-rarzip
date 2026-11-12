#pragma once

#include "archivetypes.h"

namespace yasuo {

// 归档引擎统一契约。UI / CLI / 右键菜单只依赖此接口，
// 底层可替换为 libarchive、7-Zip SDK 或任何第三方实现。
class IArchiveEngine {
public:
    virtual ~IArchiveEngine() = default;

    virtual EngineCaps caps() const = 0;

    // 列出归档内容（含元数据），不做数据解压。
    virtual bool list(const QString& archivePath, const QString& password,
                      ArchiveInfo& out, QString& err) = 0;

    // 创建归档。
    virtual bool compress(const CompressOptions& opt,
                          ProgressFn progress, CancelFn cancel, QString& err) = 0;

    // 解压归档（可为部分条目）。
    virtual bool extract(const ExtractOptions& opt,
                         ProgressFn progress, CancelFn cancel, QString& err) = 0;

    // 完整性测试：逐条目读取全部数据并校验校验和。
    virtual bool test(const QString& archivePath, const QString& password,
                      ProgressFn progress, CancelFn cancel, QString& err) = 0;

    // 探测归档真实格式（按文件头魔数，而非扩展名）。
    virtual Format detect(const QString& archivePath) const = 0;

    // 删除归档内条目：names 可为文件或目录（目录级联删除其下所有条目）。
    // 实现必须写临时文件 + 校验 + 原子替换，失败时保持原归档不变；
    // 不支持或会丢失加密语义时应明确拒绝，不得静默降级。
    virtual bool removeEntries(const QString& archivePath, const QStringList& names,
                               const QString& password, QString& err) {
        Q_UNUSED(archivePath); Q_UNUSED(names); Q_UNUSED(password);
        err = QStringLiteral("当前引擎不支持包内删除");
        return false;
    }
};

// 引擎工厂与能力聚合
class Engines {
public:
    // 返回当前构建中可用的最佳引擎（调用方不持有所有权）。
    static IArchiveEngine* best();
    // 按归档类型返回最合适引擎：7z / RAR 优先走 7-Zip 后端（若可用），其余走主引擎。
    static IArchiveEngine* best(const QString& archivePath);
    // 所有可用引擎的能力描述，供设置页展示。
    static QVector<EngineCaps> availableCaps();
    static QStringList writableFormatLabels();
    static QString summary();
};

} // namespace yasuo
