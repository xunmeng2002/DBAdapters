# DbAdapters
[![License](https://img.shields.io/badge/License-BSD--4--Clause-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C++20+-orange.svg)]()
[![Build](https://img.shields.io/badge/Build-CMake3.25+-green.svg)]()

**DbAdapters** 是一套基于 **Spark** 基础库的统一**数据库访问层**，面向金融交易 / 风控系统，提供 **SQLite、DuckDB、MySQL、MariaDB** 四种数据库的一致性读写封装。通过"**表结构描述（TableSchema）→ 类型化表（TypedTable）→ 异步写库（AsyncDbWriter）**"三层抽象，业务代码无需手写 SQL 即可完成建表、增删改查与异步落盘，天然适配"内存数据库 + 持久化数据库"的经典低时延架构。

Created by [Fireseeker](https://fireseeker.cn/)

## 一、项目概述

本项目为个人开源组件库，聚焦"**一份表结构描述，读写任意数据库**"的核心诉求，规避手写 SQL 的重复劳动。库基于标准 C++20 开发，采用 CMake 跨平台构建，依赖 [Spark](https://gitee.com/xunmeng2002/Spark.git) 提供线程、日志、对象池等基础能力，配套 vcpkg 管理第三方驱动（SQLite / MySQL / MariaDB），内置覆盖四种数据库的集成测试程序。

## 二、核心功能模块

整体分为三大部分：**统一接口层（DbInterface）**、**四种数据库适配器（Wrapper）**、**异步写库（AsyncDbWriter）**。

### 1. DbInterface —— 统一接口层（header-only 模板库）

不依赖任何具体数据库，仅提供描述与访问协议：

| 组件 | 说明 |
| --- | --- |
| `Db` | 抽象基类：连接管理 + 建表 / 删表 / 清表 + 单条 / 批量增删改 + 全量 / 自定义 SQL 查询 |
| `TableSchema` | 表结构描述（表名、字段描述、主键、二级索引），驱动 SQL 自动生成 |
| `FieldDescriptor` | 字段描述：名称、类型（Int8 / UInt8 / Int16 / UInt16 / Int32 / UInt32 / Int64 / UInt64 / Double / Char / Bool 共 11 种）、在记录结构体中的偏移与数组长度 |
| `RecordFactory` | 查询结果的记录分配与收集回调（`Allocate` / `PushBack`） |
| `IndexDefinition` | 二级索引定义（索引 ID + 字段集合），供按索引删除使用 |
| `SchemaRegistry` | 表 ID → `TableSchema` 的注册表，`AsyncDbWriter` 据此反查 schema |
| `TypedTable<T>` | 类型化表模板：基于 `T::GetSchema()` 提供类型安全的 `Insert / Update / Delete / SelectAll / SelectWithSql / BatchInsert` |
| `MdbSubscriber` | 内存数据库变更订阅接口（插入 / 批量插入 / 删除 / 按索引删除 / 更新 / 清表） |
| `DbSubscriber` | 数据库连接状态订阅接口（连接 / 断开） |

### 2. 数据库适配器（Wrapper）

四种适配器继承统一 `Db` 接口，切换数据库只需替换构造参数：

| 适配器 | 后端 | 构造参数 | 说明 |
| --- | --- | --- | --- |
| `SqliteWrapper` | SQLite | 数据库文件路径 | 单文件 / 内存库（`:memory:`） |
| `DuckdbWrapper` | DuckDB | 数据库文件路径 | 支持 `:memory:`；额外提供 `SelectWithSqlVectorized` 向量化批量读取 |
| `MysqlWrapper` | MySQL | X DevAPI 连接串 | 如 `mysqlx://user:pass@localhost:33060/mdb` |
| `MariadbWrapper` | MariaDB | host / user / passwd | 如 `tcp://localhost:3306/mdb` |

> MySQL 适配器使用 **X DevAPI**（`mysqlx://` 协议，需服务端开启 X Plugin，默认端口 33060）；MariaDB 适配器使用经典 `tcp://` 协议（默认端口 3306）。

`Connect()` 是**幂等动作**——语义为「确保本连接可用并返回是否可用」：已连接时不重开，被
`DisConnect()` 关掉后**重建**。`DisConnect()` 会把内部句柄置空，两者互为前提。契约细节、四个
适配器在失败通道上的差异、以及断线重连与全量 resync 的设计见
[`docs/connection-lifecycle.md`](docs/connection-lifecycle.md)。

连接关闭后继续调用任一操作接口**不会崩**：四个适配器的每个入口都有空守卫，命中时记一条错误
日志并跳过（`MysqlWrapper: EXEC skipped, session is not open.` 之类），返回 `void` 的接口不做
额外的失败上报。

#### 运行时按配置装载（让某个后端可以不随包发运）

四个适配器若全部被链接，它们连同各自的客户端库会在**进程加载那一刻**就全部进地址空间 ——
即便当前配置只走其中一个。要让某个后端"按配置引用、可以不随包发运"，就在**运行时**按名字装载它：

```cpp
#include <DbAdapters/BackendLoader/DbBackendLoader.h>

// 按后端种类装载：种类取自 Spark 的 DbTypeType（本库不自备枚举），模块名（含 .dll/.so 与
// 调试后缀）全由装载器自己拼，调用方不参与。
DbAdapters::Db* backend = DbAdapters::LoadDatabaseBackend(
    DbTypeType::MysqlDb, "mysqlx://user:pass@localhost:33060/mdb", "", "");
// ... 使用 backend ...
delete backend;
```

装载器是**静态库**（CMake 目标 `DbAdapters::BackendLoaderStatic`），消费方需要链接它。
装载失败一律抛 `std::runtime_error`，文案含"试过哪些路径"与平台给出的原因。成功取到的 `Db*`
由调用方持有并 `delete`（`Db` 的析构是 public virtual，两平台均为动态运行库，跨模块 delete 安全）。
**不要卸载**装载进来的模块（`FreeLibrary` / `dlclose` 一概不调）。

另有一条**低层入口**按模块基名装载（`LoadDatabaseBackend("MysqlWrapper", …)`），供本库自己的
测试构造"模块根本不存在"这一情形；正常调用请用上面按种类的那条。

契约、两步查找次序、平台差异与已知边界的完整说明见
[`docs/backend-runtime-loading.md`](docs/backend-runtime-loading.md)。

**要发运某个后端**：把 `Libs/DbAdapters/<triplet>/bin` 下该模块（`MysqlWrapper` / `MariadbWrapper`，
含调试后缀）连同其客户端库一起拷到引擎的模块目录。**MariaDB 的认证插件不由本库发运、也不由本库
管理，由部署方按实际部署需求提供**：`libmariadb` 的 `caching_sha2_password` / `sha256_password` /
`client_ed25519` 等插件不是 PE 依赖，vcpkg 的 applocal 拷不了，而 `MariadbWrapper::Connect()`
会用它**编译期注入的绝对路径**覆盖 `MARIADB_PLUGIN_DIR`——该路径即插件应就位的位置。细节见
[`docs/backend-runtime-loading.md`](docs/backend-runtime-loading.md) §七。

### 3. AsyncDbWriter —— 异步写库

把"内存库变更"异步落盘的关键组件：

- 继承 `Spark::Core::ThreadBase`，后台线程循环消费操作队列
- 实现 `MdbSubscriber`，将内存库的 `OnRecordInsert / OnRecordBatchInsert / OnRecordErase / ...` 事件封装为 `DbOperate` 投递入队
- 建表 / 删表 / 清表以批操作下发，插入支持事务批量（BatchInsert）
- 断线自动重连，异常操作记录日志并短暂休眠后重试；重连成功后由订阅方全量 resync 补齐断开期间的
  内存表状态（SQLite / DuckDB 的写失败就地记日志、不抛异常，故只有 MySQL / MariaDB 会走到重连）
- `DbOperate` 走对象池复用，减少高频写库时的内存分配

### 4. 典型场景

```
内存交易数据库（低时延读写）── 订阅变更 ──► AsyncDbWriter ──► 持久化数据库（SQLite / DuckDB / MySQL / MariaDB）
```

内存库以 `MdbSubscriber` 广播每次数据变化，`AsyncDbWriter` 将这些事件异步写入持久化库，供盘后对账、离线分析、Web 查询等场景使用，写库延迟不影响交易主链路。

## 三、项目目录结构

```
DbAdapters/
├── include/DbAdapters/           # 对外暴露头文件
│   ├── DbInterface/              # 统一接口层（DB、Schema、TypedTable、SchemaRegistry 等）
│   ├── BackendLoader/            # 运行时按配置装载后端（静态库，平台差异收在 .cpp 内）
│   ├── AsyncDbWriter/            # 异步写库组件
│   ├── SqliteWrapper/            # SQLite 适配器
│   ├── DuckdbWrapper/            # DuckDB 适配器
│   ├── MysqlWrapper/             # MySQL 适配器
│   └── MariadbWrapper/           # MariaDB 适配器
├── src/DbAdapters/               # 源码实现
│   ├── BackendLoader/            # 运行时装载实现
│   ├── AsyncDbWriter/            # 异步写库实现
│   ├── SqliteWrapper/            # SQLite 适配器实现
│   ├── DuckdbWrapper/            # DuckDB 适配器实现（含向量化读取）
│   ├── MysqlWrapper/             # MySQL 适配器实现
│   └── MariadbWrapper/           # MariaDB 适配器实现
├── test/                         # 测试程序
│   ├── TestDB/                   # 四库一体化集成测试 + DuckDB 向量化读取测试
│   ├── UnitTests/                # 单元测试（doctest）：记录归属语义，无后端、无 DLL
│   ├── Common/                   # 两个测试目标共用的测试支撑（含真后端用例也要用的探针记录）
│   └── CMakeLists.txt
├── docs/                         # 文档
│   ├── backend-runtime-loading.md # 后端运行时按配置装载（设计说明）
│   ├── connection-lifecycle.md   # 连接契约与断线重连（设计说明）
│   ├── environment-setup.md      # 环境准备指南（中文）
│   ├── environment-setup.en.md   # 环境准备指南（英文）
│   └── record-ownership-refactor.md # 记录归属重构（设计说明与历次台账）
├── submodules/                   # 子模块依赖（CMakeCommon）
├── bin/                          # 构建产物：动态库 / 可执行文件（按配置分目录）
├── lib/                          # 构建产物：导入库 / 静态库（按配置分目录）
├── out/                          # CMake Presets 构建目录
├── CMakeLists.txt                # CMake 主构建配置
├── CMakePresets.json             # CMake 预设配置（VS / 命令行）
├── vcpkg.json                    # vcpkg 清单（第三方驱动）
├── UpdateSubmodule.bat/sh        # 子模块更新脚本
├── ConvertToUtf8Bom.py           # 源码文件转 UTF-8 BOM（工具脚本）
├── Install.sh                    # Linux 安装脚本（cmake --install）
├── PROGRESS.md                   # 进度记录（跨会话状态）
├── PROGRESS-archive.md           # 已关闭条目的原文归档
├── .editorconfig                 # 编辑器格式约定
├── .gitmodules                   # Git 子模块配置
├── .gitignore                    # Git 忽略规则
└── LICENSE                       # BSD-4-Clause 开源许可证
```

## 四、环境依赖

### 基础要求

- C++ 编译器：支持 **C++20 及以上**（GCC、Clang、MSVC）
- 构建工具：**CMake 3.25+**（`CMakeLists.txt` 的 `cmake_minimum_required` 即为 3.25）
- 包管理：**vcpkg**（`VCPKG_ROOT` 环境变量必需，供 CMake 定位 toolchain）
- 平台：Linux、Windows（Windows 推荐搭配 VS2022 或 WSL）

### 依赖子模块

- **CMakeCommon**：公共 CMake 宏集合，克隆后需同步拉取子模块。

### 预编译依赖（需手动准备）

项目构建依赖两个**预编译第三方库**，需先安装到项目父目录的 `Libs/` 下（与 `CMakePresets.json` 的 `CMAKE_INSTALL_PREFIX` 布局一致）：

| 依赖 | 安装位置 | 提供内容 |
| --- | --- | --- |
| **Spark** 基础库 | `../Libs/Spark/<triplet>` | `Spark::Core`（线程、日志、对象池）、`Spark/Types.h` 类型定义 |
| **DuckDB** | `../Libs/duckdb/<triplet>` | `duckdb::duckdb`（头文件 + 运行时库） |

`<triplet>` 在 Windows 下为 `x64-windows`，Linux / WSL 下为 `x64-linux`。Spark 库的构建与安装方法参见 [Spark 仓库](https://gitee.com/xunmeng2002/Spark.git) 的 README。

### vcpkg 第三方依赖

`vcpkg.json` 声明的依赖（构建时自动解析，含 protobuf / lz4 / zstd / zlib 等传递依赖）：

- `sqlite3` —— SQLite 驱动
- `mysql-connector-cpp` —— MySQL X DevAPI 驱动
- `mariadb-connector-cpp` —— MariaDB 驱动

> 详细的环境搭建步骤（代理配置、vcpkg 安装、WSL 镜像网络等）请参见 [环境准备指南](docs/environment-setup.md)。

## 五、快速构建 & 编译

### 1. 克隆代码（含子模块）

```bash
git clone --recursive https://gitee.com/xunmeng2002/DbAdapters.git
cd DbAdapters
```

### 2. 更新子模块（若未递归克隆）

```bash
# Linux / Mac
sh UpdateSubmodule.sh

# Windows
UpdateSubmodule.bat
```

### 3. 准备依赖

```bash
# 确保 VCPKG_ROOT 已配置（Windows 设为系统环境变量，Linux 写入 ~/.bashrc）
# 确保 Spark、duckdb 已安装到 ../Libs/ 对应目录（见上文"预编译依赖"）
```

### 4. CMake 编译（推荐使用 Presets）

```bash
# Windows（MSVC，Ninja）
cmake --preset x64-Release
cmake --build out/build/x64-Release

# Linux / WSL（GCC）
cmake --preset WSL-GCC-Release
cmake --build out/build/WSL-GCC-Release
```

`BUILD_TESTS` **默认 `ON`**，上面两条命令会把 `test/`（`TestDB` 与 `UnitTests`）一并编出来；配置期
因此会执行 `find_package(doctest CONFIG REQUIRED PATHS "../Libs/doctest")`，该目录必须先就位（见
[`docs/environment-setup.md`](docs/environment-setup.md) §1.7）。只编库、不想准备 doctest 的消费者
加 `-DBUILD_TESTS=OFF` 即可跳过 `test/`，此时那条 `find_package` 不会执行。

编译完成后，库文件输出至 `lib/<Config>`（Release 对应 `lib/Release`），可执行文件输出至 `bin/<Config>`（如 `bin/Release/TestDB.exe`）。Windows 下 vcpkg 的 applocal 机制会把 `sqlite3.dll`、`mysqlcppconnx-*.dll`、`mariadbcpp.dll`、`duckdb.dll` 等运行时 DLL 自动拷贝到可执行文件旁。

### 5. 运行集成测试

```bash
# Windows
./bin/Release/TestDB.exe

# Linux
./bin/Release/TestDB
```

测试程序依次执行 SQLite / DuckDB / MySQL / MariaDB 四套相同的 CRUD 流程（MySQL / MariaDB 需要本地服务，默认注释关闭），并额外运行 DuckDB 向量化读取测试。

### 6. 安装（可选）

```bash
# Linux（安装 Debug / Release 到 ../Libs/DbAdapters/x64-linux）
sh Install.sh
```

### 7. 在别的仓库消费

安装之后即可用 `find_package` 直接消费，不必把 DbAdapters 的源码拉进消费方工程：

```cmake
find_package(DbAdapters CONFIG REQUIRED PATHS "../Libs/DbAdapters/x64-windows")

target_link_libraries(YourTarget PRIVATE
    DbAdapters::SqliteWrapper      # 四个适配器按需选
    DbAdapters::AsyncDbWriter)
```

导出目标共 7 个：`DbInterface`（接口目标）、四个 Wrapper、`AsyncDbWriter`、`BackendLoaderStatic`。

**消费方必须自己准备 Spark**。`DbAdaptersConfig.cmake` 的第一条就是 `find_dependency(Spark CONFIG)`，
故配置期 `Spark` 必须能被找到（`Spark_DIR` 或 `CMAKE_PREFIX_PATH`）；此外下列公开头还直接 include Spark：
`AsyncDbWriter.h` 拉 `<Spark/Core/Core.h>`，`DbBackendLoader.h` 与 `MdbSubscriber.h` 拉
`<Spark/Types.h>`，`FailureLogThrottle.h` 拉 `<Spark/Core/Logger/Logger.h>`。而导出集里 Spark 只以
`$<LINK_ONLY:Spark::Core>` 挂在 `BackendLoaderStatic` 上——该项只影响链接行，**不传播 include 目录**。
因此用了上述任何一个头，消费方都得自己再接一次：

```cmake
find_package(Spark CONFIG REQUIRED PATHS "../Libs/Spark/x64-windows")
target_link_libraries(YourTarget PRIVATE Spark::Core)
```

只用四个 Wrapper 头与 `TypedTable.h` / `Schema.h` / `SchemaRegistry.h` 的消费方不需要这一步：
这些头只 include 各自的 Export 头与 `Db.h`，链条上不出现 Spark。

> **注意**：`DbAdaptersConfig.cmake` 还会 `find_dependency` vcpkg 提供的
> `unofficial-sqlite3`、`unofficial-mysql-connector-cpp`、`unofficial-mariadb-connector-cpp`、
> `ZLIB`、`OpenSSL`。它们不在安装树里，消费方的配置期得让 vcpkg 能看见一棵已安装树
> （`-DVCPKG_INSTALLED_DIR=<DbAdapters 的 vcpkg_installed>`，或消费方自己的 `vcpkg.json`
> manifest），否则 `find_package(DbAdapters)` 会停在 `unofficial-sqlite3` 上。

> **提示**：Windows 运行时需要三棵安装树的 `bin` 同时可见（`PATH` 或拷到可执行文件旁）：
> DbAdapters 的 `bin`（`SqliteWrapper.dll` 等）、Spark 的 `bin`（`Core.dll`）、
> vcpkg 的 `bin`（`sqlite3.dll` 等第三方 DLL）。实测 applocal 不会为这几个 IMPORTED 目标拷任何
> DLL——消费方构建产物目录里是空的——缺哪一个都以"找不到 `SqliteWrapper.dll`"的形式报出来。

## 六、基础使用示例

### 示例 1：Schema 驱动的增删改查（TypedTable + SQLite）

```cpp
#include <DbAdapters/SqliteWrapper/SqliteWrapper.h>
#include <DbAdapters/DbInterface/TypedTable.h>
#include <Spark/Core/Core.h>
#include <cstring>
#include <vector>

using namespace DbAdapters;
using namespace Spark::Core;

// 记录结构体：字段的内存布局与 TableSchema 的 FieldDescriptor 一一对应
struct Account
{
    static constexpr unsigned int TableId = 0x0001;
    char AccountId[32];
    char AccountName[64];
    int  AccountType;

    static Account* Allocate() { return new Account(); }

    static const FieldDescriptor Fields[3];
    static const int PrimaryKey[1];
    static const TableSchema& GetSchema();
};

const FieldDescriptor Account::Fields[3] = {
    {"AccountId",   FieldType::Char, offsetof(Account, AccountId),   sizeof(Account::AccountId)},
    {"AccountName", FieldType::Char, offsetof(Account, AccountName), sizeof(Account::AccountName)},
    {"AccountType", FieldType::Int32, offsetof(Account, AccountType), 0},
};
const int Account::PrimaryKey[1] = {0};
const TableSchema& Account::GetSchema()
{
    static const TableSchema schema = {
        "t_account", Account::Fields, 3, Account::PrimaryKey, 1, nullptr, 0,
    };
    return schema;
}

int main(int argc, const char* argv[])
{
    Logger::GetInstance().Init(argv[0]);
    Logger::GetInstance().SetLogLevel(LogLevel::Info, LogLevel::Info);
    Logger::GetInstance().Start();

    SqliteWrapper db("./demo.sqlitedb");          // 打开 / 创建 SQLite 数据库文件
    if (!db.Connect())
    {
        WriteLog(LogLevel::Error, "Connect sqlite failed.");
        return -1;
    }

    db.CreateTable(&Account::GetSchema());        // 按 schema 自动生成 CREATE TABLE

    TypedTable<Account> accounts(&db);            // 类型化表：CRUD 全部基于 schema 自动生成 SQL
    {
        Account record;
        std::memset(&record, 0, sizeof(record));
        std::strcpy(record.AccountId, "A001");
        std::strcpy(record.AccountName, u8"张三");
        record.AccountType = 1;
        accounts.Insert(record);
    }
    {
        std::vector<Account*> rows;
        accounts.SelectAll(rows);                 // 自动生成 SELECT 并填充记录
        for (Account* row : rows)
        {
            WriteLog(LogLevel::Info, "%s %s", row->AccountId, row->AccountName);
        }
    }

    db.DisConnect();
    return 0;
}
```

### 示例 2：DuckDB 向量化批量读取（SelectWithSqlVectorized）

```cpp
#include <DbAdapters/DuckdbWrapper/DuckdbWrapper.h>
#include <Spark/Core/Core.h>
#include <cstring>
#include <vector>

using namespace DbAdapters;
using namespace Spark::Core;

struct TickRow
{
    char      TradingDay[9];
    char      InstrumentId[16];
    double    LastPrice;
    double    PreClosePrice;
    long long Volume;
    int       BarPeriod;
    bool      IsValid;

    static TickRow* Allocate() { return new TickRow(); }
    void Deallocate() { delete this; }
    static const TableSchema& GetSchema();
};

static const FieldDescriptor TickRowFields[] = {
    {"TradingDay",    FieldType::Char,   offsetof(TickRow, TradingDay),    sizeof(TickRow::TradingDay)},
    {"InstrumentId",  FieldType::Char,   offsetof(TickRow, InstrumentId),  sizeof(TickRow::InstrumentId)},
    {"LastPrice",     FieldType::Double, offsetof(TickRow, LastPrice),     0},
    {"PreClosePrice", FieldType::Double, offsetof(TickRow, PreClosePrice), 0},
    {"Volume",        FieldType::Int64,  offsetof(TickRow, Volume),        0},
    {"BarPeriod",     FieldType::Int32,  offsetof(TickRow, BarPeriod),     0},
    {"IsValid",       FieldType::Bool,   offsetof(TickRow, IsValid),       0},
};
const TableSchema& TickRow::GetSchema()
{
    static const TableSchema schema = {
        "t_test_tick", TickRowFields, 7, nullptr, 0, nullptr, 0,
    };
    return schema;
}

int main()
{
    Logger::GetInstance().Init("DuckdbVectorizedDemo");
    Logger::GetInstance().SetLogLevel(LogLevel::Info, LogLevel::Info);
    Logger::GetInstance().Start();

    DuckdbWrapper db(":memory:");                 // 内存库
    if (!db.Connect())
    {
        WriteLog(LogLevel::Error, "Connect duckdb failed.");
        return -1;
    }

    db.Exec("CREATE TABLE t_test_tick (TradingDay VARCHAR, InstrumentId VARCHAR, "
            "LastPrice DECIMAL(24,8), PreClosePrice DOUBLE, Volume BIGINT, "
            "BarPeriod INTEGER, IsValid BOOLEAN);");
    db.Exec("INSERT INTO t_test_tick VALUES ('20260101', 'rb2610', 1234.5, 1200.0, 100, 60, true);");

    RecordFactory factory = {
        []() -> void* { return TickRow::Allocate(); },
        [](void* records, void* record) {
            static_cast<std::vector<TickRow*>*>(records)->push_back(static_cast<TickRow*>(record));
        },
    };

    std::vector<TickRow*> rows;
    std::string error = db.SelectWithSqlVectorized(
        "SELECT TradingDay, InstrumentId, LastPrice, PreClosePrice, Volume, BarPeriod, IsValid "
        "FROM t_test_tick ORDER BY TradingDay;",
        &TickRow::GetSchema(), &rows, factory);

    if (!error.empty())
    {
        WriteLog(LogLevel::Error, "vectorized select error: %s", error.c_str());
    }
    else
    {
        for (TickRow* row : rows)
        {
            WriteLog(LogLevel::Info, "%s %s last=%.2f", row->TradingDay, row->InstrumentId, row->LastPrice);
        }
    }

    Logger::GetInstance().Stop();
    Logger::GetInstance().Join();
    return 0;
}
```

> **向量化读取说明**：SQL 列序必须与 schema 字段序一一对应；NULL 写入类型哨兵——`Double → +inf`、`Int / Int64 → 0`、`Char → 空串`、`Bool → false`；返回值为空串表示成功，否则为 DuckDB 错误信息。

### 示例 3：AsyncDbWriter 异步写库

```cpp
#include <DbAdapters/AsyncDbWriter/AsyncDbWriter.h>
#include <DbAdapters/DbInterface/RecordHandle.h>
#include <DbAdapters/DbInterface/SchemaRegistry.h>
#include <DbAdapters/SqliteWrapper/SqliteWrapper.h>
#include <Spark/Core/Core.h>

using namespace DbAdapters;
using namespace Spark::Core;

// 1) 自定义 SchemaRegistry：按 tableID 反查 schema（Account 定义见示例 1）
class DemoSchemaRegistry : public SchemaRegistry
{
public:
    const TableSchema* GetSchema(unsigned int tableID) const override
    {
        return tableID == Account::TableId ? &Account::GetSchema() : nullptr;
    }
    const TableSchema* const* GetAllSchemas() const override
    {
        static const TableSchema* all[] = { &Account::GetSchema() };
        return all;
    }
    int GetTableCount() const override { return 1; }
};

// 2) 连接状态回调
class DemoDBSubscriber : public DbSubscriber
{
public:
    void OnDbConnected() override { WriteLog(LogLevel::Info, "DB connected."); }
    void OnDbDisConnected() override { WriteLog(LogLevel::Info, "DB disconnected."); }
};

int main()
{
    DemoSchemaRegistry registry;
    DemoDBSubscriber subscriber;

    // AsyncDbWriter 接管 db 的所有权，析构时释放
    AsyncDbWriter writer(new SqliteWrapper("./async.sqlitedb"), &registry);
    writer.Subscribe(&subscriber);

    if (!writer.Connect())
    {
        WriteLog(LogLevel::Error, "AsyncDbWriter connect failed.");
        return -1;
    }
    writer.Start();                               // 启动后台落库线程

    // 模拟内存库变更：业务代码在数据变化时回调 MdbSubscriber 接口
    Account record;
    std::memset(&record, 0, sizeof(record));
    std::strcpy(record.AccountId, "A002");
    writer.OnRecordInsert(Account::TableId, BorrowRecord(&record));  // 借用：record 是栈上活对象，归还仍归本函数

    writer.Stop();
    writer.Join();
    return 0;
}
```

> **记录所有权约定**：归还方式随记录句柄（`RecordHandle`）一起移交，不再按操作类型推断。生产方在移交点声明：`AdoptRecord(record)` 表示此后由写侧归还；`BorrowRecord(record)` 表示只借用指针、归还仍归自己（内存表同时持有同一条记录时用后者，否则写侧释放会让内存表里的活记录变成悬空指针）。`Insert` 与 `BatchInsert` 同属后者：表直接持有调用方交来的记录，写侧只借不还，故批与单次的归属语义完全一致，批插入也不必再为每条记录多造一份副本。**代价是借用期间调用方不得改写或释放该记录**——`Update` 会覆写它的字节、`Erase` 与 `TruncateTable` 会把它归还给对象池，而写线程此刻可能正在读它；凡是这批记录写出去之前就要改表，先 `Stop()` / `Join()` 写线程。**`BatchInsert` 不查重，这一条是调用方义务**：批量插入要求这批记录内部无重复键、且其主键与唯一键均不与表中已有记录冲突。该前提成立时逐条零额外开销；Release 下**不成立时不会有任何日志或失败**——重复的那条既不在索引里（`TruncateTable` 不会归还它，池槽位泄漏），又照样被写进数据库（内存表与库静默分叉）；**Debug 下则不静默**——`PrimaryKey` 与各唯一键的 `Insert` 返回值被就地 `assert`，违反契约会立刻中断进程（断言只覆盖这两类索引；二级索引是 `std::multiset`，重复键本就合法）。单条 `Insert` 则有预检：失败返回 `false` 并释放该记录。

## 七、集成测试

项目内置 **test/TestDB** 集成测试程序，覆盖四种数据库：

| 测试项 | 说明 |
| --- | --- |
| `TestSqlite` | SQLite 全流程 CRUD（建表、清表、插入、查询、更新、删除） |
| `TestDuckdb` | DuckDB 全流程 CRUD |
| `TestMysql` | MySQL 全流程 CRUD（需本机 MySQL X Plugin，默认注释关闭） |
| `TestMariadb` | MariaDB 全流程 CRUD（需本机 MariaDB，默认注释关闭） |
| `TestDuckdbVectorized` | 向量化读取：类型转换、NULL 哨兵、错误透出 |
| `TestDuckdbVectorizedMultiChunk` | 多 chunk 回退路径：BIGINT→Char、DOUBLE→Int64 跨 chunk 行索引正确性 |
| `TestSqliteNarrowInteger` | 窄整数读回：无符号列与 NULL 哨兵（SQLite 的 8 字节整数是二补数、无无符号列，绑定侧饱和到 `INT64_MAX`） |
| `TestSqliteNarrowSaturation` | 读侧收窄饱和：宽列塞入超范围值，期望 6 格各自饱和并汇总一条 Warning |
| `TestDuckdbNarrowInteger` | 同 SQLite 那条，另覆盖 DuckDB 专有的向量化 chunk 读取路径 |
| `TestDuckdbNarrowSaturation` | 读侧收窄饱和（DuckDB，走向量化读取路径） |
| `TestDuckdbNarrowColumnTypes` | DuckDB 窄列类型映射：`typeof` 断言各窄整数在库内的物理类型名（`TINYINT` / `UTINYINT` / `SMALLINT` / `USMALLINT` / `UINTEGER` / `UBIGINT` / `INTEGER`），不读值 |
| `TestFailureVisibility` | 失败可见性：刻意打在不存在的库/表上，断言日志出现对应 ERROR 行（该段 ERROR 属预期输出） |
| `TestBackendLoader` | 运行时装载：按种类的常路入口装载成功的后端返回可用对象；构造失败的后端返回可读原因而非让异常穿过 C 边界；另有一条按基名的低层入口覆盖"模块根本不存在"，文案须含两条候选路径。失败会反映到进程退出码 |
| `TestAsyncWriterRecordOwnership` | 记录归属的端到端烟雾：真 sqlite + 真写线程，`AdoptRecord` 移交一条记录后断言行确实落库、且归还恰好一次。语义细节（借用、移动、批、异常、断开）见下节单元测试。失败会反映到进程退出码 |
| `TestReconnect` | 断线重连语义：`Connect → DisConnect → Connect` 之后建表、写入、读回一行，断言真读到该行（SQLite 与 DuckDB 各一次）。只看 `Connect` 的返回值不足以判定——断连后 `Exec` 的失败是静默的。失败会反映到进程退出码 |

### 运行测试

```bash
# Windows
./bin/Release/TestDB.exe

# Linux
./bin/Release/TestDB
```

## 八、单元测试

`test/UnitTests` 用 [doctest](https://github.com/doctest/doctest) 承载与后端无关的语义用例，
只链接 `doctest::doctest`、`Spark::Core` 与 `AsyncDbWriter`，**不需要四个 Wrapper、不需要 duckdb.dll**
—— 与 `TestDB` 的快慢两档因此可以分开跑。

| 测试套件 | 用例 |
| :--- | :--- |
| `RecordHandle` | 移动后源不再归还；移动赋值先归还旧记录；容器扩容不重复归还；空记录不调用归还回调 |
| `RecordOwnership` | 借用不归还；移交恰好归还一次；批元素逐个恰好归还一次；执行抛异常仍恰好归还一次；断开丢弃待办时恰好归还一次；析构抽干待办队列并归还记录；断连后写线程自行重连 |

```bash
# Windows
./bin/Release/UnitTests.exe

# Linux
./bin/Release/UnitTests

# 按套件 / 用例筛选（doctest 内置）
./bin/Release/UnitTests.exe --test-suite=RecordOwnership
./bin/Release/UnitTests.exe --list-test-cases
```

> **注意**：doctest 单头不入版本库，新环境须先按
> [环境准备指南 1.7](docs/environment-setup.md#17-doctest单元测试框架) 把它放到 `../Libs/doctest`。
> `RecordHandle` 的归还回调若抛异常会直接 `std::terminate`（析构为 `noexcept`），
> doctest 无死亡测试，这条契约属已知测试盲区，由头文件注释说明。

## 九、许可证 & 声明

- **开源协议**：BSD-4-Clause，详见 [LICENSE](LICENSE) 文件
- **适用范围**：本项目仅供个人学习、研究使用
- **风险提示**：本库为个人开源项目，生产环境使用请自行充分测试并评估风险

## 十、补充说明

- **包含路径**：头文件统一使用 `#include <DbAdapters/Module/HeaderName.h>` 风格
- **命名空间**：全部接口位于 `DbAdapters` 命名空间
- **依赖 Spark**：线程、日志、对象池、`DbOperateType` 等类型定义来自 [Spark](https://gitee.com/xunmeng2002/Spark.git) 基础库
- **跨库差异**：MySQL 使用 MyISAM 引擎与 `utf8mb4_bin` 排序规则；DuckDB 的 `TruncateTable` 实际执行 `DELETE FROM`；SQLite / DuckDB 单文件库与内存库（`:memory:`）均可直接使用
- **切换后端**：同一套 `TableSchema` 与业务代码可直接在四种数据库间切换，仅需替换适配器构造参数
