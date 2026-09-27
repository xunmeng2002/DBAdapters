# DBAdapters 进度归档

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

### Q.01 · 2026-09-28 · Linux 的 `libMysqlWrapper.so` 缺 OpenSSL 链接（**已修**，见主文件 2026-09-28 条）

- **Linux 的 `libMysqlWrapper.so` 缺 OpenSSL 链接，功能上装载不起来（2026-09-28 查明，待决定是否改）**：今天这份产物功能性 `dlopen` 失败，报 `undefined symbol: GENERAL_NAME_free`；它有 1 个**动态未定义**的 `GENERAL_NAME_free`、全文件**零处**定义（静态链入的符号会出现在 `nm --defined-only` 里，那里也没有），42 个未定义的 SSL/CRYPTO 符号，`DT_NEEDED` 里**没有** `libssl`/`libcrypto`。从 `build.ninja` 抽出的 `LINK_LIBRARIES` 是 `libCore.so libmysqlcppconnx-static.a libprotobuf.a liblz4.a libzstd.a`——**没带 `libssl.a`/`libcrypto.a`**，尽管 `unofficial-mysql-connector-cpp-config.cmake` 提到 OpenSSL，且 `WSL-GCC-Release/vcpkg_installed/x64-linux/lib/` 下这两份 `.a` 真实存在（另一棵 `probe-fresh` 的 `vcpkg_installed` 里**没有**）。**这不是本批引入的**（本批只动 `BackendLoader`，该模块的链接行未碰），但**本批的 `cmake --install` 把安装树 `../Libs/DbAdapters/x64-linux` 里那份一并覆盖了**，且盘上只剩两份同样的 6,162,880 B 坏产物、**无好副本可还原**。Windows 侧不受影响（`MysqlWrapper.dll` 装载正常、报 `USER option not defined`）。**候选处置**：(a) 在该模块的链接行上显式补 `OpenSSL::SSL`/`OpenSSL::Crypto`（或让 vcpkg 的 connector-cpp 把它作为 `INTERFACE` 依赖带出来）；(b) 重跑一次 vcpkg manifest 安装让 `probe-fresh` 那棵树也拿到 OpenSSL 再全量重建。**两者都是构建配置变更，须用户点头后再动。** 参见下方"构建产物全部写到源码根"条——它会掩盖这类失效。**副作用提示**：本库 `TestDB` 的 MySQL 用例在 Linux 上因此报的是装载失败而非连接失败；那条"两次尝试的原因都留在文案里"的设计正好把它点出来了（文案里是 `libMysqlWrapper.so -> …: undefined symbol: GENERAL_NAME_free`，而不是只有误导性的前一次"文件不存在"）。
