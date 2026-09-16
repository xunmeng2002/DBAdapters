#include "MdbStructs.h"
#include <DBAdapters/SqliteWrapper/SqliteWrapper.h>
#include <DBAdapters/DuckdbWrapper/DuckdbWrapper.h>
#include <DBAdapters/MysqlWrapper/MysqlWrapper.h>
#include <DBAdapters/MariadbWrapper/MariadbWrapper.h>
#include <DBAdapters/DBInterface/TypedTable.h>
#include <DBAdapters/DBInterface/SchemaRegistry.h>
#include <DBAdapters/AsyncDBWriter/AsyncDBWriter.h>
#include <Spark/Core/Core.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>


using namespace std;
using namespace Mdb;
using namespace Spark::Core;
using namespace DbAdapters;

const char* sqliteDBName = "./Test.sqlitedb";
const char* duckdbDBName = "./Test.duckdb";
const char* mysqlHost = "mysqlx://sams:sams@localhost:33060/mdb";   
const char* mariadbHost = "tcp://localhost:3306/mdb";
const char* mariadbUser = "sams";
const char* mariadbPassword = "sams";

TradingDay* PrepareTradingDay()
{
	auto currDate = TimeUtility::GetLocalDate();
	TradingDay* tradingDay = new TradingDay();
	memset(tradingDay, 0, sizeof(TradingDay));
	tradingDay->PK = 1;
	TimeUtility::GetPreTradingDay(currDate.c_str(), tradingDay->PreTradingDay);
	TimeUtility::GetNextTradingDay(tradingDay->PreTradingDay, tradingDay->CurrTradingDay);
	return tradingDay;
}
list<Exchange*>* PrepareExchanges()
{
	list<Exchange*>* exchanges = new list<Exchange*>();
	Exchange* exchange1 = new Exchange();
	Exchange* exchange2 = new Exchange();
	Exchange* exchange3 = new Exchange();
	Exchange* exchange4 = new Exchange();
	Exchange* exchange5 = new Exchange();
	Exchange* exchange6 = new Exchange();
	strcpy(exchange1->ExchangeId, "SHFE");
	strcpy(exchange1->ExchangeName, (const char*)(u8"上海期货交易所"));
	strcpy(exchange2->ExchangeId, "INE");
	strcpy(exchange2->ExchangeName, (const char*)(u8"上海国际能源交易中心"));
	strcpy(exchange3->ExchangeId, "CFFEX");
	strcpy(exchange3->ExchangeName, (const char*)(u8"中国金融期货交易所"));
	strcpy(exchange4->ExchangeId, "CZCE");
	strcpy(exchange4->ExchangeName, (const char*)(u8"郑州商品期货交易所"));
	strcpy(exchange5->ExchangeId, "DCE");
	strcpy(exchange5->ExchangeName, (const char*)(u8"大连商品期货交易所"));
	strcpy(exchange6->ExchangeId, "GFE");
	strcpy(exchange6->ExchangeName, (const char*)(u8"广期所"));

	exchanges->push_back(exchange1);
	exchanges->push_back(exchange2);
	exchanges->push_back(exchange3);
	exchanges->push_back(exchange4);
	exchanges->push_back(exchange5);
	exchanges->push_back(exchange6);
	return exchanges;
}
Account* PrepareAccount(const char* accountId, const char* accountName, const char* password)
{
	Account* account = new Account();
	memset(account, 0, sizeof(Account));
	strcpy(account->AccountId, accountId);
	strcpy(account->AccountName, accountName);
	account->AccountType = AccountTypeType::Primary;
	account->AccountStatus = AccountStatusType::Normal;
	strcpy(account->Password, "123456");
	account->TradeGroupId = 10000;
	account->RiskGroupId = 10000;
	account->CommissionGroupId = 10000;
	return account;
}


void Print(TradingDay* tradingDay)
{
	WriteLog(LogLevel::Info, "%s", tradingDay->GetDebugString());
}
void Print(Exchange* exchange)
{
	WriteLog(LogLevel::Info, "%s", exchange->GetDebugString());
}
void Print(Account* account)
{
	WriteLog(LogLevel::Info, "%s", account->GetDebugString());
}

static void InitTradingDay(TypedTable<TradingDay>& table)
{
	auto tradingDay = PrepareTradingDay();
	table.Insert(*tradingDay);
}
static void InitExchange(TypedTable<Exchange>& table)
{
	auto exchanges = PrepareExchanges();
	for (auto exchange : *exchanges)
	{
		table.Insert(*exchange);
	}
	exchanges->clear();
	delete exchanges;
}
static void InitAccount(TypedTable<Account>& table)
{
	Account* account = PrepareAccount("Xunmeng01", "Xunmeng01", "123456");
	table.Insert(*account);
}

