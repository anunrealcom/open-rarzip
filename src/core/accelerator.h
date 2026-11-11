#pragma once

#include "archivetypes.h"
#include <QByteArray>
#include <QVector>

namespace yasuo {

// 加速后端描述。gpu=true 的后端只通过外部可选插件提供，
// 主程序不链接、不分发任何专有 SDK，保证 MIT 发行合规。
struct AccelBackend {
    QString id;
    QString name;
    QString detail;
    bool available = false;
    bool gpu = false;
    int threads = 1;
};

// 可选 GPU 插件契约：第三方插件导出 C 函数 yasuo_create_gpu_accelerator()，
// 返回实现此接口的对象。插件不属于本项目，也不随安装包分发。
class IGpuAccelerator {
public:
    virtual ~IGpuAccelerator() = default;
    virtual QString name() const = 0;
    virtual bool available() const = 0;
    virtual bool computeChecksums(const QVector<QByteArray>& chunks,
                                  QVector<quint32>& out, QString& err) = 0;
};
using CreateGpuAcceleratorFn = IGpuAccelerator* (*)();

class Accelerator {
public:
    static QVector<AccelBackend> backends();
    static QString activeId();
    static void setActiveId(const QString& id);
    static AccelBackend active();
    static QString statusText();
    static int idealThreads();

    // 分块并行 CRC32（zlib crc32_combine 按序合并），用于压缩后校验与完整性测试。
    static quint32 parallelCrc32(const QString& filePath, int threads,
                                 CancelFn cancel, bool& ok);

    // 批量校验和：有可用 GPU 插件时交给插件，否则走 CPU 并行。
    static bool batchChecksum(const QVector<QByteArray>& chunks,
                              QVector<quint32>& out, QString& err);

    // 简易吞吐基准，返回 MB/s，供设置页展示。
    static double benchmarkCrc(const QString& filePath, int threads);
};

} // namespace yasuo
