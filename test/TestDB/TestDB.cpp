#include "MdbStructs.h"
#include <DBAdapters/SqliteWrapper/SqliteWrapper.h>
#include <DBAdapters/DuckdbWrapper/DuckdbWrapper.h>
#include <DBAdapters/MysqlWrapper/MysqlWrapper.h>
#include <DBAdapters/MariadbWrapper/MariadbWrapper.h>
#include <DBAdapters/DBInterface/TypedTable.h>
#include <DBAdapters/DBInterface/SchemaRegistry.h>
#include <DBAdapters/AsyncDBWriter/AsyncDBWriter.h>
#include <Spark/Core/Core.h>
#include <cstring>
#include <iostream>
#include <limits>


using namespace std;
using namespace mdb;

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
	strcpy(exchange1->ExchangeID, "SHFE");
	strcpy(exchange1->ExchangeName, (const char*)(u8"上海期货交易所"));
	strcpy(exchange2->ExchangeID, "INE");
	strcpy(exchange2->ExchangeName, (const char*)(u8"上海国际能源交易中心"));
	strcpy(exchange3->ExchangeID, "CFFEX");
	strcpy(exchange3->ExchangeName, (const char*)(u8"中国金融期货交易所"));
	strcpy(exchange4->ExchangeID, "CZCE");
	strcpy(exchange4->ExchangeName, (const char*)(u8"郑州商品期货交易所"));
	strcpy(exchange5->ExchangeID, "DCE");
	strcpy(exchange5->ExchangeName, (const char*)(u8"大连商品期货交易所"));
	strcpy(exchange6->ExchangeID, "GFE");
	strcpy(exchange6->ExchangeName, (const char*)(u8"广期所"));

	exchanges->push_back(exchange1);
	exchanges->push_back(exchange2);
	exchanges->push_back(exchange3);
	exchanges->push_back(exchange4);
	exchanges->push_back(exchange5);
	exchanges->push_back(exchange6);
	return exchanges;
}
Account* PrepareAccount(const char* accountID, const char* accountName, const char* password)
{
	Account* account = new Account();
	memset(account, 0, sizeof(Account));
	strcpy(account->AccountID, accountID);
	strcpy(account->AccountName, accountName);
	account->AccountType = AccountTypeType::Primary;
	account->AccountStatus = AccountStatusType::Normal;
	strcpy(account->Password, "123456");
	account->TradeGroupID = 10000;
	account->RiskGroupID = 10000;
	account->CommissionGroupID = 10000;
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

namespace mdb
{
    class TestTickRow
    {
    public:
        char TradingDay[9];
        char InstrumentID[16];
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
        {"InstrumentID",  FieldType::Char,   offsetof(TestTickRow, InstrumentID),  sizeof(TestTickRow::InstrumentID)},
        {"LastPrice",     FieldType::Double, offsetof(TestTickRow, LastPrice),     0},
        {"PreClosePrice", FieldType::Double, offsetof(TestTickRow, PreClosePrice), 0},
        {"Volume",        FieldType::Int64,  offsetof(TestTickRow, Volume),        0},
        {"BarPeriod",     FieldType::Int,    offsetof(TestTickRow, BarPeriod),     0},
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
    using namespace mdb;
    DuckdbWrapper* duckdb = new DuckdbWrapper(":memory:");
    WriteLog(LogLevel::Info, "TestDB with DuckdbVectorized");

    duckdb->Exec("CREATE TABLE t_test_tick (TradingDay VARCHAR, InstrumentID VARCHAR, "
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
        "SELECT TradingDay, InstrumentID, LastPrice, PreClosePrice, Volume, BarPeriod, IsValid "
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
            && strcmp(row0.InstrumentID, "rb2610") == 0
            && row0.LastPrice == 1234.5
            && row0.PreClosePrice == 1200.0
            && row0.Volume == 100
            && row0.BarPeriod == 60
            && row0.IsValid
            && strcmp(row1.InstrumentID, "rb2611") == 0
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
                    record->TradingDay, record->InstrumentID, record->LastPrice,
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

int main(int argc, char* argv[])
{
	Logger::GetInstance().Init(argv[0]);
	Logger::GetInstance().SetLogLevel(LogLevel::Info, LogLevel::Info);
	Logger::GetInstance().Start();

    TestSqlite();
    TestDuckdb();
    TestDuckdbVectorized();
    //TestMysql();
    //TestMariadb();

	Logger::GetInstance().Stop();
	Logger::GetInstance().Join();
	return 0;
}