static void TestDB(DB* db)
{
	if (!db->Connect())
	{
		WriteLog(LogLevel::Warning, "Connect Failed.");
		return;
	}

	static const TableSchema* allSchemas[] = {
		&TradingDay::GetSchema(), &Exchange::GetSchema(), &Product::GetSchema(),
		&Instrument::GetSchema(), &PrimaryAccount::GetSchema(), &Account::GetSchema(),
		&Capital::GetSchema(), &Position::GetSchema(), &PositionDetail::GetSchema(),
		&Order::GetSchema(), &Trade::GetSchema(),
	};
	db->CreateTables(allSchemas, 11);
	db->TruncateTables(allSchemas, 11);

	TypedTable<TradingDay>    t_tradingDay(db);
	TypedTable<Exchange>      t_exchange(db);
	TypedTable<Account>       t_account(db);

	InitTradingDay(t_tradingDay);
	InitExchange(t_exchange);
	InitAccount(t_account);

	{
		std::vector<Account*> accounts;
		t_account.SelectAll(accounts);
		for (auto account : accounts)
		{
			Print(account);
		}
		if (!accounts.empty())
		{
			auto account = accounts.front();
			Account newAccount;
			memcpy(&newAccount, account, sizeof(Account));
			strcpy(newAccount.AccountName, "Jack01");
			t_account.Update(newAccount);
		}
	}

	{
		std::vector<Exchange*> exchanges;
		t_exchange.SelectAll(exchanges);
		for (auto exchange : exchanges)
		{
			Print(exchange);
		}
		if (!exchanges.empty())
		{
			auto exchange = exchanges.front();
			t_exchange.Delete(*exchange);
		}
	}

    //db->DropAllTables(allSchemas, 11);
	db->DisConnect();
}

static void TestSqlite()
{
	SqliteWrapper* sqlitedb = new SqliteWrapper(sqliteDBName);
	WriteLog(LogLevel::Info, "TestDB with Sqlite");
	TestDB(sqlitedb);
}
static void TestDuckdb()
{
    DuckdbWrapper* duckdb = new DuckdbWrapper(duckdbDBName);
    WriteLog(LogLevel::Info, "TestDB with Duckdb");
    TestDB(duckdb);
}
static void TestMysql()
{
    MysqlWrapper* mysql = new MysqlWrapper(mysqlHost);
    WriteLog(LogLevel::Info, "TestDB with Mysql");
    TestDB(mysql);
}
static void TestMariadb()
{
    MariadbWrapper* mariadb = new MariadbWrapper(mariadbHost, mariadbUser, mariadbPassword);
    WriteLog(LogLevel::Info, "TestDB with Mariadb");
    TestDB(mariadb);
}

// ===================== SelectWithSqlVectorized 测试 =====================

namespace Mdb
{
    class TestTickRow
    {
    public:
        char TradingDay[9];
        char InstrumentId[16];
        double LastPrice;
        double PreClosePrice;
        long long Volume;
        int BarPeriod;
        bool IsValid;

        static TestTickRow* Allocate() { return new TestTickRow(); }
        void Deallocate() { delete this; }
        static const TableSchema& GetSchema();
    };

    static const FieldDescriptor TestTickRowFields[] = {
        {"TradingDay",    FieldType::Char,   offsetof(TestTickRow, TradingDay),    sizeof(TestTickRow::TradingDay)},
        {"InstrumentId",  FieldType::Char,   offsetof(TestTickRow, InstrumentId),  sizeof(TestTickRow::InstrumentId)},
        {"LastPrice",     FieldType::Double, offsetof(TestTickRow, LastPrice),     0},
        {"PreClosePrice", FieldType::Double, offsetof(TestTickRow, PreClosePrice), 0},
        {"Volume",        FieldType::Int64,  offsetof(TestTickRow, Volume),        0},
        {"BarPeriod",     FieldType::Int32,  offsetof(TestTickRow, BarPeriod),     0},
        {"IsValid",       FieldType::Bool,   offsetof(TestTickRow, IsValid),       0},
    };
    static void DeallocateTestTickRow(void* record)
    {
        static_cast<TestTickRow*>(record)->Deallocate();
    }
    const TableSchema& TestTickRow::GetSchema()
    {
        static const TableSchema schema = {
            "t_test_tick", TestTickRowFields, 7, nullptr, 0, DeallocateTestTickRow, nullptr, 0,
        };
        return schema;
    }
}

