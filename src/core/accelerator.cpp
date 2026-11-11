#include "accelerator.h"

#include <zlib.h>

#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QLibrary>
#include <QThread>
#include <QElapsedTimer>
#include <QtConcurrent>
#include <QCoreApplication>
#include <algorithm>

namespace yasuo {
namespace {

constexpr qint64 kChunkSize = 4 * 1024 * 1024;

struct ChunkResult {
    int index = 0;
    quint32 crc = 0;
    qint64 length = 0;
    bool ok = false;
};

ChunkResult crcChunk(const QString& path, int index, qint64 offset, qint64 length) {
    ChunkResult r;
    r.index = index;
    r.length = length;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return r;
    if (!f.seek(offset)) return r;
    QByteArray buf(64 * 1024, Qt::Uninitialized);
    qint64 remaining = length;
    quint32 crc = crc32(0L, Z_NULL, 0);
    while (remaining > 0) {
        const qint64 want = qMin<qint64>(buf.size(), remaining);
        const qint64 got = f.read(buf.data(), want);
        if (got <= 0) return r;
        crc = crc32(crc, reinterpret_cast<const Bytef*>(buf.constData()), static_cast<uInt>(got));
        remaining -= got;
    }
    r.crc = crc;
    r.ok = true;
    return r;
}

quint32 combineParallel(const QString& path, int threads, CancelFn cancel, bool& ok) {
    ok = false;
    QFile info(path);
    const qint64 size = info.size();
    if (size <= 0) return crc32(0L, Z_NULL, 0);

    // 线程数决定并发度，但绝不能减少总工作量：
    // 按「文件长度 / 线程数」动态切块，单线程时就是完整一块。
    const int maxChunks = static_cast<int>((size + kChunkSize - 1) / kChunkSize);
    const int chunks = qMax(1, qMin(qMax(1, threads), maxChunks));
    const qint64 chunkSize = (size + chunks - 1) / chunks;
    QVector<QFuture<ChunkResult>> futures;
    futures.reserve(chunks);
    for (int i = 0; i < chunks; ++i) {
        const qint64 offset = static_cast<qint64>(i) * chunkSize;
        const qint64 length = qMin<qint64>(chunkSize, size - offset);
        if (length <= 0) break;
        futures.append(QtConcurrent::run(crcChunk, path, i, offset, length));
    }
    QVector<ChunkResult> results;
    results.reserve(futures.size());
    for (auto& f : futures) {
        if (cancel && cancel()) { ok = false; return 0; }
        results.append(f.result());
    }
    std::sort(results.begin(), results.end(),
              [](const ChunkResult& a, const ChunkResult& b) { return a.index < b.index; });

    quint32 acc = crc32(0L, Z_NULL, 0);
    for (const ChunkResult& r : results) {
        if (!r.ok) return 0;
        acc = crc32_combine(acc, r.crc, static_cast<z_off_t>(r.length));
    }
    ok = true;
    return acc;
}

// 只做能力探测，不链接 CUDA/nvCOMP：
// NVIDIA nvCOMP SDK 许可禁止将其置于开源许可证约束下，且仅限 NVIDIA GPU 系统使用。
// 因此本项目只提供插件接口，由用户自行选择是否安装外部插件。
bool probeGpuEnvironment(QString& detail) {
    bool hasCudaDriver = false;
    bool hasNvcomp = false;
#ifdef Q_OS_WIN
    QLibrary cuda(QStringLiteral("nvcuda.dll"));
    hasCudaDriver = cuda.load();
    QLibrary nvcomp(QStringLiteral("nvcomp.dll"));
    hasNvcomp = nvcomp.load();
#endif
    if (hasCudaDriver && hasNvcomp)
        detail = QStringLiteral("检测到 CUDA 驱动与 nvCOMP 运行时");
    else if (hasCudaDriver)
        detail = QStringLiteral("检测到 CUDA 驱动，但未安装 nvCOMP 运行时");
    else
        detail = QStringLiteral("未检测到 NVIDIA CUDA 环境");
    return hasCudaDriver && hasNvcomp;
}

QString gpuPluginPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/plugins/yasuo_gpu_nvcomp.dll");
}

} // namespace

