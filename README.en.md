# DbAdapters
[![License](https://img.shields.io/badge/License-BSD--4--Clause-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C++20+-orange.svg)]()
[![Build](https://img.shields.io/badge/Build-CMake3.25+-green.svg)]()

**DbAdapters** is a unified **database access layer** built on top of the **Spark** foundational library. Designed for financial trading and risk-management systems, it provides consistent read/write encapsulation for four databases: **SQLite, DuckDB, MySQL, and MariaDB**. Through a three-layer abstraction — **table schema description (`TableSchema`) → typed table (`TypedTable`) → async writer (`AsyncDbWriter`)** — business code can create tables, perform CRUD, and persist asynchronously without writing SQL by hand, fitting naturally into the classic low-latency "in-memory database + persistent database" architecture.

Created by [Fireseeker](https://fireseeker.cn/)

## 1. Project Overview

This is a personal open-source component library focused on the goal of "**describe a table schema once, read/write any database**", eliminating the repetitive work of hand-written SQL. The library is developed in standard C++20, built cross-platform with CMake, and depends on [Spark](https://gitee.com/xunmeng2002/Spark.git) for threading, logging, and object pooling. Third-party drivers (SQLite / MySQL / MariaDB) are managed via vcpkg, and an integration test program covering all four databases is included.

## 2. Core Features

The library is organized into three parts: the **unified interface layer (DbInterface)**, the **four database adapters (Wrappers)**, and the **async writer (AsyncDbWriter)**.

### 2.1 DbInterface — Unified Interface Layer (header-only template library)

It does not depend on any concrete database and only defines the description and access protocols:

| Component | Description |
| --- | --- |
| `Db` | Abstract base class: connection management + create / drop / truncate tables + single / batch CRUD + full / custom-SQL queries |
| `TableSchema` | Table schema description (table name, field descriptors, primary key, secondary indexes) that drives automatic SQL generation |
| `FieldDescriptor` | Field description: name, type (Int8 / UInt8 / Int16 / UInt16 / Int32 / UInt32 / Int64 / UInt64 / Double / Char / Bool — 11 in total), offset within the record struct, and array size |
| `RecordFactory` | Record allocation and collection callbacks for query results (`Allocate` / `PushBack`) |
| `IndexDefinition` | Secondary index definition (index ID + field set), used for delete-by-index |
| `SchemaRegistry` | Registry mapping table ID → `TableSchema`, used by `AsyncDbWriter` to look up schemas |
| `TypedTable<T>` | Typed table template: type-safe `Insert / Update / Delete / SelectAll / SelectWithSql / BatchInsert` built on `T::GetSchema()` |
| `MdbSubscriber` | In-memory database change subscription interface (insert / batch insert / erase / erase-by-index / update / truncate) |
| `DbSubscriber` | Database connection state subscription interface (connect / disconnect) |

### 2.2 Database Adapters (Wrappers)

All four adapters inherit the unified `Db` interface — switching databases only requires changing the constructor arguments:

| Adapter | Backend | Constructor | Notes |
| --- | --- | --- | --- |
| `SqliteWrapper` | SQLite | database file path | File-based or in-memory (`:memory:`) |
| `DuckdbWrapper` | DuckDB | database file path | Supports `:memory:`; additionally provides `SelectWithSqlVectorized` for vectorized bulk reads |
| `MysqlWrapper` | MySQL | X DevAPI connection string | e.g. `mysqlx://user:pass@localhost:33060/mdb` |
| `MariadbWrapper` | MariaDB | host / user / passwd | e.g. `tcp://localhost:3306/mdb` |

> The MySQL adapter uses the **X DevAPI** (`mysqlx://` protocol; the server must enable the X Plugin, default port 33060). The MariaDB adapter uses the classic `tcp://` protocol (default port 3306).

`Connect()` is an **idempotent action** — it means "make this connection usable and report whether it
is": it does not reopen an established connection, and it **rebuilds** one after `DisConnect()`.
`DisConnect()` nulls the internal handle, and the two are mutually dependent. See
[`docs/connection-lifecycle.md`](docs/connection-lifecycle.md) for the contract, the differences in
how the four adapters report failures, and the reconnect plus full-resync design.

Calling any operation after the connection has been closed **does not crash**: every entry point of
all four adapters has a null guard, which logs one error line and skips the call (e.g.
`MysqlWrapper: EXEC skipped, session is not open.`). The `void` interfaces report no further failure.

#### Loading a backend at runtime (so a backend can be left out of a release)

When all four adapters are linked, they and their client libraries enter the address space the moment the **process loads** — even if the current configuration only uses one of them. To make a backend "referenced per configuration, optionally absent from a release", load it **at runtime** by name:

```cpp
#include <DbAdapters/BackendLoader/DbBackendLoader.h>

// Load by backend kind: the kind comes from Spark's DbTypeType (this library keeps no
// enum of its own); the module name — including the .dll/.so suffix and the debug
// postfix — is assembled by the loader itself, not by the caller.
DbAdapters::Db* backend = DbAdapters::LoadDatabaseBackend(
    DbTypeType::MysqlDb, "mysqlx://user:pass@localhost:33060/mdb", "", "");
// ... use backend ...
delete backend;
```

The loader is a **static library** (CMake target `DbAdapters::BackendLoaderStatic`); consumers must link it. Failure always throws `std::runtime_error`, naming the paths tried and the platform reason. The returned `Db*` is owned by the caller and released with `delete` (`Db`'s destructor is public virtual; both platforms use a dynamic runtime, so cross-module delete is safe). **Never unload** a module loaded this way (neither `FreeLibrary` nor `dlclose` is called).

A second, **lower-level entry** loads by module base name (`LoadDatabaseBackend("MysqlWrapper", …)`); it exists so this library's own tests can construct the "module does not exist" case. Normal callers use the kind-based one above.

For the entry-point contract, the two-step search order, the platform differences and the known limits, see [`docs/backend-runtime-loading.md`](docs/backend-runtime-loading.md) (Chinese).

**To ship a backend**: copy that module (`MysqlWrapper` / `MariadbWrapper`, including the debug postfix) and its client libraries from `Libs/DbAdapters/<triplet>/bin` into the engine's module directory. **MariaDB's authentication plugins are neither shipped nor managed by this library — whoever deploys provides them according to the actual deployment requirements**: `libmariadb`'s `caching_sha2_password` / `sha256_password` / `client_ed25519` plugins are not PE dependencies, so vcpkg's applocal cannot copy them, and `MariadbWrapper::Connect()` overwrites `MARIADB_PLUGIN_DIR` with the absolute path injected at compile time — that path is where the plugins are expected to sit. See [`docs/backend-runtime-loading.md`](docs/backend-runtime-loading.md) §七 for details.

### 2.3 AsyncDbWriter — Async Persistence

The key component that flushes "in-memory database changes" to disk asynchronously:

- Inherits `Spark::Core::ThreadBase`; a background thread loops over the operation queue
- Implements `MdbSubscriber`, wrapping the in-memory database's `OnRecordInsert / OnRecordBatchInsert / OnRecordErase / ...` events into `DbOperate` items and enqueueing them
- Create / drop / truncate are dispatched as batch operations; inserts support transactional batches (`BatchInsert`)
- Auto-reconnects on disconnect; failed operations are logged and retried after a short sleep. After a
  successful reconnect the subscriber performs a full resync to restore the in-memory tables for the
  disconnected period (SQLite / DuckDB log write failures in place and never throw, so only
  MySQL / MariaDB ever reach the reconnect path)
- `DbOperate` objects are pooled to reduce memory allocation during high-frequency writes

### 2.4 Typical Scenario

```
In-memory trading database (low-latency) ── subscribes to changes ──► AsyncDbWriter ──► Persistent database (SQLite / DuckDB / MySQL / MariaDB)
```

The in-memory database broadcasts every change through `MdbSubscriber`; `AsyncDbWriter` asynchronously writes these events to the persistent database for post-close reconciliation, offline analysis, web queries, etc. Write latency never blocks the trading hot path.

## 3. Project Directory Structure

```
DbAdapters/
├── include/DbAdapters/           # Public headers
│   ├── DbInterface/              # Unified interface layer (DB, Schema, TypedTable, SchemaRegistry, etc.)
│   ├── BackendLoader/            # Runtime backend loading (static library; platform code stays in the .cpp)
│   ├── AsyncDbWriter/            # Async writer component
│   ├── SqliteWrapper/            # SQLite adapter
│   ├── DuckdbWrapper/            # DuckDB adapter
│   ├── MysqlWrapper/             # MySQL adapter
│   └── MariadbWrapper/           # MariaDB adapter
├── src/DbAdapters/               # Source code
│   ├── BackendLoader/            # Runtime loading implementation
│   ├── AsyncDbWriter/            # Async writer implementation
│   ├── SqliteWrapper/            # SQLite adapter implementation
│   ├── DuckdbWrapper/            # DuckDB adapter implementation (incl. vectorized reads)
│   ├── MysqlWrapper/             # MySQL adapter implementation
│   └── MariadbWrapper/           # MariaDB adapter implementation
├── test/                         # Test programs
│   ├── TestDB/                   # All-in-one integration tests for four DBs + DuckDB vectorized-read tests
│   ├── UnitTests/                # doctest unit tests (no backend, no DLL)
│   ├── Common/                   # Test support shared by both test targets
│   └── CMakeLists.txt
├── docs/                         # Documentation
│   ├── backend-runtime-loading.md # Runtime backend loading (design notes, Chinese)
│   ├── connection-lifecycle.md   # Connection contract and reconnect (design notes, Chinese)
│   ├── environment-setup.md      # Environment setup guide (Chinese)
│   ├── environment-setup.en.md   # Environment setup guide (English)
│   └── record-ownership-refactor.md # Record ownership refactor (design notes and ledger)
├── submodules/                   # Submodule dependencies (CMakeCommon)
├── bin/                          # Build outputs: dynamic libraries / executables (per config)
├── lib/                          # Build outputs: import / static libraries (per config)
├── out/                          # CMake Presets build directory
├── CMakeLists.txt                # CMake main build configuration
├── CMakePresets.json             # CMake presets (VS / CLI)
├── vcpkg.json                    # vcpkg manifest (third-party drivers)
├── UpdateSubmodule.bat/sh        # Submodule update scripts
├── ConvertToUtf8Bom.py           # Source-to-UTF-8-BOM conversion (tool script)
├── Install.sh                    # Linux install script (cmake --install)
├── PROGRESS.md                   # Progress ledger (cross-session state)
├── PROGRESS-archive.md           # Archived full text of closed entries
├── .editorconfig                 # Editor formatting conventions
├── .gitmodules                   # Git submodule configuration
├── .gitignore                    # Git ignore rules
└── LICENSE                       # BSD-4-Clause license
```

## 4. Environment Dependencies

### Prerequisites

- C++ compiler supporting **C++20 or later** (GCC, Clang, MSVC)
- Build tool: **CMake 3.25+** (`cmake_minimum_required` in `CMakeLists.txt` is 3.25)
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
git clone --recursive https://gitee.com/xunmeng2002/DbAdapters.git
cd DbAdapters
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

`BUILD_TESTS` **defaults to `ON`**, so the two commands above also build `test/` (`TestDB` and
`UnitTests`); configure therefore runs `find_package(doctest CONFIG REQUIRED PATHS "../Libs/doctest")`,
and that directory must be in place first (see
[`docs/environment-setup.en.md`](docs/environment-setup.en.md) §1.7). A library-only consumer that does not
want to provide doctest can pass `-DBUILD_TESTS=OFF` to skip `test/`; the `find_package` above then
does not run.

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
# Linux (installs Debug / Release into ../Libs/DbAdapters/x64-linux)
sh Install.sh
```

### 5.7 Consuming from Another Repository

Once installed, the library can be consumed with `find_package` — there is no need to pull the
DbAdapters sources into the consuming project:

```cmake
find_package(DbAdapters CONFIG REQUIRED PATHS "../Libs/DbAdapters/x64-windows")

target_link_libraries(YourTarget PRIVATE
    DbAdapters::SqliteWrapper      # pick the adapters you need
    DbAdapters::AsyncDbWriter)
```

Seven targets are exported: `DbInterface` (the interface target), the four wrappers,
`AsyncDbWriter`, and `BackendLoaderStatic`.

**The consumer must provide Spark itself.** The very first line of `DbAdaptersConfig.cmake` is
`find_dependency(Spark CONFIG)`, so `Spark` has to be locatable at configure time (`Spark_DIR` or
`CMAKE_PREFIX_PATH`). On top of that, these public headers also include Spark directly:
`AsyncDbWriter.h` pulls `<Spark/Core/Core.h>`, `DbBackendLoader.h` and `MdbSubscriber.h` pull
`<Spark/Types.h>`, and `FailureLogThrottle.h` pulls `<Spark/Core/Logger/Logger.h>`. In the export set
Spark appears only as `$<LINK_ONLY:Spark::Core>` on `BackendLoaderStatic` — that governs the link
line and **does not propagate include directories**. So a consumer using any of those headers has to
wire it up once:

```cmake
find_package(Spark CONFIG REQUIRED PATHS "../Libs/Spark/x64-windows")
target_link_libraries(YourTarget PRIVATE Spark::Core)
```

A consumer that only uses the four wrapper headers plus `TypedTable.h` / `Schema.h` /
`SchemaRegistry.h` does not need that step: those headers include nothing but their own export
header and `Db.h`, and Spark never appears in the chain.

> **Note**: `DbAdaptersConfig.cmake` also runs `find_dependency` for the vcpkg-provided
> `unofficial-sqlite3`, `unofficial-mysql-connector-cpp`, `unofficial-mariadb-connector-cpp`,
> `ZLIB` and `OpenSSL`. Those are not part of the install tree, so the consumer's configure step
> must expose an installed tree to vcpkg (`-DVCPKG_INSTALLED_DIR=<DbAdapters' vcpkg_installed>`,
> or a `vcpkg.json` manifest of its own); otherwise `find_package(DbAdapters)` stops at
> `unofficial-sqlite3`.

> **Tip**: at runtime on Windows the `bin` directories of all three install trees must be visible
> (`PATH`, or copy the DLLs next to the executable): DbAdapters' `bin` (`SqliteWrapper.dll` and
> friends), Spark's `bin` (`Core.dll`), and vcpkg's `bin` (`sqlite3.dll` and other third-party
> DLLs). Measured behaviour: applocal copies nothing for these IMPORTED targets — the consumer's
> build directory ends up empty — and any missing one surfaces as "cannot open shared object file:
> `SqliteWrapper.dll`".

## 6. Basic Usage Examples

### Example 1: Schema-Driven CRUD (TypedTable + SQLite)

```cpp
#include <DbAdapters/SqliteWrapper/SqliteWrapper.h>
#include <DbAdapters/DbInterface/TypedTable.h>
#include <Spark/Core/Core.h>
#include <cstring>
#include <vector>

using namespace DbAdapters;
using namespace Spark::Core;

// Record struct: field memory layout must match the FieldDescriptors of the TableSchema
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
        std::strcpy(record.AccountId, "A001");
        std::strcpy(record.AccountName, "Alice");
        record.AccountType = 1;
        accounts.Insert(record);
    }
    {
        std::vector<Account*> rows;
        accounts.SelectAll(rows);                 // Auto-generates SELECT and fills the records
        for (Account* row : rows)
        {
            WriteLog(LogLevel::Info, "%s %s", row->AccountId, row->AccountName);
        }
    }

    db.DisConnect();
    return 0;
}
```

### Example 2: DuckDB Vectorized Bulk Read (SelectWithSqlVectorized)

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

    DuckdbWrapper db(":memory:");                 // In-memory database
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

> **Vectorized-read notes**: the SQL column order must match the schema field order one-to-one; NULL cells are written as type sentinels — `Double → +inf`, `Int / Int64 → 0`, `Char → empty string`, `Bool → false`. An empty return string means success; otherwise it holds the DuckDB error message.

### Example 3: Async Persistence with AsyncDbWriter

```cpp
#include <DbAdapters/AsyncDbWriter/AsyncDbWriter.h>
#include <DbAdapters/DbInterface/RecordHandle.h>
#include <DbAdapters/DbInterface/SchemaRegistry.h>
#include <DbAdapters/SqliteWrapper/SqliteWrapper.h>
#include <Spark/Core/Core.h>

using namespace DbAdapters;
using namespace Spark::Core;

// 1) Custom SchemaRegistry: look up a schema by table ID (Account defined in Example 1)
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

// 2) Connection-state callback
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

    // AsyncDbWriter takes ownership of db and releases it in its destructor
    AsyncDbWriter writer(new SqliteWrapper("./async.sqlitedb"), &registry);
    writer.Subscribe(&subscriber);

    if (!writer.Connect())
    {
        WriteLog(LogLevel::Error, "AsyncDbWriter connect failed.");
        return -1;
    }
    writer.Start();                               // Start the background persistence thread

    // Simulate in-memory database changes: business code calls the MdbSubscriber interface
    Account record;
    std::memset(&record, 0, sizeof(record));
    std::strcpy(record.AccountId, "A002");
    writer.OnRecordInsert(Account::TableId, BorrowRecord(&record));  // Borrowed: `record` is a live stack object, still released here

    writer.Stop();
    writer.Join();
    return 0;
}
```

> **Record-ownership contract**: how a record is released travels with its record handle (`RecordHandle`) instead of being inferred from the operation type. The producer declares it at the handoff point: `AdoptRecord(record)` means the writer releases it from now on; `BorrowRecord(record)` means only the pointer is borrowed and the producer still releases it. `Insert` and `BatchInsert` both use the latter: the table holds the records handed in by the caller and the writer only borrows them, so batches and single inserts share exactly the same ownership semantics — and a batch no longer needs a private copy of every record. The price is that a borrowed record must not be modified or released while the writer may still be reading it: `Update` rewrites its bytes, and `Erase` / `TruncateTable` returns it to the object pool. Stop and join the writer before changing the table ahead of a pending write. **`BatchInsert` does not check for key conflicts — that is the caller's obligation**: a batch must not contain duplicates internally, nor collide with records already in the table. When that holds, every record costs nothing extra; when it does not, **Release builds log nothing and fail nothing** — the losing record is absent from the index (so `TruncateTable` never releases it, leaking a pool slot) yet is still written to the database (memory table and database silently diverge). **Debug builds are not silent**: the `bool` returned by `PrimaryKey->Insert` and each unique key's `Insert` is passed straight to `assert`, so a broken contract aborts the process immediately (only those two index kinds are asserted; secondary indexes are `std::multiset`, where duplicate keys are legitimate). A single `Insert` does pre-check: it returns `false` and releases the record on conflict.

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
| `TestSqliteNarrowInteger` | Reading back narrow integer columns: unsigned columns and the NULL sentinel (SQLite's 8-byte integer is two's complement with no unsigned column, so the bind side saturates to `INT64_MAX`) |
| `TestSqliteNarrowSaturation` | Read-side narrowing saturation: out-of-range values in wide columns must saturate cell by cell and roll up into a single Warning |
| `TestDuckdbNarrowInteger` | Same as the SQLite case, plus DuckDB's vectorized chunk read path |
| `TestDuckdbNarrowSaturation` | Read-side narrowing saturation (DuckDB, through the vectorized path) |
| `TestDuckdbNarrowColumnTypes` | DuckDB narrow column-type mapping: `typeof` asserts the physical type name each narrow integer gets in the database (`TINYINT` / `UTINYINT` / `SMALLINT` / `USMALLINT` / `UINTEGER` / `UBIGINT` / `INTEGER`); no values are read |
| `TestFailureVisibility` | Failure visibility: operations are deliberately aimed at a missing database / table and the log is asserted to carry the matching ERROR lines (those ERRORs are expected output) |
| `TestBackendLoader` | Runtime loading: the normal per-kind entry returns a usable object for a backend that loads; a backend that fails to construct returns a readable reason instead of letting the exception cross the C boundary; a low-level per-basename entry covers "module does not exist at all", and the message must name both candidate paths. Failures are reflected in the process exit code |
| `TestAsyncWriterRecordOwnership` | End-to-end smoke for record ownership: against a real sqlite backend with a real writer thread, a record handed over via `AdoptRecord` must actually land in the table and be released exactly once. The semantic details (borrow, move, batch, exceptions, disconnect) live in the unit tests below. Failures are reflected in the process exit code |
| `TestReconnect` | Reconnect semantics: after `Connect → DisConnect → Connect` the test creates a table, writes a row and reads it back, asserting the row is really there (once for SQLite, once for DuckDB). The return value of `Connect` alone is not enough — after a disconnect, `Exec` failures are silent. Failures are reflected in the process exit code |

### Run the Tests

```bash
# Windows
./bin/Release/TestDB.exe

# Linux
./bin/Release/TestDB
```

## 8. Unit Tests

`test/UnitTests` uses [doctest](https://github.com/doctest/doctest) for backend-independent
semantic cases. It links only `doctest::doctest`, `Spark::Core`, and `AsyncDbWriter` — it
**needs none of the four wrappers and no duckdb.dll**, so the fast and slow tiers of `TestDB`
can be run separately.

| Suite | Cases |
| :--- | :--- |
| `RecordHandle` | Move leaves the source empty; move assignment releases the old record first; vector growth does not double-release; a null record does not invoke the release callback |
| `RecordOwnership` | A borrowed record is not released; an adopted one is released exactly once; every batch element is released; a throwing execution still releases exactly once; discarding pending operations on disconnect releases exactly once; the destructor drains the pending queue and releases its records; the writer thread reconnects by itself after a disconnect |

```bash
# Windows
./bin/Release/UnitTests.exe

# Linux
./bin/Release/UnitTests

# Filter by suite / case (built into doctest)
./bin/Release/UnitTests.exe --test-suite=RecordOwnership
./bin/Release/UnitTests.exe --list-test-cases
```

> **Note**: the doctest single header is not committed, so a new environment must first place it
> under `../Libs/doctest` as described in
> [Environment Setup 1.7](docs/environment-setup.en.md#17-doctest-unit-test-framework).
> A throwing release callback in `RecordHandle` reaches `std::terminate` directly (the destructor
> is `noexcept`); doctest has no death tests, so that contract is a known test blind spot covered
> by the header's comments.

## 9. License & Disclaimer

- **License**: BSD-4-Clause — see the [LICENSE](LICENSE) file
- **Scope**: This project is for personal learning and research only
- **Risk**: This is a personal open-source project — thoroughly test and assess risk before use in production

## 10. Additional Notes

- **Include style**: headers use the `#include <DbAdapters/Module/HeaderName.h>` convention
- **Namespace**: all interfaces live in the `DbAdapters` namespace
- **Spark dependency**: threading, logging, object pooling, and type definitions such as `DbOperateType` come from the [Spark](https://gitee.com/xunmeng2002/Spark.git) foundational library
- **Cross-database differences**: MySQL uses the MyISAM engine with the `utf8mb4_bin` collation; DuckDB's `TruncateTable` actually executes `DELETE FROM`; SQLite / DuckDB file-based and in-memory (`:memory:`) databases can both be used directly
- **Switching backends**: the same `TableSchema` and business code can switch between all four databases by simply changing the adapter constructor arguments