static void TestDuckdbVectorized()
{
    using namespace Mdb;
    DuckdbWrapper* duckdb = new DuckdbWrapper(":memory:");
    WriteLog(LogLevel::Info, "TestDB with DuckdbVectorized");

    duckdb->Exec("CREATE TABLE t_test_tick (TradingDay VARCHAR, InstrumentId VARCHAR, "
                 "LastPrice DECIMAL(24,8), PreClosePrice DOUBLE, Volume BIGINT, "
                 "BarPeriod INTEGER, IsValid BOOLEAN);");
    duckdb->Exec("INSERT INTO t_test_tick VALUES "
                 "('20260101', 'rb2610', 1234.50000000, 1200.0, 100, 60, true), "
                 "('20260102', 'rb2611', 1250.25000000, NULL, 200, 60, false);");

    RecordFactory factory = {
        []() -> void* { return TestTickRow::Allocate(); },
        [](void* records, void* record) {
            static_cast<std::vector<TestTickRow*>*>(records)->push_back(
                static_cast<TestTickRow*>(record));
        },
    };

    std::vector<TestTickRow*> records;
    std::string error = duckdb->SelectWithSqlVectorized(
        "SELECT TradingDay, InstrumentId, LastPrice, PreClosePrice, Volume, BarPeriod, IsValid "
        "FROM t_test_tick ORDER BY TradingDay;",
        &TestTickRow::GetSchema(), &records, factory);

    if (!error.empty())
    {
        WriteLog(LogLevel::Error, "SelectWithSqlVectorized Error: %s", error.c_str());
    }
    if (records.size() != 2)
    {
        WriteLog(LogLevel::Error, "Vectorized record count mismatch, expected 2 got %d", (int)records.size());
    }
    else
    {
        auto& row0 = *records[0];
        auto& row1 = *records[1];
        bool pass = strcmp(row0.TradingDay, "20260101") == 0
            && strcmp(row0.InstrumentId, "rb2610") == 0
            && row0.LastPrice == 1234.5
            && row0.PreClosePrice == 1200.0
            && row0.Volume == 100
            && row0.BarPeriod == 60
            && row0.IsValid
            && strcmp(row1.InstrumentId, "rb2611") == 0
            && row1.LastPrice == 1250.25
            && row1.PreClosePrice == std::numeric_limits<double>::infinity()
            && row1.Volume == 200
            && !row1.IsValid;
        if (pass)
        {
            WriteLog(LogLevel::Info, "TestDuckdbVectorized PASS");
        }
        else
        {
            WriteLog(LogLevel::Error, "TestDuckdbVectorized FAILED");
            for (auto record : records)
            {
                WriteLog(LogLevel::Error,
                    "row: day=%s inst=%s last=%f preclose=%f vol=%lld period=%d valid=%d",
                    record->TradingDay, record->InstrumentId, record->LastPrice,
                    record->PreClosePrice, record->Volume, record->BarPeriod,
                    record->IsValid ? 1 : 0);
            }
        }
    }
    for (auto record : records)
    {
        record->Deallocate();
    }

    // 错误透出：非法 SQL 应返回非空错误信息
    std::vector<TestTickRow*> badRecords;
    std::string badError = duckdb->SelectWithSqlVectorized(
        "SELECT * FROM no_such_table;", &TestTickRow::GetSchema(), &badRecords, factory);
    if (!badError.empty())
    {
        WriteLog(LogLevel::Info, "TestDuckdbVectorized ErrorPropagation OK: %s", badError.c_str());
    }
    else
    {
        WriteLog(LogLevel::Error, "TestDuckdbVectorized ErrorPropagation FAILED (empty error)");
    }

    delete duckdb;
}

// ============ 多 chunk 回退路径测试：BIGINT→Char、DOUBLE→Int64 ============

namespace Mdb
{
    class TestMultiChunkRow
    {
    public:
        char TradingDay[16];    // BIGINT 源 -> Char 字段（走 duckdb_value_string 回退）
        long long Volume;       // DOUBLE 源 -> Int64 字段（走 duckdb_value_int64 回退）
        double LastPrice;       // DECIMAL 源 -> Double 字段（向量快路径）

        static TestMultiChunkRow* Allocate() { return new TestMultiChunkRow(); }
        void Deallocate() { delete this; }
        static const TableSchema& GetSchema();
    };

    static const FieldDescriptor TestMultiChunkRowFields[] = {
        {"TradingDay", FieldType::Char,   offsetof(TestMultiChunkRow, TradingDay), sizeof(TestMultiChunkRow::TradingDay)},
        {"Volume",     FieldType::Int64,  offsetof(TestMultiChunkRow, Volume),     0},
        {"LastPrice",  FieldType::Double, offsetof(TestMultiChunkRow, LastPrice),  0},
    };
    static void DeallocateTestMultiChunkRow(void* record)
    {
        static_cast<TestMultiChunkRow*>(record)->Deallocate();
    }
    const TableSchema& TestMultiChunkRow::GetSchema()
    {
        static const TableSchema schema = {
            "t_test_multichunk", TestMultiChunkRowFields, 3, nullptr, 0,
            DeallocateTestMultiChunkRow, nullptr, 0,
        };
        return schema;
    }
}

