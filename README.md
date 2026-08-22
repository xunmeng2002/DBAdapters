# DBAdapters
[![License](https://img.shields.io/badge/License-BSD--4--Clause-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C++20+-orange.svg)]()
[![Build](https://img.shields.io/badge/Build-CMake3.20+-green.svg)]()

**DBAdapters** 是一套基于 **Spark** 基础库的统一**数据库访问层**，面向金融交易 / 风控系统，提供 **SQLite、DuckDB、MySQL、MariaDB** 四种数据库的一致性读写封装。通过"**表结构描述（TableSchema）→ 类型化表（TypedTable）→ 异步写库（AsyncDBWriter）**"三层抽象，业务代码无需手写 SQL 即可完成建表、增删改查与异步落盘，天然适配"内存数据库 + 持久化数据库"的经典低时延架构。

Created by [Fireseeker](https://fireseeker.cn/)

## 一、项目概述

本项目为个人开源组件库，聚焦"**一份表结构描述，读写任意数据库**"的核心诉求，规避手写 SQL 的重复劳动。库基于标准 C++20 开发，采用 CMake 跨平台构建，依赖 [Spark](https://gitee.com/xunmeng2002/Spark.git) 提供线程、日志、对象池等基础能力，配套 vcpkg 管理第三方驱动（SQLite / MySQL / MariaDB），内置覆盖四种数据库的集成测试程序。

## 二、核心功能模块

整体分为三大部分：**统一接口层（DBInterface）**、**四种数据库适配器（Wrapper）**、**异步写库（AsyncDBWriter）**。

### 1. DBInterface —— 统一接口层（header-only 模板库）

不依赖任何具体数据库，仅提供描述与访问协议：

| 组件 | 说明 |
| --- | --- |
| `DB` | 抽象基类：连接管理 + 建表 / 删表 / 清表 + 单条 / 批量增删改 + 全量 / 自定义 SQL 查询 |
| `TableSchema` | 表结构描述（表名、字段描述、主键、二级索引、记录释放回调），驱动 SQL 自动生成 |
| `FieldDescriptor` | 字段描述：名称、类型（Int / Int64 / Double / Char / Bool）、在记录结构体中的偏移与数组长度 |
| `RecordFactory` | 查询结果的记录分配与收集回调（`Allocate` / `PushBack`） |
| `IndexDefinition` | 二级索引定义（索引 ID + 字段集合），供按索引删除使用 |
| `SchemaRegistry` | 表 ID → `TableSchema` 的注册表，`AsyncDBWriter` 据此反查 schema |
| `TypedTable<T>` | 类型化表模板：基于 `T::GetSchema()` 提供类型安全的 `Insert / Update / Delete / SelectAll / SelectWithSql / BatchInsert` |
| `MdbSubscriber` | 内存数据库变更订阅接口（插入 / 批量插入 / 删除 / 按索引删除 / 更新 / 清表） |
| `DBSubscriber` | 数据库连接状态订阅接口（连接 / 断开） |

### 2. 数据库适配器（Wrapper）

四种适配器继承统一 `DB` 接口，切换数据库只需替换构造参数：

| 适配器 | 后端 | 构造参数 | 说明 |
| --- | --- | --- | --- |
| `SqliteWrapper` | SQLite | 数据库文件路径 | 单文件 / 内存库（`:memory:`） |
| `DuckdbWrapper` | DuckDB | 数据库文件路径 | 支持 `:memory:`；额外提供 `SelectWithSqlVectorized` 向量化批量读取 |
| `MysqlWrapper` | MySQL | X DevAPI 连接串 | 如 `mysqlx://user:pass@localhost:33060/mdb` |
| `MariadbWrapper` | MariaDB | host / user / passwd | 如 `tcp://localhost:3306/mdb` |

> MySQL 适配器使用 **X DevAPI**（`mysqlx://` 协议，需服务端开启 X Plugin，默认端口 33060）；MariaDB 适配器使用经典 `tcp://` 协议（默认端口 3306）。

### 3. AsyncDBWriter —— 异步写库

把"内存库变更"异步落盘的关键组件：

- 继承 `Spark::Core::ThreadBase`，后台线程循环消费操作队列
- 实现 `MdbSubscriber`，将内存库的 `OnRecordInsert / OnRecordBatchInsert / OnRecordErase / ...` 事件封装为 `DBOperate` 投递入队
- 建表 / 删表 / 清表以批操作下发，插入支持事务批量（BatchInsert）
- 断线自动重连，异常操作记录日志并短暂休眠后重试
- `DBOperate` 走对象池复用，减少高频写库时的内存分配

### 4. 典型场景

```
内存交易数据库（低时延读写）── 订阅变更 ──► AsyncDBWriter ──► 持久化数据库（SQLite / DuckDB / MySQL / MariaDB）
```

内存库以 `MdbSubscriber` 广播每次数据变化，`AsyncDBWriter` 将这些事件异步写入持久化库，供盘后对账、离线分析、Web 查询等场景使用，写库延迟不影响交易主链路。

## 三、项目目录结构

```
DBAdapters/
├── include/DBAdapters/           # 对外暴露头文件
│   ├── DBInterface/              # 统一接口层（DB、Schema、TypedTable、SchemaRegistry 等）
│   ├── AsyncDBWriter/            # 异步写库组件
│   ├── SqliteWrapper/            # SQLite 适配器
│   ├── DuckdbWrapper/            # DuckDB 适配器
│   ├── MysqlWrapper/             # MySQL 适配器
│   └── MariadbWrapper/           # MariaDB 适配器
├── src/DBAdapters/               # 源码实现
│   ├── AsyncDBWriter/            # 异步写库实现
│   ├── SqliteWrapper/            # SQLite 适配器实现
│   ├── DuckdbWrapper/            # DuckDB 适配器实现（含向量化读取）
│   ├── MysqlWrapper/             # MySQL 适配器实现
│   └── MariadbWrapper/           # MariaDB 适配器实现
├── test/                         # 测试程序
│   ├── TestDB/                   # 四库一体化集成测试 + DuckDB 向量化读取测试
│   └── CMakeLists.txt
├── docs/                         # 文档
│   ├── environment-setup.md      # 环境准备指南（中文）
│   └── environment-setup.en.md   # 环境准备指南（英文）
├── submodules/                   # 子模块依赖（CMakeCommon）
├── bin/                          # 构建产物：动态库 / 可执行文件（按配置分目录）
├── lib/                          # 构建产物：导入库 / 静态库（按配置分目录）
├── out/                          # CMake Presets 构建目录
├── CMakeLists.txt                # CMake 主构建配置
├── CMakePresets.json             # CMake 预设配置（VS / 命令行）
├── vcpkg.json                    # vcpkg 清单（第三方驱动）
├── UpdateSubmodule.bat/sh        # 子模块更新脚本
├── Install.sh                    # Linux 安装脚本（cmake --install）
├── .gitmodules                   # Git 子模块配置
├── .gitignore                    # Git 忽略规则
└── LICENSE                       # BSD-4-Clause 开源许可证
```

## 四、环境依赖

### 基础要求

- C++ 编译器：支持 **C++20 及以上**（GCC、Clang、MSVC）
- 构建工具：**CMake 3.20+**（本项目 Presets 需 3.21+）
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
git clone --recursive https://gitee.com/xunmeng2002/DBAdapters.git
cd DBAdapters
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
# Linux（安装 Debug / Release 到 ../Libs/DBAdapters/x64-linux）
sh Install.sh
```

## 六、基础使用示例

### 示例 1：Schema 驱动的增删改查（TypedTable + SQLite）

```cpp
#include <DBAdapters/SqliteWrapper/SqliteWrapper.h>
#include <DBAdapters/DBInterface/TypedTable.h>
#include <Spark/Core/Core.h>
#include <cstring>
#include <vector>

using namespace dbadapters;
using namespace spark::core;

// 记录结构体：字段的内存布局与 TableSchema 的 FieldDescriptor 一一对应
struct Account
{
    static constexpr unsigned int TableID = 0x0001;
    char AccountID[32];
    char AccountName[64];
    int  AccountType;

    static Account* Allocate() { return new Account(); }
    static void Deallocate(void* record) { delete static_cast<Account*>(record); }

    static const FieldDescriptor Fields[3];
    static const int PrimaryKey[1];
    static const TableSchema& GetSchema();
};

const FieldDescriptor Account::Fields[3] = {
    {"AccountID",   FieldType::Char, offsetof(Account, AccountID),   sizeof(Account::AccountID)},
    {"AccountName", FieldType::Char, offsetof(Account, AccountName), sizeof(Account::AccountName)},
    {"AccountType", FieldType::Int,  offsetof(Account, AccountType), 0},
};
const int Account::PrimaryKey[1] = {0};
const TableSchema& Account::GetSchema()
{
    static const TableSchema schema = {
        "t_account", Account::Fields, 3, Account::PrimaryKey, 1, Account::Deallocate, nullptr, 0,
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
        std::strcpy(record.AccountID, "A001");
        std::strcpy(record.AccountName, u8"张三");
        record.AccountType = 1;
        accounts.Insert(record);
    }
    {
        std::vector<Account*> rows;
        accounts.SelectAll(rows);                 // 自动生成 SELECT 并填充记录
        for (Account* row : rows)
        {
            WriteLog(LogLevel::Info, "%s %s", row->AccountID, row->AccountName);
        }
    }

    db.DisConnect();
    return 0;
}
```

### 示例 2：DuckDB 向量化批量读取（SelectWithSqlVectorized）

```cpp
#include <DBAdapters/DuckdbWrapper/DuckdbWrapper.h>
#include <Spark/Core/Core.h>
#include <cstring>
#include <vector>

using namespace dbadapters;
using namespace spark::core;

struct TickRow
{
    char      TradingDay[9];
    char      InstrumentID[16];
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
    {"InstrumentID",  FieldType::Char,   offsetof(TickRow, InstrumentID),  sizeof(TickRow::InstrumentID)},
    {"LastPrice",     FieldType::Double, offsetof(TickRow, LastPrice),     0},
    {"PreClosePrice", FieldType::Double, offsetof(TickRow, PreClosePrice), 0},
    {"Volume",        FieldType::Int64,  offsetof(TickRow, Volume),        0},
    {"BarPeriod",     FieldType::Int,    offsetof(TickRow, BarPeriod),     0},
    {"IsValid",       FieldType::Bool,   offsetof(TickRow, IsValid),       0},
};
static void DeallocateTickRow(void* record) { static_cast<TickRow*>(record)->Deallocate(); }
const TableSchema& TickRow::GetSchema()
{
    static const TableSchema schema = {
        "t_test_tick", TickRowFields, 7, nullptr, 0, DeallocateTickRow, nullptr, 0,
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

    db.Exec("CREATE TABLE t_test_tick (TradingDay VARCHAR, InstrumentID VARCHAR, "
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
        "SELECT TradingDay, InstrumentID, LastPrice, PreClosePrice, Volume, BarPeriod, IsValid "
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
            WriteLog(LogLevel::Info, "%s %s last=%.2f", row->TradingDay, row->InstrumentID, row->LastPrice);
        }
    }

    Logger::GetInstance().Stop();
    Logger::GetInstance().Join();
    return 0;
}
```

> **向量化读取说明**：SQL 列序必须与 schema 字段序一一对应；NULL 写入类型哨兵——`Double → +inf`、`Int / Int64 → 0`、`Char → 空串`、`Bool → false`；返回值为空串表示成功，否则为 DuckDB 错误信息。

### 示例 3：AsyncDBWriter 异步写库

```cpp
#include <DBAdapters/AsyncDBWriter/AsyncDBWriter.h>
#include <DBAdapters/SqliteWrapper/SqliteWrapper.h>
#include <DBAdapters/DBInterface/SchemaRegistry.h>
#include <Spark/Core/Core.h>

using namespace dbadapters;
using namespace spark::core;

// 1) 自定义 SchemaRegistry：按 tableID 反查 schema（Account 定义见示例 1）
class DemoSchemaRegistry : public SchemaRegistry
{
public:
    const TableSchema* GetSchema(unsigned int tableID) const override
    {
        return tableID == Account::TableID ? &Account::GetSchema() : nullptr;
    }
    const TableSchema* const* GetAllSchemas() const override
    {
        static const TableSchema* all[] = { &Account::GetSchema() };
        return all;
    }
    int GetTableCount() const override { return 1; }
};

// 2) 连接状态回调
class DemoDBSubscriber : public DBSubscriber
{
public:
    void OnDBConnected() override { WriteLog(LogLevel::Info, "DB connected."); }
    void OnDBDisConnected() override { WriteLog(LogLevel::Info, "DB disconnected."); }
};

int main()
{
    DemoSchemaRegistry registry;
    DemoDBSubscriber subscriber;

    // AsyncDBWriter 接管 db 的所有权，析构时释放
    AsyncDBWriter writer(new SqliteWrapper("./async.sqlitedb"), &registry);
    writer.Subscribe(&subscriber);

    if (!writer.Connect())
    {
        WriteLog(LogLevel::Error, "AsyncDBWriter connect failed.");
        return -1;
    }
    writer.Start();                               // 启动后台落库线程

    // 模拟内存库变更：业务代码在数据变化时回调 MdbSubscriber 接口
    Account record;
    std::memset(&record, 0, sizeof(record));
    std::strcpy(record.AccountID, "A002");
    writer.OnRecordInsert(Account::TableID, &record);   // 入队 → 后台线程异步落库

    writer.Stop();
    writer.Join();
    return 0;
}
```

> **记录所有权约定**：`Insert / BatchInsert` 的记录由调用方（内存库）管理生命周期，且在操作被消费前需保持有效；`Delete / DeleteByIndex / Update` 的记录由 `AsyncDBWriter` 通过 schema 的 `DeallocateRecord` 释放，因此删除 / 更新用的记录应动态分配。

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

### 运行测试

```bash
# Windows
./bin/Release/TestDB.exe

# Linux
./bin/Release/TestDB
```

## 八、许可证 & 声明

- **开源协议**：BSD-4-Clause，详见 [LICENSE](LICENSE) 文件
- **适用范围**：本项目仅供个人学习、研究使用
- **风险提示**：本库为个人开源项目，生产环境使用请自行充分测试并评估风险

## 九、补充说明

- **包含路径**：头文件统一使用 `#include <DBAdapters/Module/HeaderName.h>` 风格
- **命名空间**：全部接口位于 `dbadapters` 命名空间
- **依赖 Spark**：线程、日志、对象池、`DBOperateType` 等类型定义来自 [Spark](https://gitee.com/xunmeng2002/Spark.git) 基础库
- **跨库差异**：MySQL 使用 MyISAM 引擎与 `utf8mb4_bin` 排序规则；DuckDB 的 `TruncateTable` 实际执行 `DELETE FROM`；SQLite / DuckDB 单文件库与内存库（`:memory:`）均可直接使用
- **切换后端**：同一套 `TableSchema` 与业务代码可直接在四种数据库间切换，仅需替换适配器构造参数
