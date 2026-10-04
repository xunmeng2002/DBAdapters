# DbAdapters 进度归档

> 本文件是 [`PROGRESS.md`](PROGRESS.md) 的归档层，按项目治理规则 §8.1 维护：已关闭 / 已了结条目的**原文**存放于此，**只移动、不删改**，保留原日期与原结论。
> 条目 ID 取自 2026-09-28 首次拆分当日的文档顺序（✅ 记 `D.nn`、❓ 记 `Q.nn`、🔄 记 `R.nn`），此后永久稳定；编号按时间升序，各分类内倒序展示。
> 本文件**不参与**会话开头的通读；引用主文件未载的更早结论前，按 ID 或关键词 grep 本文件核实。

## ✅ 已完成（历史，倒序）

### D.03 · 中英文 README

- **中英文 README**：新建 `README.md`（中文）与 `README.en.md`（英文），结构参考 Spark 仓库，涵盖项目概述、核心功能模块（DBInterface / 四种 Wrapper / AsyncDBWriter）、目录结构、环境依赖、快速构建、使用示例（TypedTable CRUD、DuckDB 向量化读取、异步写库）、集成测试、许可证。

### D.02 · 环境准备文档修正

- **环境准备文档修正**：将从 Spark 拷贝的 `docs/environment-setup.md` / `docs/environment-setup.en.md` 改为 DBAdapters 专属版本，替换 GTest / test/unittest / Spark 路径等错误内容，补充 Spark + duckdb 预编译依赖说明。

### D.01 · 写库失败可见性（2026-09-12）

- **写库失败可见性（2026-09-12，`416b0ac`）**：`DB` 接口全 `void`，四种 Wrapper 拿到失败原因（`sqlite3_open`/`sqlite3_step` 返回码、stmt 有效性、`duckdb_result_error`、异常）后一律丢弃，故"没落库"与"落库成功"在日志里同形。新增 `include/DBAdapters/DBInterface/FailureLogThrottle.h`（首失败 + 每 1000 次上报一次并附累计次数；`LogOperationFailure` 统一按 wrapper/操作/表名/后端原生错误文本上报），四 Wrapper 逐语句接入，`BatchInsert` 另补 `FailedRecords:x/y`（节流会掩盖整批规模）；`AsyncDBWriter` 补连接失败上报（含积压条数与重试次数节流）、`DisConnect` 丢弃条数、新增 `ThreadExit` 未落库条数、`HandleDBOperate` 的 catch 由按值改按引用并用 `SchemaRegistry::GetSchema` 解析表名。**行为变更**：`MariadbWrapper::Connect()` 由"抛 `sql::SQLException` 穿透 `AsyncDBWriter::Run()` → `ThreadBase::ThreadFunc`（无 catch）→ `std::terminate`"改为捕获记日志后返回 false。**新增依赖**：四个 Wrapper DLL 私有链接 `Spark::Core`。**验证**：`TestDB` 新增 `TestFailureVisibility()`，`bin/Debug/TestDB.exe` 退出码 0，日志实测出现预期的 `Open database failed. ReturnCode:14`、`EXEC skipped, database is not open.`、`INSERT failed. ... Error:no such table`、`BATCH INSERT incomplete. FailedRecords:2/2`、Duckdb 同类行，四条既有用例输出与改前一致且无新增 ERROR。

## ❓ 待讨论（已关闭 / 已了结，倒序）

### Q.03 · 2026-09-27 · 项目名大小写不一致（**2026-10-04 统一为 `DbAdapters`，见主文件 ✅**）

- **项目名大小写不一致（2026-09-27）**：仓库目录、gitee 远端地址、安装目录均为 `DBAdapters`（大写 B），而 CMake `PROJECT_NAME`、C++ 命名空间、库内子目录（`include/DbAdapters`、`src/DbAdapters`）为 `DbAdapters`（`rules/cpp-style.md` §1「缩写按普通单词处理，只首字母大写」）。彻底统一需改仓库目录名 / gitee 远端地址 / `CMakePresets.json` 中的 `../Libs/DBAdapters/<triplet>`，会影响 VS 缓存目录与下游依赖树，**待决定是否统一**（本次仅统一了库内代码与文档引用，未动仓库名）。