static void TestDuckdbVectorizedMultiChunk()
{
    using namespace Mdb;
    DuckdbWrapper* duckdb = new DuckdbWrapper(":memory:");
    WriteLog(LogLevel::Info, "TestDB with DuckdbVectorizedMultiChunk");

    duckdb->Exec("CREATE TABLE t_test_multichunk (TradingDay BIGINT, Volume DOUBLE, LastPrice DECIMAL(24,8));");
    // 6000 行 -> 3 个 chunk（每 chunk 2048 行），跨 chunk 边界验证回退路径的行索引
    {
        std::string sql = "INSERT INTO t_test_multichunk SELECT "
            "20000000 + i AS TradingDay, i AS Volume, Cast(100.5 + i AS DECIMAL(24,8)) AS LastPrice "
            "FROM range(0, 6000) t(i);";
        duckdb->Exec(sql.c_str());
    }

    RecordFactory factory = {
        []() -> void* { return TestMultiChunkRow::Allocate(); },
        [](void* records, void* record) {
            static_cast<std::vector<TestMultiChunkRow*>*>(records)->push_back(
                static_cast<TestMultiChunkRow*>(record));
        },
    };

    std::vector<TestMultiChunkRow*> records;
    std::string error = duckdb->SelectWithSqlVectorized(
        "SELECT TradingDay, Volume, LastPrice FROM t_test_multichunk ORDER BY Volume;",
        &TestMultiChunkRow::GetSchema(), &records, factory);

    if (!error.empty())
    {
        WriteLog(LogLevel::Error, "MultiChunk Error: %s", error.c_str());
    }
    bool pass = records.size() == 6000;
    int failedRow = -1;
    if (pass)
    {
        for (int i = 0; i < 6000; ++i)
        {
            char expectedDay[16];
            snprintf(expectedDay, sizeof(expectedDay), "%d", 20000000 + i);
            if (strcmp(records[i]->TradingDay, expectedDay) != 0
                || records[i]->Volume != i
                || records[i]->LastPrice != 100.5 + i)
            {
                pass = false;
                failedRow = i;
                break;
            }
        }
    }
    if (pass)
    {
        WriteLog(LogLevel::Info, "TestDuckdbVectorizedMultiChunk PASS");
    }
    else
    {
        WriteLog(LogLevel::Error, "TestDuckdbVectorizedMultiChunk FAILED, failedRow=%d", failedRow);
        for (int k = failedRow - 1; k <= failedRow + 1; ++k)
        {
            if (k >= 0 && k < (int)records.size())
            {
                char expectedDay[16];
                snprintf(expectedDay, sizeof(expectedDay), "%d", 20000000 + k);
                WriteLog(LogLevel::Error,
                    "  idx=%d expected day=%s vol=%d last=%.1f | got day=%s vol=%lld last=%f",
                    k, expectedDay, k, 100.5 + k,
                    records[k]->TradingDay, records[k]->Volume, records[k]->LastPrice);
            }
        }
    }
    for (auto record : records)
    {
        record->Deallocate();
    }
    delete duckdb;
}

// ============ 窄整数/无符号整数落库测试 ============

namespace Mdb
{
    // 成员相邻排布：旧实现把所有窄整数按 4 字节 Int 读写，会越界写坏相邻成员。
    // Allocate 故意填 0xAB 脏值，"NULL 列写 0"与"漏改分支保留脏值"才能被区分开。
    class TestNarrowRow
    {
    public:
        int32_t  HeadGuard;
        int8_t   MinInt8;
        uint8_t  UInt8Value;
        int16_t  MinInt16;
        uint16_t UInt16Value;
        int32_t  BodyGuard;
        uint32_t UInt32Value;
        uint64_t UInt64Value;
        int32_t  TailGuard;

        static TestNarrowRow* Allocate()
        {
            TestNarrowRow* row = new TestNarrowRow();
            std::memset(row, 0xAB, sizeof(TestNarrowRow));
            return row;
        }
        void Deallocate() { delete this; }
        static const TableSchema& GetSchema();
    };

    static const int32_t  kHeadGuardValue = 0x11223344;
    static const int32_t  kBodyGuardValue = 0x55667788;
    static const int32_t  kTailGuardValue = 0x0A0B0C0D;
    static const uint32_t kUInt32Value    = 0xFFFFFFFFu;
    static const uint64_t kUInt64Value    = 0xFFFFFFFFFFFFFFFFull;

    static const FieldDescriptor TestNarrowRowFields[] = {
        {"HeadGuard",   FieldType::Int32,  offsetof(TestNarrowRow, HeadGuard),   0},
        {"MinInt8",     FieldType::Int8,   offsetof(TestNarrowRow, MinInt8),     0},
        {"UInt8Value",  FieldType::UInt8,  offsetof(TestNarrowRow, UInt8Value),  0},
        {"MinInt16",    FieldType::Int16,  offsetof(TestNarrowRow, MinInt16),    0},
        {"UInt16Value", FieldType::UInt16, offsetof(TestNarrowRow, UInt16Value), 0},
        {"BodyGuard",   FieldType::Int32,  offsetof(TestNarrowRow, BodyGuard),   0},
        {"UInt32Value", FieldType::UInt32, offsetof(TestNarrowRow, UInt32Value), 0},
        {"UInt64Value", FieldType::UInt64, offsetof(TestNarrowRow, UInt64Value), 0},
        {"TailGuard",   FieldType::Int32,  offsetof(TestNarrowRow, TailGuard),   0},
    };
    static void DeallocateTestNarrowRow(void* record)
    {
        static_cast<TestNarrowRow*>(record)->Deallocate();
    }
    const TableSchema& TestNarrowRow::GetSchema()
    {
        static const TableSchema schema = {
            "t_test_narrow", TestNarrowRowFields, 9, nullptr, 0,
            DeallocateTestNarrowRow, nullptr, 0,
        };
        return schema;
    }

