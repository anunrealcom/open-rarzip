# 压索 Yasuo Compressor（open-rarzip）项目状态记录

> 更新：2026-11-11 22:2x（换账号接续会话）
> 分工：`README.md` 讲「是什么、怎么用、路线图」；本文件讲「代码真实现状、实测证据、风险与未完成工作」，是状态核查的单一事实来源。
> 依据：本仓库 `CMakeLists.txt`、`src/`、`test_engine.cpp`、`build_deps_local.sh`、`deps/dist`、`.github/workflows/build.yml`、`installer/setup.iss`、`THIRD_PARTY_NOTICES.txt`、git 状态与本机实测输出。

| 项 | 内容 |
| --- | --- |
| 定位 | Qt 6 / C++17 开源压缩软件，对标商业压缩软件的日常体验；**只解压 RAR / RAR5，原生输出 7Z / ZIP / TAR 家族** |
| 版本 / 许可 | `project(YasuoCompressor VERSION 2.0.0)`；主程序 MIT |
| 技术栈 | Qt 6（Core / Widgets / Network / Concurrent）、CMake ≥ 3.20、Ninja、libarchive 3.9.0（静态，本地构建）、Windows CNG/bcrypt（AES 后端）、zlib |
| 代码规模 | `src/` 约 26 个文件 / 约 4900 行；另有 `test_engine.cpp` 约 370 行、安装包 / CI / 图标脚本 / 资源 |
| 仓库状态 | 只有 `33f189e Initial commit`、`a64c9e9 feat: 导入 Qt6 源码并完善 .gitignore`；**v2.0 的 `src/core`、`src/ui`、`src/shell`、`.github`、`installer`、`scripts`、`THIRD_PARTY_NOTICES.txt` 全未跟踪**，旧 `src/*.cpp` 删除未提交 |
| 构建状态 | 本机全格式路径与零依赖回退路径均 configure / build **exit 0**；自测 41 项 + 22 项全通过；CTest 通过 |

---

## 一、当前基线（v2.0 重构后，代码级核实）

- **引擎抽象**：`src/core/iengine.h` 定义 `IArchiveEngine`（`list / compress / extract / test / detect / removeEntries`）与 `Engines` 工厂；UI、CLI、右键菜单只依赖该接口，后续可插入 7-Zip SDK 后端。
- **主引擎** `libarchiveengine.cpp`：11 种可写格式（ZIP / 7Z / TAR / TAR.GZ / BZ2 / XZ / ZST / GZ / BZ2 / XZ / ZST），额外支持 RAR / RAR5 只读；ZIP WinZip AES-256/128 + ZipCrypto 写入（CNG）；64KB 流式读写；`zip:threads` / `zstd:threads` 并行编码；压缩写临时文件后原子替换，取消不留半成品。
- **回退引擎** `zlibengine.cpp`：零第三方依赖的流式 ZIP / GZ（手写 deflate + 中央目录），CI 用它验证「没有 libarchive 也能完整工作」。
- **安全** `pathguard.cpp`：拒绝 `..`、绝对路径、盘符、UNC、Windows 保留设备名与控制字符；`resolve()` 用 `absDest + "/"` 前缀 + 规范化双重校验，修掉旧 `startsWith` 前缀相似目录越界漏洞；覆盖策略 `Ask / Overwrite / Skip / Rename / OverwriteIfNewer`。
- **加速** `accelerator.cpp`：分块并行 CRC32（`crc32_combine` 按序合并）、CPU SIMD/多线程基准、`IGpuAccelerator` 可选插件契约（不链接、不分发任何专有 SDK）。
- **界面 / 集成**：单窗口压缩 / 浏览 / 任务 / 设置四页、拖拽、密码与加密方式、排除规则、主题切换、进度与取消；`archivebrowser` 归档浏览与部分解压；`shellintegration` HKCU 右键菜单 + 文件关联 + `SingleInstance` 本地套接字参数聚合；`--cli list|test|compress|extract`、`--install-shell` / `--uninstall-shell`。
- **工程资产**：`build_deps_local.sh` 离线静态编译 4 个依赖并在缺源码时打印固定 commit 获取命令；CMake 自动探测 Qt 前缀与 MinGW sysroot（不再写死版本/盘符）、`YASUO_ENABLE_LIBARCHIVE` 开关；`THIRD_PARTY_NOTICES.txt`；`.github/workflows/build.yml`（Qt 6.8 + 零依赖 CI）；`installer/setup.iss`（Inno Setup 6 用户级安装，附 LICENSE 与第三方声明）；`scripts/make_icons.py` + 原创图标资源。

