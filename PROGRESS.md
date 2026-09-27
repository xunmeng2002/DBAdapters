# DBAdapters 进度记录

> 按项目治理规则维护：每次会话结束更新本文件，下次会话开始先读取。

## ✅ 已完成

- **WSL 构建失败修复 + 命名大小写统一（2026-09-27）**：VS `WSL-GCC-Debug` 预设 configure 失败，报 5 条 `No SOURCES given to target`（AsyncDbWriter / MysqlWrapper / MariadbWrapper / DuckdbWrapper / SqliteWrapper）。根因是目录改名（`3a0e39a rename`、`8c01bae 文件夹改名`）后引用未同步：`CMakeLists.txt` 写 `src/DBAdapters/...`、13 个源文件写 `#include <DBAdapters/DBInterface/...>`，而真实目录是 `src/DbAdapters`、`include/DbAdapters/DbInterface`。NTFS 大小写不敏感，故 Windows/MSVC 一直照常编译；VS 的 WSL 预设开了 `copySources`，源码被复制到 ext4（大小写敏感）后 `file(GLOB_RECURSE)` 匹配 0 个文件、`#include` 找不到头文件，问题才暴露。**改动**：修正上述路径大小写（15 文件 26 行，纯字符串替换，无逻辑与接口变更）；一并将 README（中/英）目录树、include 示例与模块名统一为代码真实名称（`DBInterface`→`DbInterface`，`AsyncDBWriter`→`AsyncDbWriter`——后者在示例代码中本就是错标识符，照抄无法编译）、`CMakeLists.txt:118` 与 `MariadbWrapper.cpp:293` 注释、`DbAdaptersConfig.cmake` 的写法。项目名 / 仓库目录 / 安装目录 `../Libs/DBAdapters/<triplet>` 有意保持 `DBAdapters` 不变（见待讨论项）。**验证**：WSL 内实测旧路径 glob 到 0 个 .cpp，修正后为 2/1/1/1/1 个；用 `git ls-files` 逐条校验项目源码与 README 中所有 `<DbAdapters/...>` include 均按大小写精确命中（CMake 生成型 `*Export.h` 除外）。**未验证**：尚未在 VS/WSL 重新跑完整 configure 与编译（需 VS 重新同步源码）。本文件历史条目中的旧写法按归档规则原文保留，未改动。

- **中英文 README**：新建 `README.md`（中文）与 `README.en.md`（英文），结构参考 Spark 仓库，涵盖项目概述、核心功能模块（DBInterface / 四种 Wrapper / AsyncDBWriter）、目录结构、环境依赖、快速构建、使用示例（TypedTable CRUD、DuckDB 向量化读取、异步写库）、集成测试、许可证。
- **环境准备文档修正**：将从 Spark 拷贝的 `docs/environment-setup.md` / `docs/environment-setup.en.md` 改为 DBAdapters 专属版本，替换 GTest / test/unittest / Spark 路径等错误内容，补充 Spark + duckdb 预编译依赖说明。

- **写库失败可见性（2026-09-12，`416b0ac`）**：`DB` 接口全 `void`，四种 Wrapper 拿到失败原因（`sqlite3_open`/`sqlite3_step` 返回码、stmt 有效性、`duckdb_result_error`、异常）后一律丢弃，故"没落库"与"落库成功"在日志里同形。新增 `include/DBAdapters/DBInterface/FailureLogThrottle.h`（首失败 + 每 1000 次上报一次并附累计次数；`LogOperationFailure` 统一按 wrapper/操作/表名/后端原生错误文本上报），四 Wrapper 逐语句接入，`BatchInsert` 另补 `FailedRecords:x/y`（节流会掩盖整批规模）；`AsyncDBWriter` 补连接失败上报（含积压条数与重试次数节流）、`DisConnect` 丢弃条数、新增 `ThreadExit` 未落库条数、`HandleDBOperate` 的 catch 由按值改按引用并用 `SchemaRegistry::GetSchema` 解析表名。**行为变更**：`MariadbWrapper::Connect()` 由"抛 `sql::SQLException` 穿透 `AsyncDBWriter::Run()` → `ThreadBase::ThreadFunc`（无 catch）→ `std::terminate`"改为捕获记日志后返回 false。**新增依赖**：四个 Wrapper DLL 私有链接 `Spark::Core`。**验证**：`TestDB` 新增 `TestFailureVisibility()`，`bin/Debug/TestDB.exe` 退出码 0，日志实测出现预期的 `Open database failed. ReturnCode:14`、`EXEC skipped, database is not open.`、`INSERT failed. ... Error:no such table`、`BATCH INSERT incomplete. FailedRecords:2/2`、Duckdb 同类行，四条既有用例输出与改前一致且无新增 ERROR。

## 🔄 进行中

- 无

## ❓ 待讨论 / 待决策

- **项目名大小写不一致（2026-09-27）**：仓库目录、gitee 远端地址、安装目录均为 `DBAdapters`（大写 B），而 CMake `PROJECT_NAME`、C++ 命名空间、库内子目录（`include/DbAdapters`、`src/DbAdapters`）为 `DbAdapters`（`rules/cpp-style.md` §1「缩写按普通单词处理，只首字母大写」）。彻底统一需改仓库目录名 / gitee 远端地址 / `CMakePresets.json` 中的 `../Libs/DBAdapters/<triplet>`，会影响 VS 缓存目录与下游依赖树，**待决定是否统一**（本次仅统一了库内代码与文档引用，未动仓库名）。
- **DBAdapters 遗留项（2026-09-12 复查，均未改）**：① 未连接期间 `AsyncDBWriter` 队列只积压不丢弃，长时间断库会无界增长（内存）；② `HandleDBOperate` 的 catch 路径中 `m_DB->DisConnect()` 与 `DeallocateRecord`/`Deallocate` 若再抛异常会逃出 `Run()` 直达 `std::terminate`（catch 内未再包一层）；③ `DbOperate::DeallocateRecord()` 对 Insert/BatchInsert/Truncate 早退（记录泄漏，既有）；④ `MysqlWrapper::Connect()` 仍恒真（`impl_ != nullptr`，close 后照旧为真），写库线程不会重连；⑤ 尚未 `cmake --install` 到 `../Libs/DBAdapters/x64-windows`（会改动 QuantTrading 的依赖树与运行时 DLL），待决定；⑥ `DBAdaptersConfig.cmake` 未 `find_dependency(Spark)`，安装后消费方需自行保证 `Spark::Core` 可用。
- README 落款沿用 Spark 的 "Created by [Fireseeker]"（两仓库 LICENSE 同为 xunmeng2002）——如需调整署名请告知。
- 构建依赖 Spark 与 duckdb 为**预编译库**（`../Libs/Spark/<triplet>`、`../Libs/duckdb/<triplet>`），`vcpkg.json` 未声明这两者；后续若考虑可复现构建，可讨论是否将 duckdb 纳入 vcpkg 管理。
- `test/TestDB` 中 MySQL / MariaDB 测试默认注释关闭（需本地服务），如要常开可配置 CI。