    static TestNarrowRow* MakeNarrowRow()
    {
        TestNarrowRow* row = TestNarrowRow::Allocate();
        row->HeadGuard   = kHeadGuardValue;
        row->MinInt8     = std::numeric_limits<int8_t>::min();
        row->UInt8Value  = 200;
        row->MinInt16    = std::numeric_limits<int16_t>::min();
        row->UInt16Value = std::numeric_limits<uint16_t>::max();
        row->BodyGuard   = kBodyGuardValue;
        row->UInt32Value = kUInt32Value;
        row->UInt64Value = kUInt64Value;
        row->TailGuard   = kTailGuardValue;
        return row;
    }

    // SQLite 无无符号列，UInt64 ≥2^63 在绑定侧按 INT64_MAX 饱和，故期望值随后端不同。
    static bool IsNarrowRowAsExpected(const TestNarrowRow& row, uint64_t expectedUInt64Value)
    {
        return row.HeadGuard   == kHeadGuardValue
            && row.MinInt8     == std::numeric_limits<int8_t>::min()
            && row.UInt8Value  == 200
            && row.MinInt16    == std::numeric_limits<int16_t>::min()
            && row.UInt16Value == std::numeric_limits<uint16_t>::max()
            && row.BodyGuard   == kBodyGuardValue
            && row.UInt32Value == kUInt32Value
            && row.UInt64Value == expectedUInt64Value
            && row.TailGuard   == kTailGuardValue;
    }

    static void DumpNarrowRow(const char* backend, const TestNarrowRow& row)
    {
        WriteLog(LogLevel::Error,
            "%s narrow row: head=%d i8=%d u8=%u i16=%d u16=%u body=%d u32=%u u64=%llu tail=%d",
            backend, row.HeadGuard, (int)row.MinInt8, (unsigned)row.UInt8Value,
            (int)row.MinInt16, (unsigned)row.UInt16Value, row.BodyGuard,
            (unsigned)row.UInt32Value, (unsigned long long)row.UInt64Value, row.TailGuard);
    }
}

static RecordFactory MakeNarrowRowFactory()
{
    return RecordFactory{
        []() -> void* { return Mdb::TestNarrowRow::Allocate(); },
        [](void* records, void* record) {
            static_cast<std::vector<Mdb::TestNarrowRow*>*>(records)->push_back(
                static_cast<Mdb::TestNarrowRow*>(record));
        },
    };
}

// 建表 + 插入一行边界值。SQLite 会在 UInt64 绑定侧饱和并告警（属预期输出）。
template <typename Wrapper>
static void InsertNarrowRow(Wrapper* db)
{
    const TableSchema& schema = Mdb::TestNarrowRow::GetSchema();
    db->CreateTable(&schema);
    Mdb::TestNarrowRow* row = Mdb::MakeNarrowRow();
    db->Insert(&schema, row);
    row->Deallocate();
}

// 结果级读路径：Sqlite 与 Duckdb 都实现（Duckdb 走 duckdb_value_* 派发）。
template <typename Wrapper>
static void SelectNarrowRows(Wrapper* db, std::vector<Mdb::TestNarrowRow*>& records)
{
    RecordFactory factory = MakeNarrowRowFactory();
    db->SelectAll(&Mdb::TestNarrowRow::GetSchema(), &records, factory);
}

// chunk 读路径：只有 Duckdb 有，走 BindChunkToRecords 与 WriteNullSentinel。
static void SelectNarrowRowsVectorized(DuckdbWrapper* db, std::vector<Mdb::TestNarrowRow*>& records)
{
    RecordFactory factory = MakeNarrowRowFactory();
    std::string error = db->SelectWithSqlVectorized("SELECT * FROM t_test_narrow;",
        &Mdb::TestNarrowRow::GetSchema(), &records, factory);
    if (!error.empty())
    {
        WriteLog(LogLevel::Error, "Duckdb narrow vectorized error: %s", error.c_str());
    }
}

static void CheckNarrowRows(const char* backend, std::vector<Mdb::TestNarrowRow*>& records,
                            uint64_t expectedUInt64Value)
{
    bool pass = records.size() == 1;
    if (pass)
    {
        pass = Mdb::IsNarrowRowAsExpected(*records[0], expectedUInt64Value);
        if (!pass)
        {
            Mdb::DumpNarrowRow(backend, *records[0]);
        }
    }
    else
    {
        WriteLog(LogLevel::Error, "%s narrow record count mismatch, expected 1 got %d",
            backend, (int)records.size());
    }
    for (auto record : records)
    {
        record->Deallocate();
    }
    records.clear();
    if (pass)
    {
        WriteLog(LogLevel::Info, "%s NarrowInteger PASS", backend);
    }
    else
    {
        WriteLog(LogLevel::Error, "%s NarrowInteger FAILED", backend);
    }
}

// Allocate 已填 0xAB 脏值：NULL 列必须被写成 0，而不是留下脏值。
static void CheckNarrowNullSentinel(const char* backend, std::vector<Mdb::TestNarrowRow*>& records)
{
    bool pass = records.size() == 1
        && records[0]->MinInt8 == 0
        && records[0]->UInt8Value == 0
        && records[0]->MinInt16 == 0
        && records[0]->UInt16Value == 0
        && records[0]->UInt32Value == 0
        && records[0]->UInt64Value == 0;
    if (!pass && !records.empty())
    {
        Mdb::DumpNarrowRow(backend, *records[0]);
    }
    for (auto record : records)
    {
        record->Deallocate();
    }
    records.clear();
    if (pass)
    {
        WriteLog(LogLevel::Info, "%s NarrowNullSentinel PASS", backend);
    }
    else
    {
        WriteLog(LogLevel::Error, "%s NarrowNullSentinel FAILED", backend);
    }
}