**关闭（2026-10-04，用户指令「DBAdapters 这个命名都要改成 DbAdapters」）**：动手前核查发现本条的**前提大半已过时**——仓目录实为 `D:\Gitee\DbAdapters`、gitee 远端实为 `https://gitee.com/xunmeng2002/DbAdapters.git`、`CMakeLists.txt:10` 的 `PROJECT_NAME` 与 `CMakePresets.json` 两处 `CMAKE_INSTALL_PREFIX` 也都早已是小写 b，故「需改仓库目录名 / 远端地址」这半句不再成立，本仓无需改名、无需动 gitee。真正还剩的只是**文本层**：本仓 19 处（`LICENSE` 1、中英 README 各 6、中英 `docs/environment-setup*` 各 3）。**真正的缺陷在隔壁仓**：`Mdb` 的 `src/Mdb/*.h` 与 `test/TestMdb/TestMdb.cpp` 共 11 处 `#include <DBAdapters/...>` —— NTFS 大小写不敏感故 Windows/MSVC 一直照常编译，到 ext4（VS `copySources` 复制源码后）才 `找不到头文件`，与 2026-09-27「WSL 构建失败修复」那条**同源**；另 `Mdb/CMakePresets.json` 两处安装前缀、`QuantTrading` 4 处注释、`LearningWiki` 1 处文档提及为纯文本。**处理**：四仓一并改名（详见主文件 ✅ 2026-10-04 条）。**未动**：两仓 `PROGRESS*.md` 的正文——其中多条是在**引述**当时的名字（如"项目名 / 仓库目录 / 安装目录 `../Libs/DBAdapters/<triplet>` 有意保持 `DBAdapters` 不变"），整篇替换会让这类句子自相矛盾；仅两文件的 H1 标题随项目改定。

### Q.02 · 2026-09-27 · 构建产物全部写到源码根，多棵构建树互相覆盖（**2026-10-04 用户裁定关闭，不处理**）

- **构建产物全部写到源码根，多棵构建树互相覆盖（2026-09-27 排查 Linux 侧产物时查明，待决定是否改）**：本仓 `CMakeLists.txt` 把 `CMAKE_RUNTIME_OUTPUT_DIRECTORY`/`CMAKE_LIBRARY_OUTPUT_DIRECTORY` 指到源码根下的 `bin/$<CONFIG>` 与 `lib/$<CONFIG>`，而 `$<CONFIG>` 在单配置生成器下只由 `CMAKE_BUILD_TYPE` 决定——于是**所有 Linux 构建树都往同一个 `lib/Release` 里写**，谁最后构建谁覆盖。后果有二：①"哪个 `.so` 才是刚编出来的"从产物本身看不出来，排查时极易拿错文件；②某棵树的链接图陈旧时（实测 QuantTrading 的 `out/build/WSL-GCC-Release` 重建出的 `libMysqlWrapper.so` 只有 6,162,880 B、`DT_NEEDED` 缺 ssl/crypto、`GENERAL_NAME_free` 未定义，而正常值是 14,837,856 B），**它的坏产物会盖掉好产物**，且失效表现是运行时装载失败而非编译失败。**已规避**：那次验证改走一棵全新配置的树，产物已装到 `../Libs/DbAdapters/x64-linux`。**补记（2026-09-28）**：本条的**归因被实测推翻**——今天那棵全新配置的 `probe-fresh` 树与 `WSL-GCC-Release` 树编出的是**同一份** 6,162,880 B 的产物，故"WSL-GCC-Release 的增量链接图是坏的"不成立，真因是「缺 OpenSSL 链接」那条（已修，原文见归档 `Q.01`；该模块的 `LINK_LIBRARIES` 里没有 `libssl.a`/`libcrypto.a`）。历史日志也显示它是**时好时坏**的：2026-09-27 23:17 与 23:54 两次 Linux 运行该模块**装得上**（报 `USER option not defined`），23:12 与 2026-09-28 00:35 两次**装不上**。本条要解决的"多棵树互相覆盖"仍然成立且仍是待决项，只是它不是这桩失效的原因。`out/build/wsl_build_fresh.sh` 里那句归因注释已按实测改正。**候选处置**：(a) 按构建树分目录（如 `${CMAKE_BINARY_DIR}/bin/$<CONFIG>`）；(b) 维持现状，约定"只认一棵树并定期 `--fresh`"。**属构建布局变更，会影响消费方的 `../Libs/DbAdapters/<triplet>` 拷贝路径与 VS 预设，待用户决定。**