---

## 二、本次接手的断点与修复

### 2.1 中断点（22:03 接手时）

上一个账号在 20:59~21:26 完成 v2.0 重构并产出 `build/v2/*.exe`，但最后一次自测停在 **32 PASS / 1 FAIL**（「加密：无密码解压被拒绝」），`test_engine.cpp` 里还留着诊断输出与临时 debug ZIP 复制；README 仍是 51 字节占位。随后 22:08~22:17 另一会话继续修复并同步了 README 与日志（见第六节）。

### 2.2 缺陷 A：标称 AES-256、实为明文 ZIP（已由并行会话修复，本机复核）

- 根因：`archive_write_set_options()` 是「整串」语义（`archive_options.c:_archive_set_options`），旧代码拼了 libarchive 根本不存在的 `zip:utf8`，遇未知选项即返回 `ARCHIVE_FAILED` 并中止后续选项，排在后面的 `zip:encryption=aes256` 从未生效；返回值又被忽略，TAR 的 `hdrcharset` 也被连坐。
- 修复：改为按格式逐项 `archive_write_set_option`，加密与 passphrase 硬性校验，绝不降级为明文；测试补「归档被识别为加密 / 无密码拒绝 / 不留明文副本 / 错误密码拒绝」。
- 复核：生成包已带 AES 标志，`加密:*` 9 项全 PASS。

### 2.3 缺陷 B：外部 UTF-8 中文条目被静默丢弃（本会话发现并修复）

- 现象：`--cli list` 对 Python/WinRAR 等外部工具生成的 UTF-8 标志 ZIP，中文条目名输出为空；`--cli extract` **返回成功码 0，却只还原出 ASCII 文件，中文文件被无声丢弃**——属于数据完整性事故。
- 根因：构建关闭了 iconv，ZIP reader 对只带 UTF-8 名称（general purpose bit11）的条目，`archive_entry_pathname()` 可能返回 NULL；旧代码把空名当普通条目，在 `extract` 里经 `PathGuard::resolve` 判为不安全后 `continue` 跳过，且不报错。
- 修复（`src/core/libarchiveengine.cpp`）：
  1. 新增 `entryPathUtf8()` / `entryLinkUtf8()`：优先 `archive_entry_pathname`（本项目自产归档正确），为空时回退 `archive_entry_pathname_utf8` / `symlink_utf8`（外部 UTF-8 归档正确）；`list` / `extract` / `test` / 符号链接统一走它。
  2. `extract` 遇到仍解不出名字的条目**整体失败并报错**，不再静默跳过，宁可中止也不丢文件。
- 验证：外部中文 ZIP 的 `list` 正确显示 `中文/说明.txt`，`extract` 完整还原 2 个文件；自产归档原有中文名用例不回退（41 项全绿）。

> 顺序说明：曾先试「UTF-8 名称优先」，导致自产归档的中文名用例 2 项失败（libarchive 对无 UTF-8 标志的条目给出的两个字段语义不同），因此最终采用「pathname 优先、为空回退 UTF-8」的互补策略。

---

## 三、验证记录（本机实测，2026-11-11）

| 验证 | 命令 / 方式 | 结果 |
| --- | --- | --- |
| 全格式 configure | `cmake -S . -B build/verify -G Ninja` | exit 0，识别「本地静态 libarchive 3.9（全格式 + RAR 解压 + ZIP AES 加密）」 |
| 全格式 build | `cmake --build build/verify -j4` | exit 0，`YasuoCompressor.exe` + `test_engine.exe` |
| 全格式自测 | `test_engine.exe` | **ALL PASS（41 passed, 0 failed）** |
| CTest | `ctest --test-dir build/verify` | 1/1 Passed |
| 零依赖 configure/build | `-DYASUO_ENABLE_LIBARCHIVE=OFF`，`build/verify-fallback` | 均 exit 0，识别「内置流式 ZIP/GZ」 |
| 零依赖自测 | 同上 `test_engine.exe` | **ALL PASS（22 passed, 0 failed）** |
| CLI | `--cli list` / `test` / `extract`（外部中文 ZIP） | 全部 exit 0；列表与解压文件完整 |
| GUI 冒烟 | `QT_QPA_PLATFORM=offscreen` 启动 8 秒 | 持续运行至超时、stdout/stderr 为空，无 Qt 报错 |