static void TestSqliteNarrowInteger()
{
    SqliteWrapper* sqlite = new SqliteWrapper(":memory:");
    WriteLog(LogLevel::Info, "TestDB with SqliteNarrow");
    InsertNarrowRow(sqlite);

    std::vector<Mdb::TestNarrowRow*> records;
    // SQLite 的 8 字节整数是二补数且无无符号列：0xFFFFFFFFFFFFFFFF 在绑定侧饱和到 INT64_MAX
    SelectNarrowRows(sqlite, records);
    CheckNarrowRows("Sqlite", records, (uint64_t)std::numeric_limits<int64_t>::max());

    sqlite->Exec("DELETE FROM t_test_narrow;");
    sqlite->Exec("INSERT INTO t_test_narrow (HeadGuard) VALUES (1);");
    SelectNarrowRows(sqlite, records);
    CheckNarrowNullSentinel("Sqlite", records);
    delete sqlite;
}

static void TestSqliteNarrowSaturation()
{
    SqliteWrapper* sqlite = new SqliteWrapper(":memory:");
    WriteLog(LogLevel::Info, "TestDB with SqliteNarrowSaturation");
    // 手工建宽列：用 Wrapper 自己的 DDL 会把列建成窄类型，INSERT 阶段就失败，
    // 读侧的收窄分支一行也走不到。期望 6 格饱和 + 一条 narrowed-out-of-range Warning。
    sqlite->Exec("CREATE TABLE t_test_narrow_saturate (HeadGuard INTEGER, MinInt8 INTEGER, "
                 "UInt8Value INTEGER, MinInt16 INTEGER, UInt16Value INTEGER, BodyGuard INTEGER, "
                 "UInt32Value INTEGER, UInt64Value INTEGER, TailGuard INTEGER);");
    sqlite->Exec("INSERT INTO t_test_narrow_saturate VALUES (1, 300, -5, -40000, 70000, "
                 "3000000000, 5000000000, 2, 3);");

    std::vector<Mdb::TestNarrowRow*> records;
    RecordFactory factory = MakeNarrowRowFactory();
    sqlite->SelectWithSql("SELECT * FROM t_test_narrow_saturate;", &Mdb::TestNarrowRow::GetSchema(),
                          &records, factory);

    bool pass = records.size() == 1;
    if (pass)
    {
        const auto& row = *records[0];
        pass = row.HeadGuard == 1
            && row.MinInt8 == std::numeric_limits<int8_t>::max()
            && row.UInt8Value == 0
            && row.MinInt16 == std::numeric_limits<int16_t>::min()
            && row.UInt16Value == std::numeric_limits<uint16_t>::max()
            && row.BodyGuard == std::numeric_limits<int32_t>::max()
            && row.UInt32Value == std::numeric_limits<uint32_t>::max()
            && row.UInt64Value == 2
            && row.TailGuard == 3;
        if (!pass)
        {
            Mdb::DumpNarrowRow("SqliteSaturation", row);
        }
    }
    for (auto record : records)
    {
        record->Deallocate();
    }
    if (pass)
    {
        WriteLog(LogLevel::Info, "Sqlite NarrowSaturation PASS");
    }
    else
    {
        WriteLog(LogLevel::Error, "Sqlite NarrowSaturation FAILED");
    }
    delete sqlite;
}

static void TestDuckdbNarrowInteger()
{
    DuckdbWrapper* duckdb = new DuckdbWrapper(":memory:");
    WriteLog(LogLevel::Info, "TestDB with DuckdbNarrow");
    InsertNarrowRow(duckdb);

    std::vector<Mdb::TestNarrowRow*> records;
    SelectNarrowRows(duckdb, records);
    CheckNarrowRows("Duckdb", records, Mdb::kUInt64Value);
    SelectNarrowRowsVectorized(duckdb, records);
    CheckNarrowRows("DuckdbVectorized", records, Mdb::kUInt64Value);

    // NULL 哨兵只走 chunk 路径：WriteNullSentinel 是那条路径独有的分支
    duckdb->Exec("DELETE FROM t_test_narrow;");
    duckdb->Exec("INSERT INTO t_test_narrow (HeadGuard) VALUES (1);");
    SelectNarrowRowsVectorized(duckdb, records);
    CheckNarrowNullSentinel("DuckdbVectorized", records);
    delete duckdb;
}