**关闭（2026-10-04，用户裁定：不处理）**：用户指出 Windows 与 Linux 本就不在同一目录下编译，故本条设立的前提（两个平台的产物进同一个 `lib/<config>`）不成立，无需按构建树分目录。核对下来，原记录的措辞也确实把两件事压成了一句错话——`$<CONFIG>` 在单配置生成器下只等于 `CMAKE_BUILD_TYPE`，因此 Debug → `bin/Debug`/`lib/Debug`、Release → `bin/Release`/`lib/Release`，**Debug 与 Release 各自独立，不存在"Linux 都写到 Release"**；真正被丢掉的只是"构建树"与"平台"两维。而"后果 ②"（坏产物盖好产物）已由 2026-09-28 的补记实测证伪（真因是缺 OpenSSL 链接，见 `Q.01`），"后果 ①"（分不清哪份产物是刚编的）只是排查便利性，两者都不足以支撑一次会牵动消费方预设与 VS 启动配置的构建布局变更。**留意（供日后复查）**：盘上现存的 `out/build/WSL-GCC-Release` 树其 `CMakeCache.txt` 记的是 `CMAKE_HOME_DIRECTORY=/mnt/d/Gitee/DBAdapters`，即当时是直连 Windows 源码树配置的（`lib/Release` 里同时躺着 `.lib` 与 `.so` 即是旁证）；若今后重新启用这棵树，它仍会往源码根写——届时按本条结论重新评估。

### Q.01 · 2026-09-28 · Linux 的 `libMysqlWrapper.so` 缺 OpenSSL 链接（**已修**，见主文件 2026-09-28 条）

- **Linux 的 `libMysqlWrapper.so` 缺 OpenSSL 链接，功能上装载不起来（2026-09-28 查明，待决定是否改）**：今天这份产物功能性 `dlopen` 失败，报 `undefined symbol: GENERAL_NAME_free`；它有 1 个**动态未定义**的 `GENERAL_NAME_free`、全文件**零处**定义（静态链入的符号会出现在 `nm --defined-only` 里，那里也没有），42 个未定义的 SSL/CRYPTO 符号，`DT_NEEDED` 里**没有** `libssl`/`libcrypto`。从 `build.ninja` 抽出的 `LINK_LIBRARIES` 是 `libCore.so libmysqlcppconnx-static.a libprotobuf.a liblz4.a libzstd.a`——**没带 `libssl.a`/`libcrypto.a`**，尽管 `unofficial-mysql-connector-cpp-config.cmake` 提到 OpenSSL，且 `WSL-GCC-Release/vcpkg_installed/x64-linux/lib/` 下这两份 `.a` 真实存在（另一棵 `probe-fresh` 的 `vcpkg_installed` 里**没有**）。**这不是本批引入的**（本批只动 `BackendLoader`，该模块的链接行未碰），但**本批的 `cmake --install` 把安装树 `../Libs/DbAdapters/x64-linux` 里那份一并覆盖了**，且盘上只剩两份同样的 6,162,880 B 坏产物、**无好副本可还原**。Windows 侧不受影响（`MysqlWrapper.dll` 装载正常、报 `USER option not defined`）。**候选处置**：(a) 在该模块的链接行上显式补 `OpenSSL::SSL`/`OpenSSL::Crypto`（或让 vcpkg 的 connector-cpp 把它作为 `INTERFACE` 依赖带出来）；(b) 重跑一次 vcpkg manifest 安装让 `probe-fresh` 那棵树也拿到 OpenSSL 再全量重建。**两者都是构建配置变更，须用户点头后再动。** 参见下方"构建产物全部写到源码根"条——它会掩盖这类失效。**副作用提示**：本库 `TestDB` 的 MySQL 用例在 Linux 上因此报的是装载失败而非连接失败；那条"两次尝试的原因都留在文案里"的设计正好把它点出来了（文案里是 `libMysqlWrapper.so -> …: undefined symbol: GENERAL_NAME_free`，而不是只有误导性的前一次"文件不存在"）。