未实测（仅代码存在）：Windows 右键菜单注册/卸载、文件关联、单实例 IPC 多选聚合、`windeployqt` + ISCC 出包、GitHub Actions 真实 runner、>4GB ZIP64、RAR/RAR5 真实样本、加密 RAR。

---

## 四、未完成工作清单

> 优先级按「数据安全 > 可发布性 > 功能广度 > 治理」排，不按热度。验收标准写死，便于下一会话直接接手。

### P0 · 发布前必须清掉

| # | 事项 | 现状 | 验收 |
| --- | --- | --- | --- |
| 1 | **建立真实 git 备份** | 仅 2 个历史提交；v2.0 全部新目录未跟踪，旧文件删除未提交，误删无法恢复 | `git add -A && git commit`；`git status` 干净；最好再推一份远端 |
| 2 | **给中文名兼容加回归用例** | 代码已修并手工验证，但 `test_engine.cpp` 尚无「外部 UTF-8 标志 ZIP」用例，未来易退化 | 用 Python/`QZipWriter` 或内置字节模板造一份带 bit11 的中文名 ZIP，断言 `list` 名字非空、`extract` 两个文件都存在且内容一致 |
| 3 | **ZIP64 / 大归档实测** | `caps().zip64 = true`，自测只到 8MB | 造 >4GB 或稀疏大文件样本，验证压缩/解压/列目录不溢出 32 位 |
| 4 | **RAR / RAR5 真实样本集** | 只读能力来自 libarchive，未经任何真实 RAR 样本验证 | 用合法样本（含中文名、目录、固实）跑 `list / test / extract`，记录不支持项并给出明确错误而非静默跳过 |

### P1 · 功能缺口

| # | 事项 | 验收 |
| --- | --- | --- |
| 5 | **7-Zip SDK 后端**（LGPL v2.1+，RAR 解码带 unRAR 限制） | 加密 RAR 解压、7z AES-256 / 加密文件名 / 固实 / 分卷可用；许可原文随包，`IArchiveEngine` 上层零改动 |
| 6 | 包内删除 / 重命名 | `removeEntries` 不再返回「不支持」，删除后包结构合法可再读 |
| 7 | 覆盖策略交互与「仅覆盖较新」 | `Ask` 弹窗逐项可决策；`OverwriteIfNewer` 用真实 mtime 比较；符号链接与 Unix 权限按平台保留或明确提示 |
| 8 | Shell 集成实机验证 | `--install-shell` / `--uninstall-shell` 注册表可逆；右键多选只弹 1 个窗口；文件关联可打开 RAR/ZIP |
| 9 | 一键出包 | `windeployqt` → `build/deploy` → ISCC 产出安装包；安装后附 Qt DLL、LICENSE、THIRD_PARTY_NOTICES，卸载干净 |
| 10 | 错误路径零静默 | 审计所有 `continue` / `skip` 分支：跳过任何条目都要计数并在结果里提示（防缺陷 B 类事故复发） |

### P2 · 产品化

- CI 在真实 GitHub runner 跑通（当前 Qt 6.8 + 零依赖路径仅本机模拟）；补全格式矩阵与 CTest 用例集。
- SFX 自解压、注释读写、压缩包修复、批量任务队列、多线程调优。
- 崩溃/异常归档的模糊测试（截断、损坏中央目录、伪造加密标志、超长路径）。

### P3 · 治理与跨平台

- SBOM、发布签名、Qt LGPL 合规清单（可重链接机制、许可副本、Qt 源码获取途径声明）。
- Linux / macOS 适配（`shellintegration` 与 `app.rc` 当前 Windows 专用）、本地化、贡献指南与开源治理。

---

## 五、战略与合规结论（不变）

1. **RAR 只能解压、不能生成**：RAR 压缩算法为 RARLAB 专有，unRAR 源码许可明确禁止用于重建 RAR 压缩器；项目不含、不链接、不分发 unRAR / WinRAR 代码。原生输出走 7Z / ZIP / TAR 家族。
2. **加密 RAR 与 7z 高级特性**必须引入 7-Zip SDK 后端（LGPL v2.1+，部分 BSD，RAR 解码带 unRAR 限制），libarchive 作补充。
3. **Qt 6 按 LGPLv3 动态链接合规**：随包提供可替换 Qt DLL、许可副本与使用声明、Qt 源码获取途径；第三方声明见 `THIRD_PARTY_NOTICES.txt`。
4. **GPU 加速只留插件契约**：nvCOMP 等专有 SDK 不链接、不捆绑，默认 CPU 多线程 + SIMD。

