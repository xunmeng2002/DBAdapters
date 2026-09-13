# DBAdapters
[![License](https://img.shields.io/badge/License-BSD--4--Clause-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C++20+-orange.svg)]()
[![Build](https://img.shields.io/badge/Build-CMake3.20+-green.svg)]()

**DBAdapters** is a unified **database access layer** built on top of the **Spark** foundational library. Designed for financial trading and risk-management systems, it provides consistent read/write encapsulation for four databases: **SQLite, DuckDB, MySQL, and MariaDB**. Through a three-layer abstraction — **table schema description (`TableSchema`) → typed table (`TypedTable`) → async writer (`AsyncDBWriter`)** — business code can create tables, perform CRUD, and persist asynchronously without writing SQL by hand, fitting naturally into the classic low-latency "in-memory database + persistent database" architecture.

Created by [Fireseeker](https://fireseeker.cn/)

## 1. Project Overview

This is a personal open-source component library focused on the goal of "**describe a table schema once, read/write any database**", eliminating the repetitive work of hand-written SQL. The library is developed in standard C++20, built cross-platform with CMake, and depends on [Spark](https://gitee.com/xunmeng2002/Spark.git) for threading, logging, and object pooling. Third-party drivers (SQLite / MySQL / MariaDB) are managed via vcpkg, and an integration test program covering all four databases is included.

## 2. Core Features

The library is organized into three parts: the **unified interface layer (DBInterface)**, the **four database adapters (Wrappers)**, and the **async writer (AsyncDBWriter)**.

### 2.1 DBInterface — Unified Interface Layer (header-only template library)

It does not depend on any concrete database and only defines the description and access protocols:

| Component | Description |
| --- | --- |
| `DB` | Abstract base class: connection management + create / drop / truncate tables + single / batch CRUD + full / custom-SQL queries |
| `TableSchema` | Table schema description (table name, field descriptors, primary key, secondary indexes, record deallocation callback) that drives automatic SQL generation |
| `FieldDescriptor` | Field description: name, type (Int / Int64 / Double / Char / Bool), offset within the record struct, and array size |
| `RecordFactory` | Record allocation and collection callbacks for query results (`Allocate` / `PushBack`) |
| `IndexDefinition` | Secondary index definition (index ID + field set), used for delete-by-index |
| `SchemaRegistry` | Registry mapping table ID → `TableSchema`, used by `AsyncDBWriter` to look up schemas |
| `TypedTable<T>` | Typed table template: type-safe `Insert / Update / Delete / SelectAll / SelectWithSql / BatchInsert` built on `T::GetSchema()` |
| `MdbSubscriber` | In-memory database change subscription interface (insert / batch insert / erase / erase-by-index / update / truncate) |
| `DBSubscriber` | Database connection state subscription interface (connect / disconnect) |

### 2.2 Database Adapters (Wrappers)

All four adapters inherit the unified `DB` interface — switching databases only requires changing the constructor arguments:

| Adapter | Backend | Constructor | Notes |
| --- | --- | --- | --- |
| `SqliteWrapper` | SQLite | database file path | File-based or in-memory (`:memory:`) |
| `DuckdbWrapper` | DuckDB | database file path | Supports `:memory:`; additionally provides `SelectWithSqlVectorized` for vectorized bulk reads |
| `MysqlWrapper` | MySQL | X DevAPI connection string | e.g. `mysqlx://user:pass@localhost:33060/mdb` |
| `MariadbWrapper` | MariaDB | host / user / passwd | e.g. `tcp://localhost:3306/mdb` |

> The MySQL adapter uses the **X DevAPI** (`mysqlx://` protocol; the server must enable the X Plugin, default port 33060). The MariaDB adapter uses the classic `tcp://` protocol (default port 3306).

### 2.3 AsyncDBWriter — Async Persistence

The key component that flushes "in-memory database changes" to disk asynchronously:

- Inherits `Spark::Core::ThreadBase`; a background thread loops over the operation queue
- Implements `MdbSubscriber`, wrapping the in-memory database's `OnRecordInsert / OnRecordBatchInsert / OnRecordErase / ...` events into `DBOperate` items and enqueueing them
- Create / drop / truncate are dispatched as batch operations; inserts support transactional batches (`BatchInsert`)
- Auto-reconnects on disconnect; failed operations are logged and retried after a short sleep
- `DBOperate` objects are pooled to reduce memory allocation during high-frequency writes

### 2.4 Typical Scenario

```
In-memory trading database (low-latency) ── subscribes to changes ──► AsyncDBWriter ──► Persistent database (SQLite / DuckDB / MySQL / MariaDB)
```

The in-memory database broadcasts every change through `MdbSubscriber`; `AsyncDBWriter` asynchronously writes these events to the persistent database for post-close reconciliation, offline analysis, web queries, etc. Write latency never blocks the trading hot path.

## 3. Project Directory Structure

```
DBAdapters/
├── include/DBAdapters/           # Public headers
│   ├── DBInterface/              # Unified interface layer (DB, Schema, TypedTable, SchemaRegistry, etc.)
│   ├── AsyncDBWriter/            # Async writer component
│   ├── SqliteWrapper/            # SQLite adapter
│   ├── DuckdbWrapper/            # DuckDB adapter
│   ├── MysqlWrapper/             # MySQL adapter
│   └── MariadbWrapper/           # MariaDB adapter
├── src/DBAdapters/               # Source code
│   ├── AsyncDBWriter/            # Async writer implementation
│   ├── SqliteWrapper/            # SQLite adapter implementation
│   ├── DuckdbWrapper/            # DuckDB adapter implementation (incl. vectorized reads)
│   ├── MysqlWrapper/             # MySQL adapter implementation
│   └── MariadbWrapper/           # MariaDB adapter implementation
├── test/                         # Test programs
│   ├── TestDB/                   # All-in-one integration tests for four DBs + DuckDB vectorized-read tests
│   └── CMakeLists.txt
├── docs/                         # Documentation
│   ├── environment-setup.md      # Environment setup guide (Chinese)
│   └── environment-setup.en.md   # Environment setup guide (English)
├── submodules/                   # Submodule dependencies (CMakeCommon)
├── bin/                          # Build outputs: dynamic libraries / executables (per config)
├── lib/                          # Build outputs: import / static libraries (per config)
├── out/                          # CMake Presets build directory
├── CMakeLists.txt                # CMake main build configuration
├── CMakePresets.json             # CMake presets (VS / CLI)
├── vcpkg.json                    # vcpkg manifest (third-party drivers)
├── UpdateSubmodule.bat/sh        # Submodule update scripts
├── Install.sh                    # Linux install script (cmake --install)
├── .gitmodules                   # Git submodule configuration
├── .gitignore                    # Git ignore rules
└── LICENSE                       # BSD-4-Clause license
```

## 4. Environment Dependencies

### Prerequisites

- C++ compiler supporting **C++20 or later** (GCC, Clang, MSVC)
- Build tool: **CMake 3.20+** (this project's presets require 3.21+)
- Package manager: **vcpkg** (`VCPKG_ROOT` environment variable required; CMake uses it to locate the toolchain)
- Platform: Linux, Windows (VS2022 or WSL recommended on Windows)

### Submodule Dependencies

- **CMakeCommon**: shared CMake macros. Synchronize and pull the submodule after cloning.

### Prebuilt Dependencies (prepare manually)

The build depends on two **prebuilt third-party libraries** that must first be installed under the `Libs/` directory next to the project (matching the `CMAKE_INSTALL_PREFIX` layout in `CMakePresets.json`):

| Dependency | Install location | Provides |
| --- | --- | --- |
| **Spark** foundational library | `../Libs/Spark/<triplet>` | `Spark::Core` (threading, logging, object pool), type definitions in `Spark/Types.h` |
| **DuckDB** | `../Libs/duckdb/<triplet>` | `duckdb::duckdb` (headers + runtime library) |

`<triplet>` is `x64-windows` on Windows and `x64-linux` on Linux / WSL. See the [Spark repository](https://gitee.com/xunmeng2002/Spark.git) README for how to build and install Spark.

### vcpkg Dependencies

Dependencies declared in `vcpkg.json` (resolved automatically at build time, including transitive deps such as protobuf / lz4 / zstd / zlib):

- `sqlite3` — SQLite driver
- `mysql-connector-cpp` — MySQL X DevAPI driver
- `mariadb-connector-cpp` — MariaDB driver

> For detailed environment setup steps (proxy configuration, vcpkg installation, WSL mirrored networking, etc.), see the [Environment Setup Guide](docs/environment-setup.en.md).

## 5. Quick Build & Compilation

### 5.1 Clone Repository (with Submodules)

```bash
git clone --recursive https://gitee.com/xunmeng2002/DBAdapters.git
cd DBAdapters
```

### 5.2 Update Submodules (if not cloned recursively)

```bash
# Linux / Mac
sh UpdateSubmodule.sh

# Windows
UpdateSubmodule.bat
```

### 5.3 Prepare Dependencies

```bash
# Make sure VCPKG_ROOT is configured (system env var on Windows, ~/.bashrc on Linux)
# Make sure Spark and duckdb are installed under ../Libs/ (see "Prebuilt Dependencies" above)
```

### 5.4 Build with CMake (Presets Recommended)

```bash
# Windows (MSVC, Ninja)
cmake --preset x64-Release
cmake --build out/build/x64-Release

# Linux / WSL (GCC)
cmake --preset WSL-GCC-Release
cmake --build out/build/WSL-GCC-Release
```

After building, libraries are output to `lib/<Config>` (Release → `lib/Release`) and executables to `bin/<Config>` (e.g. `bin/Release/TestDB.exe`). On Windows, vcpkg's app-local deployment copies runtime DLLs (`sqlite3.dll`, `mysqlcppconnx-*.dll`, `mariadbcpp.dll`, `duckdb.dll`, etc.) next to the executables automatically.

### 5.5 Run Integration Tests

```bash
# Windows
./bin/Release/TestDB.exe

# Linux
./bin/Release/TestDB
```

The test program runs the same CRUD flow against SQLite / DuckDB / MySQL / MariaDB (MySQL / MariaDB require local servers and are commented out by default), and additionally runs the DuckDB vectorized-read tests.

### 5.6 Install (Optional)

```bash
# Linux (installs Debug / Release into ../Libs/DBAdapters/x64-linux)
sh Install.sh
```

## 6. Basic Usage Examples

### Example 1: Schema-Driven CRUD (TypedTable + SQLite)

```cpp
#include <DBAdapters/SqliteWrapper/SqliteWrapper.h>
#include <DBAdapters/DBInterface/TypedTable.h>
#include <Spark/Core/Core.h>
#include <cstring>
#include <vector>

using namespace dbadapters;
using namespace spark::core;

// Record struct: field memory layout must match the FieldDescriptors of the TableSchema
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
    {"AccountType", FieldType::Int32, offsetof(Account, AccountType), 0},
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

    SqliteWrapper db("./demo.sqlitedb");          // Open / create the SQLite database file
    if (!db.Connect())
    {
        WriteLog(LogLevel::Error, "Connect sqlite failed.");
        return -1;
    }

    db.CreateTable(&Account::GetSchema());        // Auto-generates CREATE TABLE from the schema

    TypedTable<Account> accounts(&db);            // Typed table: CRUD SQL is generated from the schema
    {
        Account record;
        std::memset(&record, 0, sizeof(record));
        std::strcpy(record.AccountID, "A001");
        std::strcpy(record.AccountName, "Alice");
        record.AccountType = 1;
        accounts.Insert(record);
    }
    {
        std::vector<Account*> rows;
        accounts.SelectAll(rows);                 // Auto-generates SELECT and fills the records
        for (Account* row : rows)
        {
            WriteLog(LogLevel::Info, "%s %s", row->AccountID, row->AccountName);
        }
    }

    db.DisConnect();
    return 0;
}
```

### Example 2: DuckDB Vectorized Bulk Read (SelectWithSqlVectorized)

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
    {"BarPeriod",     FieldType::Int32,  offsetof(TickRow, BarPeriod),     0},
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

    DuckdbWrapper db(":memory:");                 // In-memory database
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

> **Vectorized-read notes**: the SQL column order must match the schema field order one-to-one; NULL cells are written as type sentinels — `Double → +inf`, `Int / Int64 → 0`, `Char → empty string`, `Bool → false`. An empty return string means success; otherwise it holds the DuckDB error message.

### Example 3: Async Persistence with AsyncDBWriter

```cpp
#include <DBAdapters/AsyncDBWriter/AsyncDBWriter.h>
#include <DBAdapters/SqliteWrapper/SqliteWrapper.h>
#include <DBAdapters/DBInterface/SchemaRegistry.h>
#include <Spark/Core/Core.h>

using namespace dbadapters;
using namespace spark::core;

// 1) Custom SchemaRegistry: look up a schema by table ID (Account defined in Example 1)
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

// 2) Connection-state callback
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

    // AsyncDBWriter takes ownership of db and releases it in its destructor
    AsyncDBWriter writer(new SqliteWrapper("./async.sqlitedb"), &registry);
    writer.Subscribe(&subscriber);

    if (!writer.Connect())
    {
        WriteLog(LogLevel::Error, "AsyncDBWriter connect failed.");
        return -1;
    }
    writer.Start();                               // Start the background persistence thread

    // Simulate in-memory database changes: business code calls the MdbSubscriber interface
    Account record;
    std::memset(&record, 0, sizeof(record));
    std::strcpy(record.AccountID, "A002");
    writer.OnRecordInsert(Account::TableID, &record);   // Enqueued → persisted by the background thread

    writer.Stop();
    writer.Join();
    return 0;
}
```

> **Record-ownership contract**: for `Insert / BatchInsert`, the record is owned by the caller (the in-memory database) and must stay valid until the operation is consumed; for `Delete / DeleteByIndex / Update`, the record is released by `AsyncDBWriter` via the schema's `DeallocateRecord`, so those records should be dynamically allocated.

## 7. Integration Tests

The project ships the **test/TestDB** integration test program covering all four databases:

| Test | Description |
| --- | --- |
| `TestSqlite` | Full SQLite CRUD flow (create, truncate, insert, query, update, delete) |
| `TestDuckdb` | Full DuckDB CRUD flow |
| `TestMysql` | Full MySQL CRUD flow (requires a local MySQL X Plugin; commented out by default) |
| `TestMariadb` | Full MariaDB CRUD flow (requires a local MariaDB; commented out by default) |
| `TestDuckdbVectorized` | Vectorized reads: type conversion, NULL sentinels, error propagation |
| `TestDuckdbVectorizedMultiChunk` | Multi-chunk fallback path: BIGINT→Char, DOUBLE→Int64 row-index correctness across chunks |

### Run the Tests

```bash
# Windows
./bin/Release/TestDB.exe

# Linux
./bin/Release/TestDB
```

## 8. License & Disclaimer

- **License**: BSD-4-Clause — see the [LICENSE](LICENSE) file
- **Scope**: This project is for personal learning and research only
- **Risk**: This is a personal open-source project — thoroughly test and assess risk before use in production

## 9. Additional Notes

- **Include style**: headers use the `#include <DBAdapters/Module/HeaderName.h>` convention
- **Namespace**: all interfaces live in the `dbadapters` namespace
- **Spark dependency**: threading, logging, object pooling, and type definitions such as `DBOperateType` come from the [Spark](https://gitee.com/xunmeng2002/Spark.git) foundational library
- **Cross-database differences**: MySQL uses the MyISAM engine with the `utf8mb4_bin` collation; DuckDB's `TruncateTable` actually executes `DELETE FROM`; SQLite / DuckDB file-based and in-memory (`:memory:`) databases can both be used directly
- **Switching backends**: the same `TableSchema` and business code can switch between all four databases by simply changing the adapter constructor arguments
