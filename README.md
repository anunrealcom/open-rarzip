# 压索 Yasuo Compressor（open-rarzip）

基于 Qt 6 的开源压缩软件，目标是对标商业压缩软件的日常体验，作为 RAR 生态的国产开源替代。

> **定位边界（重要）**：RAR 压缩算法为 RARLAB 专有，官方 unRAR 源码许可明确禁止
> 「用于重建 RAR 压缩算法 / 开发 WinRAR 兼容压缩器」。因此本软件**只解压 RAR / RAR5，
> 从不生成 RAR**；原生输出格式是 7Z、ZIP、TAR 及 TAR.GZ / BZ2 / XZ / ZST、单流 GZ / BZ2 / XZ / ZST。
> 项目不包含、不链接、不分发 unRAR 或 WinRAR 的任何代码。

## 能力矩阵

| 能力 | 全格式引擎（libarchive 3.9） | 内置回退引擎 |
| --- | --- | --- |
| 写入格式 | 11 种（ZIP / 7Z / TAR / TAR.\* / GZ / BZ2 / XZ / ZST） | ZIP、GZ |
| 读取格式 | 上述 11 种 + RAR / RAR5（**只读**） | ZIP、GZ |
| 加密写入 | ZIP WinZip AES-256 / AES-128 / ZipCrypto（Windows CNG 后端） | 不支持 |
| 加密 RAR / 7z AES / 加密文件名 | 不支持，需未来的 7-Zip SDK 后端 | 不支持 |
| 固实压缩 / 分卷 | 不支持 | 不支持 |
| ZIP64（>4GB） | 支持 | 不支持，超过 4GB 主动报错 |
| 并行 | `zip:threads` / `zstd:threads` | 文件级并行 + 分块 CRC |
| 流式读写 | 64KB 缓冲，内存占用恒定 | 64KB 缓冲，内存占用恒定 |
| 安全 | PathGuard：拒绝 `..` / 绝对路径 / 盘符 / UNC / 保留设备名 / 控制字符，双重前缀校验 | 同左 |
| 包内删除 | ZIP / 7Z / TAR 系列，重写 + 校验 + 原子替换；加密归档明确拒绝 | 不支持 |
| 覆盖策略 | 覆盖 / 跳过 / 自动改名 / 仅覆盖较新（按条目时间真实比较） | 同左 |
| 取消 / 失败 | 临时文件原子替换，不留下半成品包 | 同左 |

两套引擎共用 `IArchiveEngine` 接口（`src/core/iengine.h`），UI、CLI 与右键菜单只依赖该接口，
后续可插入 7-Zip SDK 后端而无需改动上层。

## 目录结构

```
CMakeLists.txt              Qt/zlib/libarchive 自动探测；两个可执行目标
build_deps_local.sh         离线静态编译 libarchive / bzip2 / xz / zstd → deps/dist
src/core/                   引擎层：iengine / archivetypes / pathguard / accelerator
                            + libarchiveengine（全格式）/ zlibengine（回退）/ enginefactory
src/ui/                     单窗口、归档浏览器、任务调度、主题
src/shell/                  Windows 右键菜单、文件关联、单实例 IPC
src/resources/              图标、清单、rc
test_engine.cpp             无界面引擎自测（含系统 bsdtar 交叉验证）
.github/workflows/build.yml CI：零依赖回退引擎构建 + 自测
scripts/package.bat         windeployqt + ISCC 一键打包
installer/setup.iss         Inno Setup 6 安装包脚本
THIRD_PARTY_NOTICES.txt     第三方组件与许可声明
```

## 构建

前置：Qt 6（MinGW 64 位）、CMake ≥ 3.20、Ninja。Qt 前缀与 MinGW sysroot 会自动探测
（`QT_ROOT_DIR` / `QT_ROOT` / `D:/Qt` / `C:/Qt` / `E:/Qt`），不写死 Qt 版本或工具链目录名。
zlib 优先用系统库，否则回退到 MinGW sysroot 自带的静态库。

### A. 全格式引擎（推荐）

`deps/src` 与 `deps/dist` 不入库（见 `.gitignore`，约 120MB）。源码缺失时脚本会打印
固定 commit 的获取命令，取回后执行：