static void TestDuckdbNarrowSaturation()
{
    DuckdbWrapper* duckdb = new DuckdbWrapper(":memory:");
    WriteLog(LogLevel::Info, "TestDB with DuckdbNarrowSaturation");
    duckdb->Exec("CREATE TABLE t_test_narrow_saturate (HeadGuard INTEGER, MinInt8 INTEGER, "
                 "UInt8Value INTEGER, MinInt16 INTEGER, UInt16Value INTEGER, BodyGuard BIGINT, "
                 "UInt32Value BIGINT, UInt64Value BIGINT, TailGuard INTEGER);");
    duckdb->Exec("INSERT INTO t_test_narrow_saturate VALUES (1, 300, -5, -40000, 70000, "
                 "3000000000, 5000000000, 2, 3);");

    std::vector<Mdb::TestNarrowRow*> records;
    RecordFactory factory = MakeNarrowRowFactory();
    std::string error = duckdb->SelectWithSqlVectorized(
        "SELECT * FROM t_test_narrow_saturate;", &Mdb::TestNarrowRow::GetSchema(),
        &records, factory);
    if (!error.empty())
    {
        WriteLog(LogLevel::Error, "Duckdb narrow saturation error: %s", error.c_str());
    }

    bool pass = records.size() == 1;
    if (pass)
    {
        const auto& row = *records[0];
        pass = row.HeadGuard == 1
            && row.MinInt8 == std::numeric_limits<int8_t>::max()
            && row.UInt8Value == 0
            && row.MinInt16 == std::numeric_limits<int16_t>::min()
            && row.UInt16Value == std::numeric_limits<uint16_t>::max()
            && row.BodyGuard == std::numeric_limits<int32_t>::max()
            && row.UInt32Value == std::numeric_limits<uint32_t>::max()
            && row.UInt64Value == 2
            && row.TailGuard == 3;
        if (!pass)
        {
            Mdb::DumpNarrowRow("DuckdbSaturation", row);
        }
    }
    for (auto record : records)
    {
        record->Deallocate();
    }
    if (pass)
    {
        WriteLog(LogLevel::Info, "Duckdb NarrowSaturation PASS");
    }
    else
    {
        WriteLog(LogLevel::Error, "Duckdb NarrowSaturation FAILED");
    }
    delete duckdb;
}

// 往返测试验不出列类型（Duckdb 对窄整数有隐式转换），直接查 typeof 断言 DDL 映射。
namespace Mdb
{
    class TestTypeNameRow
    {
    public:
        char MinInt8Type[16];
        char UInt8Type[16];
        char MinInt16Type[16];
        char UInt16Type[16];
        char UInt32Type[16];
        char UInt64Type[16];
        char HeadGuardType[16];

        static TestTypeNameRow* Allocate() { return new TestTypeNameRow(); }
        void Deallocate() { delete this; }
        static const TableSchema& GetSchema();
    };

    static const FieldDescriptor TestTypeNameRowFields[] = {
        {"MinInt8Type",   FieldType::Char, offsetof(TestTypeNameRow, MinInt8Type),   sizeof(TestTypeNameRow::MinInt8Type)},
        {"UInt8Type",     FieldType::Char, offsetof(TestTypeNameRow, UInt8Type),     sizeof(TestTypeNameRow::UInt8Type)},
        {"MinInt16Type",  FieldType::Char, offsetof(TestTypeNameRow, MinInt16Type),  sizeof(TestTypeNameRow::MinInt16Type)},
        {"UInt16Type",    FieldType::Char, offsetof(TestTypeNameRow, UInt16Type),    sizeof(TestTypeNameRow::UInt16Type)},
        {"UInt32Type",    FieldType::Char, offsetof(TestTypeNameRow, UInt32Type),    sizeof(TestTypeNameRow::UInt32Type)},
        {"UInt64Type",    FieldType::Char, offsetof(TestTypeNameRow, UInt64Type),    sizeof(TestTypeNameRow::UInt64Type)},
        {"HeadGuardType", FieldType::Char, offsetof(TestTypeNameRow, HeadGuardType), sizeof(TestTypeNameRow::HeadGuardType)},
    };
    static void DeallocateTestTypeNameRow(void* record)
    {
        static_cast<TestTypeNameRow*>(record)->Deallocate();
    }
    const TableSchema& TestTypeNameRow::GetSchema()
    {
        static const TableSchema schema = {
            "t_test_typename", TestTypeNameRowFields, 7, nullptr, 0,
            DeallocateTestTypeNameRow, nullptr, 0,
        };
        return schema;
    }
}