QVector<AccelBackend> Accelerator::backends() {
    QVector<AccelBackend> list;
    const int cores = QThread::idealThreadCount() > 0 ? QThread::idealThreadCount() : 4;

    AccelBackend cpu;
    cpu.id = QStringLiteral("cpu-mt");
    cpu.name = QStringLiteral("CPU 多线程 + SIMD");
    cpu.detail = QStringLiteral("文件级并行调度、分块流水线 CRC、AES-NI 加密；对 ZIP/7z/RAR 这类串行熵编码最有效");
    cpu.available = true;
    cpu.gpu = false;
    cpu.threads = cores;
    list.append(cpu);

    QString detail;
    const bool envOk = probeGpuEnvironment(detail);
    QLibrary plugin(gpuPluginPath());
    const bool pluginLoaded = plugin.load();
    CreateGpuAcceleratorFn create = nullptr;
    if (pluginLoaded) {
        create = reinterpret_cast<CreateGpuAcceleratorFn>(
            plugin.resolve("yasuo_create_gpu_accelerator"));
    }

    AccelBackend gpu;
    gpu.id = QStringLiteral("gpu-nvcomp");
    gpu.name = QStringLiteral("GPU 加速（可选插件）");
    gpu.detail = detail +
        (pluginLoaded
            ? (create ? QStringLiteral("；已加载外部加速插件")
                      : QStringLiteral("；插件缺少导出符号"))
            : QStringLiteral("；未安装外部插件（主程序不含任何专有 SDK）"));
    gpu.available = envOk && pluginLoaded && create != nullptr;
    gpu.gpu = true;
    gpu.threads = 1;
    list.append(gpu);
    return list;
}

QString Accelerator::activeId() {
    QSettings s;
    return s.value(QStringLiteral("accel/id"), QStringLiteral("cpu-mt")).toString();
}

void Accelerator::setActiveId(const QString& id) {
    QSettings s;
    s.setValue(QStringLiteral("accel/id"), id);
}

AccelBackend Accelerator::active() {
    const QString id = activeId();
    const QVector<AccelBackend> all = backends();
    for (const AccelBackend& b : all)
        if (b.id == id && b.available) return b;
    return all.first();
}

QString Accelerator::statusText() {
    const AccelBackend b = active();
    return QStringLiteral("%1 · %2 线程").arg(b.name).arg(b.threads);
}

int Accelerator::idealThreads() {
    const AccelBackend b = active();
    return qMax(1, b.threads);
}

quint32 Accelerator::parallelCrc32(const QString& filePath, int threads,
                                   CancelFn cancel, bool& ok) {
    return combineParallel(filePath, qMax(1, threads), cancel, ok);
}

bool Accelerator::batchChecksum(const QVector<QByteArray>& chunks,
                                QVector<quint32>& out, QString& err) {
    out.clear();
    out.reserve(chunks.size());
    const AccelBackend b = active();

    if (b.gpu && b.available) {
        QLibrary plugin(gpuPluginPath());
        if (plugin.load()) {
            auto create = reinterpret_cast<CreateGpuAcceleratorFn>(
                plugin.resolve("yasuo_create_gpu_accelerator"));
            if (create) {
                IGpuAccelerator* acc = create();
                if (acc && acc->available()) {
                    const bool ok = acc->computeChecksums(chunks, out, err);
                    delete acc;
                    if (ok) return true;
                    // 插件失败时自动回退 CPU，不中断任务
                    err.clear();
                } else {
                    delete acc;
                }
            }
        }
    }

    for (const QByteArray& c : chunks)
        out.append(static_cast<quint32>(crc32(0L,
            reinterpret_cast<const Bytef*>(c.constData()), static_cast<uInt>(c.size()))));
    return true;
}

double Accelerator::benchmarkCrc(const QString& filePath, int threads) {
    QElapsedTimer t;
    t.start();
    bool ok = false;
    parallelCrc32(filePath, threads, {}, ok);
    const qint64 size = QFileInfo(filePath).size();
    const double seconds = t.elapsed() / 1000.0;
    if (seconds <= 0 || size <= 0) return 0.0;
    return (static_cast<double>(size) / (1024.0 * 1024.0)) / seconds;
}

} // namespace yasuo