---

## 六、协作注意：存在并行的第二会话

本次接手期间（22:08~22:17）检测到**另一会话在向同一目录持续写入**：`libarchiveengine.cpp`（加密选项修复）、`zlibengine.cpp`（本地文件头 method / CRC）、`CMakeLists.txt`（Qt 通配探测 + `YASUO_ENABLE_LIBARCHIVE`）、`test_engine.cpp`（加密与双引擎 bsdtar 用例）、`README.md`（完整重写）、`.workbuddy/memory/2026-11-11.md`（「续作 P0 收尾」日志）。本会话为避免互相覆盖：

- 源码只做了一处最小改动（缺陷 B，2.3 节），并以独立构建目录 `build/verify` / `build/verify-fallback` 验证，未改对方的 README / 测试 / CMake；
- 本文件与当日日志追加为新增内容，不覆盖既有段落。

**建议**：继续开发前先确认只保留一个会话写入该目录，否则后续改动可能互相覆盖；尤其 `git commit` 应由其中一个会话统一执行。

---

## 七、记录来源

- `.workbuddy/memory/2026-11-11.md`（项目现状梳理 + 续作 P0 收尾）
- `README.md`、`THIRD_PARTY_NOTICES.txt`、`.github/workflows/build.yml`、`installer/setup.iss`
- 本机 `build/verify`、`build/verify-fallback` 的 configure / build / test / CTest / CLI 输出
- `deps/dist/build_libarchive/config.h`（`HAVE_BCRYPT_H`）、libarchive 3.9.0 源码（`archive_options.c`、`archive_write_set_format_zip.c`、`archive_cryptor.c`）

---

## 八、后续会话续作（22:30~22:50，追加，不覆盖以上记录）

> 另一会话在 `build/v2` / `build/fallback` 上继续开发并实测。2.3 节的 UTF-8 修复代码经核对仍在本仓库，两套改动共存、未互相覆盖。

- **P1 #6 包内删除（已关闭）**：`libarchiveengine.cpp` 实现 `removeEntries` —— 临时归档重写、删除后重新 `list` 校验、`.yasuo-bak` 备份 + 原子替换（Windows 下先释放全部读写句柄）；目录级联删除；加密归档 / RAR / 单流格式明确拒绝，避免重写后加密语义静默丢失。`EngineCaps::editable`、`JobKind::Remove`、`TaskRunner`、`ArchiveBrowser`「删除所选」与完成后刷新已串通。
- **P1 #7 覆盖策略（已关闭）**：`PathGuard::decide` 增加 `entryTime`，`OverwriteIfNewer` 按归档与本地 mtime 真实比较（时间缺失保守跳过）；两个引擎分别传 `archive_entry_mtime` / DOS 时间；目标目录非空时解压前弹出策略选择（改名 / 覆盖 / 仅覆盖较新 / 跳过 / 取消）。
- **P1 #9 一键出包（部分关闭）**：新增 `scripts/package.bat`，从 CMakeCache 的 `Qt6_DIR` 定位 Qt → `windeployqt` → `ISCC`。本机实跑 Qt 运行时已部署到 `build/deploy`；本机未安装 Inno Setup，脚本正确降级并提示，安装包编译仍需在装有 ISCC 的机器上验证。
- **P0 #2 回归用例（已关闭）**：`test_engine.cpp` 手工构造带 general purpose bit11 的外部 UTF-8 中文名 ZIP，对两套引擎断言 `list` 能取回中文名、`extract` 两个文件都完整，缺陷 B 不再易退化。
- **最新实测**：全格式 `build/v2` 自测 **71 PASS / 0 FAIL**，零依赖 `build/fallback` **33 PASS / 0 FAIL**，两套 `ctest` 均通过；`README.md` 已同步能力矩阵、打包说明、路线图与自测数字。

仍未关闭：P0 #1（git 备份，v2.0 新目录依旧未跟踪、旧文件删除未提交）、#3（ZIP64 / >4GB 实测）、#4（RAR / RAR5 真实样本集）、P1 #5（7-Zip SDK 后端）、#8（Shell 集成实机验证）、#10（`continue` / `skip` 分支零静默审计）、P2 / P3。