static void TestDuckdbNarrowColumnTypes()
{
    DuckdbWrapper* duckdb = new DuckdbWrapper(":memory:");
    WriteLog(LogLevel::Info, "TestDB with DuckdbNarrowColumnTypes");
    InsertNarrowRow(duckdb);

    RecordFactory factory = {
        []() -> void* { return Mdb::TestTypeNameRow::Allocate(); },
        [](void* records, void* record) {
            static_cast<std::vector<Mdb::TestTypeNameRow*>*>(records)->push_back(
                static_cast<Mdb::TestTypeNameRow*>(record));
        },
    };
    std::vector<Mdb::TestTypeNameRow*> records;
    duckdb->SelectWithSql(
        "SELECT typeof(MinInt8), typeof(UInt8Value), typeof(MinInt16), typeof(UInt16Value), "
        "typeof(UInt32Value), typeof(UInt64Value), typeof(HeadGuard) FROM t_test_narrow;",
        &Mdb::TestTypeNameRow::GetSchema(), &records, factory);

    bool pass = records.size() == 1;
    if (pass)
    {
        const auto& row = *records[0];
        pass = strcmp(row.MinInt8Type, "TINYINT") == 0
            && strcmp(row.UInt8Type, "UTINYINT") == 0
            && strcmp(row.MinInt16Type, "SMALLINT") == 0
            && strcmp(row.UInt16Type, "USMALLINT") == 0
            && strcmp(row.UInt32Type, "UINTEGER") == 0
            && strcmp(row.UInt64Type, "UBIGINT") == 0
            && strcmp(row.HeadGuardType, "INTEGER") == 0;
        if (!pass)
        {
            WriteLog(LogLevel::Error,
                "Duckdb column types: i8=%s u8=%s i16=%s u16=%s u32=%s u64=%s guard=%s",
                row.MinInt8Type, row.UInt8Type, row.MinInt16Type, row.UInt16Type,
                row.UInt32Type, row.UInt64Type, row.HeadGuardType);
        }
    }
    else
    {
        WriteLog(LogLevel::Error, "Duckdb typeof query returned %d rows, expected 1",
            (int)records.size());
    }
    for (auto record : records)
    {
        record->Deallocate();
    }
    if (pass)
    {
        WriteLog(LogLevel::Info, "Duckdb NarrowColumnTypes PASS");
    }
    else
    {
        WriteLog(LogLevel::Error, "Duckdb NarrowColumnTypes FAILED");
    }
    delete duckdb;
}

// 失败可见性验证：以下操作刻意打在不存在的库/表上，日志中应出现对应 ERROR 行（属预期输出，不代表测试失败）
static void TestFailureVisibility()
{
    WriteLog(LogLevel::Info, "===== 失败可见性验证开始：其后 ERROR 为刻意触发 =====");

    const TableSchema& schema = Mdb::TestTickRow::GetSchema();
    Mdb::TestTickRow tick;
    memset(&tick, 0, sizeof(Mdb::TestTickRow));
    RecordFactory factory = {
        []() -> void* { return Mdb::TestTickRow::Allocate(); },
        [](void* records, void* record) {
            static_cast<std::vector<Mdb::TestTickRow*>*>(records)->push_back(
                static_cast<Mdb::TestTickRow*>(record));
        },
    };
    const int keyIndices[] = { 0, 1 };

    // 1) 目录不存在：构造期报 Open database failed；句柄为空时建表/插入/查询/带事务的批量写都应逐条上报
    SqliteWrapper unavailableSqlite("./NoSuchDir/NoSuchFile.sqlitedb");
    unavailableSqlite.CreateTable(&schema);
    unavailableSqlite.Insert(&schema, &tick);
    const void* twoRecords[] = { &tick, &tick };
    unavailableSqlite.BatchInsert(&schema, twoRecords, 2);
    std::vector<Mdb::TestTickRow*> unavailableRecords;
    unavailableSqlite.SelectAll(&schema, &unavailableRecords, factory);

    // 2) 库可打开但表不存在：语句级失败（prepare/step）与整批失败规模都应上报
    SqliteWrapper missingTableSqlite(":memory:");
    missingTableSqlite.Insert(&schema, &tick);
    missingTableSqlite.BatchInsert(&schema, twoRecords, 2);
    missingTableSqlite.Update(&schema, &tick);
    missingTableSqlite.Delete(&schema, &tick, keyIndices, 2);
    std::vector<Mdb::TestTickRow*> missingTableRecords;
    missingTableSqlite.SelectAll(&schema, &missingTableRecords, factory);

    DuckdbWrapper missingTableDuckdb(":memory:");
    missingTableDuckdb.Insert(&schema, &tick);
    missingTableDuckdb.BatchInsert(&schema, twoRecords, 2);
    std::vector<Mdb::TestTickRow*> missingTableDuckdbRecords;
    missingTableDuckdb.SelectAll(&schema, &missingTableDuckdbRecords, factory);

    for (auto record : unavailableRecords)
    {
        record->Deallocate();
    }
    for (auto record : missingTableRecords)
    {
        record->Deallocate();
    }
    for (auto record : missingTableDuckdbRecords)
    {
        record->Deallocate();
    }
    WriteLog(LogLevel::Info, "===== 失败可见性验证结束 =====");
}

int main(int argc, char* argv[])
{
	Logger::GetInstance().Init(argv[0]);
	Logger::GetInstance().SetLogLevel(LogLevel::Info, LogLevel::Info);
	Logger::GetInstance().Start();

    TestSqlite();
    TestDuckdb();
    TestDuckdbVectorized();
    TestDuckdbVectorizedMultiChunk();
    TestSqliteNarrowInteger();
    TestSqliteNarrowSaturation();
    TestDuckdbNarrowInteger();
    TestDuckdbNarrowSaturation();
    TestDuckdbNarrowColumnTypes();
    TestFailureVisibility();
    //TestMysql();
    //TestMariadb();

	Logger::GetInstance().Stop();
	Logger::GetInstance().Join();
	return 0;
}