```bash
bash build_deps_local.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/test_engine.exe
```

脚本启用 Windows CNG（`bcrypt`）作为 AES 后端，并把静态库装到 `deps/dist`；
CMake 检测到 `deps/dist/include/archive.h` 与 `libarchive.a` 即自动启用全格式引擎。

### B. 零外部依赖回退路径（CI 使用）

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYASUO_ENABLE_LIBARCHIVE=OFF
cmake --build build -j4
./build/test_engine.exe
```

该路径只编译内置流式 ZIP/GZ 引擎，用于验证「没有 libarchive 也能完整构建并工作」。

## 自测

`test_engine` 覆盖路径穿越防护、双引擎往返（含中文名 / 空文件 / 空目录）、CRC 完整性测试、
AES-256 ZIP 的「无密码 / 错误密码拒绝 + 正确密码还原」、7Z / TAR / TAR.GZ / TAR.XZ / TAR.ZST
写入读回、包内删除（单条 / 目录级联 / 失败原子回滚 / 加密归档拒绝）、覆盖策略四种行为、
8MB 流式压缩与 SHA1 校验、并行 CRC 与 zlib 一致性、拒绝生成 RAR、
外部工具生成的 UTF-8 中文名 ZIP 回归（防止中文条目被静默丢弃），
以及用 Windows 自带 bsdtar 对**两套引擎各自**产出的 ZIP 做第三方交叉验证。

当前结果：全格式引擎 **71 项全通过**，回退引擎 **33 项全通过**，两套配置的 `ctest` 均通过。
`ctest` 亦可直接运行：`ctest --test-dir build --output-on-failure`。

## 使用

- **GUI**：压缩 / 解压 / 浏览三种模式，拖拽、格式选择、压缩级别、密码、排除规则、覆盖策略、进度与取消。
- **CLI**：`YasuoCompressor --cli <list|test|compress|extract> [参数...]`
- **右键菜单**：`--install-shell` / `--uninstall-shell`（写 HKCU，无需管理员权限）；首次启动也会兜底注册。
- **安装包**：运行 `scripts\package.bat [构建目录] [Qt 前缀]`，脚本从 CMakeCache 自动定位 Qt，
  用 `windeployqt` 把运行时部署到 `build/deploy/`，再调用 Inno Setup 6 编译 `installer/setup.iss`
  到 `dist/`。未安装 ISCC 时只完成运行时部署并给出提示，也可用 `ISCC` 环境变量指定编译器路径。
  安装包为用户级安装，附带 LICENSE 与第三方声明。

## 合规

主程序 MIT；Qt 6 以 LGPLv3 动态链接使用，发行需随包提供可替换的 Qt DLL、许可副本与
Qt 使用声明；其余依赖许可见 `THIRD_PARTY_NOTICES.txt`。
7-Zip SDK（LGPL v2.1+，RAR 解码带 unRAR 限制）与 GPU 加速插件均为**计划中的可选后端**，
当前不链接、不捆绑；nvCOMP 因许可限制不随包分发，加速默认走 CPU 多线程 + SIMD。

## 已知限制与路线图

**P0（已完成）**：引擎抽象与双引擎实现、静态依赖链、ZIP AES 加密、路径穿越防护、
临时文件原子写入、CRC 校验、双引擎自测、CI 回退路径、第三方声明。

**P0/P1（本轮已完成）**：包内删除（含目录级联、失败原子回滚、加密归档拒绝）、
覆盖策略交互与「仅覆盖较新」真实时间比较、`scripts\package.bat` 一键打包、
外部 UTF-8 中文名 ZIP 的回归用例（防止静默丢文件）。

**P1（下一步）**：
- 引入 7-Zip SDK 后端：加密 RAR 解压、7z AES-256 / 加密文件名 / 固实 / 分卷；
- 包内重命名与新增文件、符号链接与权限保留、归档注释；
- 真实 RAR / RAR5 / 加密样本测试集，CLI 支持密码与格式参数。

**P2**：SFX 自解压、注释与压缩包修复、多线程调优、批量任务队列。

**P3**：Linux / macOS 适配、本地化、CI 全格式矩阵、SBOM 与发布签名。
