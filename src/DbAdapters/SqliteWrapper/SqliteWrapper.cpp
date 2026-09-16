#include <DBAdapters/SqliteWrapper/SqliteWrapper.h>

#include <DBAdapters/DBInterface/FailureLogThrottle.h>

#include <Spark/Core/Logger/Logger.h>

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <sstream>


namespace DbAdapters
{
    using Spark::Core::LogLevel;

    class StatementGuard
    {
    public:
	    StatementGuard(sqlite3* db, const char* sql)
	    {
		    sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
	    }
	    ~StatementGuard()
	    {
		    if (stmt_)
		    {
			    sqlite3_finalize(stmt_);
		    }
	    }
	    StatementGuard(const StatementGuard&) = delete;
	    StatementGuard& operator=(const StatementGuard&) = delete;

	    bool IsValid() const { return stmt_ != nullptr; }
	    int Step() const { return sqlite3_step(stmt_); }
	    sqlite3_stmt* Get() const { return stmt_; }

    private:
	    sqlite3_stmt* stmt_ = nullptr;
    };

    // SQLite 没有无符号整数列，所有整数列都是二补数 8 字节，因此绑定一律走 sqlite3_bind_int64：
    // 不用 sqlite3_bind_int，它会把 UInt32 的 4e9 截断。
    long long ReadIntegerFieldForBind(FieldType type, const char* data, int& clampedCount)
    {
	    switch (type)
	    {
	    case FieldType::Int8:   return *reinterpret_cast<const int8_t*>(data);
	    case FieldType::UInt8:  return *reinterpret_cast<const uint8_t*>(data);
	    case FieldType::Int16:  return *reinterpret_cast<const int16_t*>(data);
	    case FieldType::UInt16: return *reinterpret_cast<const uint16_t*>(data);
	    case FieldType::Int32:  return *reinterpret_cast<const int32_t*>(data);
	    case FieldType::UInt32: return *reinterpret_cast<const uint32_t*>(data);
	    case FieldType::Int64:  return *reinterpret_cast<const int64_t*>(data);
	    case FieldType::UInt64:
	    {
		    const uint64_t value = *reinterpret_cast<const uint64_t*>(data);
		    if (value > static_cast<uint64_t>(std::numeric_limits<long long>::max()))
		    {
			    // SQLite 的 8 字节整数是二补数：位模式可保真落库，但 ≥2^63 的值在 SQL 语义层
			    // （WHERE/ORDER BY/聚合）以负数存在，故按 INT64_MAX 饱和并计数上报，不做位模式直传。
			    ++clampedCount;
			    return std::numeric_limits<long long>::max();
		    }
		    return static_cast<long long>(value);
	    }
	    default:                return 0;
	    }
    }

    void BindField(sqlite3_stmt* stmt, int index, const FieldDescriptor& field, const void* record, int& clampedCount)
    {
	    const char* data = static_cast<const char*>(record) + field.offset;
	    switch (field.type)
	    {
	    case FieldType::Int8:
	    case FieldType::UInt8:
	    case FieldType::Int16:
	    case FieldType::UInt16:
	    case FieldType::Int32:
	    case FieldType::UInt32:
	    case FieldType::Int64:
	    case FieldType::UInt64:
		    sqlite3_bind_int64(stmt, index, ReadIntegerFieldForBind(field.type, data, clampedCount));
		    break;
	    case FieldType::Double:
		    sqlite3_bind_double(stmt, index, *reinterpret_cast<const double*>(data));
		    break;
	    case FieldType::Char:
		    sqlite3_bind_text(stmt, index, data, -1, nullptr);
		    break;
	    case FieldType::Bool:
		    sqlite3_bind_int(stmt, index, *reinterpret_cast<const bool*>(data) ? 1 : 0);
		    break;
	    }
    }
    void BindFields(sqlite3_stmt* stmt, const TableSchema* schema, const void* record, int& clampedCount)
    {
	    for (int i = 0; i < schema->fieldCount; ++i)
	    {
		    BindField(stmt, i + 1, schema->fields[i], record, clampedCount);
	    }
    }
    void BindKeyFields(sqlite3_stmt* stmt, const TableSchema* schema, const void* record, const int* keyIndices, int keyCount, int& clampedCount)
    {
	    for (int i = 0; i < keyCount; ++i)
	    {
		    BindField(stmt, i + 1, schema->fields[keyIndices[i]], record, clampedCount);
	    }
    }
    void ReadRow(sqlite3_stmt* stmt, const TableSchema* schema, void* record, int& clampedCount)
    {
	    char* data = static_cast<char*>(record);
	    for (int i = 0; i < schema->fieldCount; ++i)
	    {
		    const auto& field = schema->fields[i];
		    char* dest = data + field.offset;
		    switch (field.type)
		    {
		    case FieldType::Int8:
		    case FieldType::UInt8:
		    case FieldType::Int16:
		    case FieldType::UInt16:
		    case FieldType::Int32:
		    case FieldType::UInt32:
		    case FieldType::Int64:
		    case FieldType::UInt64:
			    if (!TryWriteIntegerFromSigned(sqlite3_column_int64(stmt, i), field.type, dest))
			    {
				    ++clampedCount;
			    }
			    break;
		    case FieldType::Double:
			    *reinterpret_cast<double*>(dest) = sqlite3_column_double(stmt, i);
			    break;
		    case FieldType::Char:
		    {
			    const unsigned char* text = sqlite3_column_text(stmt, i);
			    if (text)
			    {
				    std::size_t copy_len = static_cast<std::size_t>(field.arraySize);
				    int text_len = sqlite3_column_bytes(stmt, i);
				    if (static_cast<std::size_t>(text_len) < copy_len)
					    copy_len = static_cast<std::size_t>(text_len);
				    std::memcpy(dest, text, copy_len);
			    }
			    break;
		    }
		    case FieldType::Bool:
			    *reinterpret_cast<bool*>(dest) = sqlite3_column_int(stmt, i) != 0;
			    break;
		    }
	    }
    }

    // 收窄饱和在一次操作结束后汇总上报一条 Warning：逐格上报会被行数淹没。
    void LogClampedCells(const char* operationName, const TableSchema* schema, int clampedCount, int totalCellCount)
    {
	    if (clampedCount > 0)
	    {
		    WriteLog(LogLevel::Warning,
			    "SqliteWrapper: %s narrowed out-of-range values. Table:%s, ClampedCells:%d/%d",
			    operationName, schema->tableName, clampedCount, totalCellCount);
	    }
    }

    // 读语句到 EOF，把所有行交给 factory，返回最后一次 step 的返回码供调用方判错。
    int ReadStatementRows(sqlite3_stmt* stmt, const TableSchema* schema, void* recordsList, const RecordFactory& factory)
    {
	    int clampedCount = 0;
	    int totalRowCount = 0;
	    int rc = SQLITE_ROW;
	    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
	    {
		    void* record = factory.Allocate();
		    ReadRow(stmt, schema, record, clampedCount);
		    factory.PushBack(recordsList, record);
		    ++totalRowCount;
	    }
	    LogClampedCells("SELECT", schema, clampedCount, totalRowCount * schema->fieldCount);
	    return rc;
    }

struct SqliteWrapper::Impl
{
	sqlite3* db = nullptr;
	FailureLogThrottle failureLogThrottle;
};

SqliteWrapper::SqliteWrapper(const std::string& dbName)
	: impl_(new Impl)
{
	int rc = sqlite3_open(dbName.c_str(), &impl_->db);
	if (rc != SQLITE_OK)
	{
		// open 失败时 SQLite 仍会返回句柄（仅 SQLITE_NOMEM 下可能为 nullptr），错误文本须在其关闭前取出
		const char* errorDetail = impl_->db != nullptr ? sqlite3_errmsg(impl_->db) : sqlite3_errstr(rc);
		WriteLog(LogLevel::Error, "SqliteWrapper: Open database failed. Path:%s, ReturnCode:%d, Error:%s", dbName.c_str(), rc, errorDetail);
		if (impl_->db != nullptr)
		{
			sqlite3_close(impl_->db);
		}
		impl_->db = nullptr;
		return;
	}
	sqlite3_exec(impl_->db, "PRAGMA encoding = 'UTF-8';", nullptr, nullptr, nullptr);

	StatementGuard stmt(impl_->db, "PRAGMA encoding;");
	if (stmt.IsValid() && stmt.Step() == SQLITE_ROW)
	{
		const unsigned char* enc = sqlite3_column_text(stmt.Get(), 0);
		std::printf("[SqliteWrapper] Database encoding: %s\n", enc ? (const char*)enc : "NULL");
	}
}
SqliteWrapper::~SqliteWrapper()
{
	DisConnect();
	delete impl_;
}

bool SqliteWrapper::Connect()
{
	return impl_->db != nullptr;
}
void SqliteWrapper::DisConnect()
{
	if (impl_->db)
	{
		sqlite3_close(impl_->db);
		impl_->db = nullptr;
	}
}
void SqliteWrapper::Exec(const char* sql)
{
	if (impl_->db == nullptr)
	{
		// 句柄为空时所有语句都会静默失效（含建表与批量写的事务控制），必须显式记录
		WriteLog(LogLevel::Error, "SqliteWrapper: EXEC skipped, database is not open. Sql:%s", sql);
		return;
	}
	char* errMsg = nullptr;
	const int rc = sqlite3_exec(impl_->db, sql, nullptr, nullptr, &errMsg);
	if (rc != SQLITE_OK)
	{
		WriteLog(LogLevel::Error, "SqliteWrapper: EXEC failed. ReturnCode:%d, Error:%s, Sql:%s",
			rc, errMsg != nullptr ? errMsg : sqlite3_errstr(rc), sql);
	}
	if (errMsg)
	{
		sqlite3_free(errMsg);
	}
}

void SqliteWrapper::CreateTable(const TableSchema* schema)
{
	std::ostringstream sql;
	sql << "CREATE TABLE IF NOT EXISTS \"" << schema->tableName << "\"(";
	for (int i = 0; i < schema->fieldCount; ++i)
	{
		if (i > 0) sql << ", ";
		const auto& f = schema->fields[i];
		sql << f.name << " ";
		switch (f.type)
		{
		case FieldType::Int8:
		case FieldType::UInt8:
		case FieldType::Int16:
		case FieldType::UInt16:
		case FieldType::Int32:
		case FieldType::UInt32: sql << "int"; break;
		case FieldType::Int64:
		case FieldType::UInt64: sql << "bigint"; break;
		case FieldType::Double: sql << "double"; break;
		case FieldType::Char:   sql << "char(" << f.arraySize << ")"; break;
		case FieldType::Bool:   sql << "bool"; break;
		}
	}
	if (schema->primaryKeyCount > 0)
	{
		sql << ", PRIMARY KEY(";
		for (int i = 0; i < schema->primaryKeyCount; ++i)
		{
			if (i > 0) sql << ", ";
			sql << schema->fields[schema->primaryKeyIndices[i]].name;
		}
		sql << ")";
	}
	sql << ");";
	Exec(sql.str().c_str());
}
void SqliteWrapper::DropTable(const char* tableName)
{
	std::string sql = "DROP TABLE IF EXISTS \"";
	sql += tableName;
	sql += "\";";
	Exec(sql.c_str());
}
void SqliteWrapper::TruncateTable(const char* tableName)
{
	std::string sql = "DELETE FROM \"";
	sql += tableName;
	sql += "\";";
	Exec(sql.c_str());
}

void SqliteWrapper::CreateTables(const TableSchema* const* schemas, int count)
{
	for (int i = 0; i < count; ++i)
	{
		CreateTable(schemas[i]);
	}
}
void SqliteWrapper::DropTables(const TableSchema* const* schemas, int count)
{
	for (int i = 0; i < count; ++i)
	{
		DropTable(schemas[i]->tableName);
	}
}
void SqliteWrapper::TruncateTables(const TableSchema* const* schemas, int count)
{
	for (int i = 0; i < count; ++i)
	{
		TruncateTable(schemas[i]->tableName);
	}
}

void SqliteWrapper::Insert(const TableSchema* schema, const void* record)
{
	std::ostringstream sql;
	sql << "INSERT INTO \"" << schema->tableName << "\" (";
	for (int i = 0; i < schema->fieldCount; ++i)
	{
		if (i > 0) sql << ", ";
		sql << schema->fields[i].name;
	}
	sql << ") VALUES (";
	for (int i = 0; i < schema->fieldCount; ++i)
	{
		if (i > 0) sql << ", ";
		sql << "?";
	}
	sql << ");";

	if (impl_->db == nullptr)
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "INSERT", schema->tableName, "database is not open");
		return;
	}
	StatementGuard stmt(impl_->db, sql.str().c_str());
	if (!stmt.IsValid())
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "INSERT", schema->tableName, sqlite3_errmsg(impl_->db));
		return;
	}
	int clampedCount = 0;
	BindFields(stmt.Get(), schema, record, clampedCount);
	const int rc = stmt.Step();
	if (rc != SQLITE_DONE)
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "INSERT", schema->tableName, sqlite3_errmsg(impl_->db));
	}
	LogClampedCells("INSERT", schema, clampedCount, schema->fieldCount);
}
void SqliteWrapper::BatchInsert(const TableSchema* schema, const void* const* records, int count)
{
	const int failureCountBeforeBatch = impl_->failureLogThrottle.FailureCount();
	Exec("BEGIN;");
	for (int i = 0; i < count; ++i)
	{
		Insert(schema, records[i]);
	}
	Exec("COMMIT;");
	const int failedRecordCount = impl_->failureLogThrottle.FailureCount() - failureCountBeforeBatch;
	if (failedRecordCount > 0)
	{
		// 明细由 Insert 的节流上报给出，此处补充整批的失败规模（每条记录一次失败会被节流掩盖总量）
		WriteLog(LogLevel::Error, "SqliteWrapper: BATCH INSERT incomplete. Table:%s, FailedRecords:%d/%d",
			schema->tableName, failedRecordCount, count);
	}
}
void SqliteWrapper::Update(const TableSchema* schema, const void* record)
{
	std::ostringstream sql;
	sql << "UPDATE \"" << schema->tableName << "\" SET ";
	for (int i = 0; i < schema->fieldCount; ++i)
	{
		if (i > 0) sql << ", ";
		sql << schema->fields[i].name << "=?";
	}
	sql << " WHERE ";
	for (int i = 0; i < schema->primaryKeyCount; ++i)
	{
		if (i > 0) sql << " AND ";
		int idx = schema->primaryKeyIndices[i];
		sql << schema->fields[idx].name << "=?";
	}
	sql << ";";

	if (impl_->db == nullptr)
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "UPDATE", schema->tableName, "database is not open");
		return;
	}
	StatementGuard stmt(impl_->db, sql.str().c_str());
	if (!stmt.IsValid())
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "UPDATE", schema->tableName,
			sqlite3_errmsg(impl_->db));
		return;
	}
	int clampedCount = 0;
	for (int i = 0; i < schema->fieldCount; ++i)
	{
		BindField(stmt.Get(), i + 1, schema->fields[i], record, clampedCount);
	}
	int paramIndex = schema->fieldCount + 1;
	for (int i = 0; i < schema->primaryKeyCount; ++i)
	{
		int idx = schema->primaryKeyIndices[i];
		BindField(stmt.Get(), paramIndex, schema->fields[idx], record, clampedCount);
		paramIndex++;
	}
	const int rc = stmt.Step();
	if (rc != SQLITE_DONE)
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "UPDATE", schema->tableName,
			sqlite3_errmsg(impl_->db));
	}
	LogClampedCells("UPDATE", schema, clampedCount, schema->fieldCount + schema->primaryKeyCount);
}
void SqliteWrapper::Delete(const TableSchema* schema, const void* record, const int* keyFieldIndices, int keyFieldCount)
{
	std::ostringstream sql;
	sql << "DELETE FROM \"" << schema->tableName << "\" WHERE ";
	for (int i = 0; i < keyFieldCount; ++i)
	{
		if (i > 0) sql << " AND ";
		sql << schema->fields[keyFieldIndices[i]].name << "=?";
	}
	sql << ";";

	if (impl_->db == nullptr)
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "DELETE", schema->tableName, "database is not open");
		return;
	}
	StatementGuard stmt(impl_->db, sql.str().c_str());
	if (!stmt.IsValid())
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "DELETE", schema->tableName,
			sqlite3_errmsg(impl_->db));
		return;
	}
	int clampedCount = 0;
	BindKeyFields(stmt.Get(), schema, record, keyFieldIndices, keyFieldCount, clampedCount);
	const int rc = stmt.Step();
	if (rc != SQLITE_DONE)
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "DELETE", schema->tableName,
			sqlite3_errmsg(impl_->db));
	}
	LogClampedCells("DELETE", schema, clampedCount, keyFieldCount);
}

void SqliteWrapper::SelectAll(const TableSchema* schema, void* recordsList, const RecordFactory& factory)
{
	std::string sql = "SELECT * FROM \"";
	sql += schema->tableName;
	sql += "\";";

	if (impl_->db == nullptr)
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "SELECT", schema->tableName, "database is not open");
		return;
	}
	StatementGuard stmt(impl_->db, sql.c_str());
	if (!stmt.IsValid())
	{
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "SELECT", schema->tableName,
			sqlite3_errmsg(impl_->db));
		return;
	}

	const int rc = ReadStatementRows(stmt.Get(), schema, recordsList, factory);
	if (rc != SQLITE_DONE)
	{
		// 中途出错时旧实现只是静默结束循环，调用方会把半份结果当成完整结果
		LogOperationFailure(impl_->failureLogThrottle, "SqliteWrapper", "SELECT", schema->tableName,
			sqlite3_errmsg(impl_->db));
	}
}
void SqliteWrapper::SelectWithSql(const char* sql, const TableSchema* schema, void* recordsList, const RecordFactory& factory)
{
	if (impl_->db == nullptr)
	{
		WriteLog(LogLevel::Error, "SqliteWrapper: SELECT skipped, database is not open. Sql:%s", sql);
		return;
	}
	StatementGuard stmt(impl_->db, sql);
	if (!stmt.IsValid())
	{
		WriteLog(LogLevel::Error, "SqliteWrapper: SELECT prepare failed. Error:%s, Sql:%s", sqlite3_errmsg(impl_->db), sql);
		return;
	}

	const int rc = ReadStatementRows(stmt.Get(), schema, recordsList, factory);
	if (rc != SQLITE_DONE)
	{
		WriteLog(LogLevel::Error, "SqliteWrapper: SELECT failed. ReturnCode:%d, Error:%s, Sql:%s",
			rc, sqlite3_errmsg(impl_->db), sql);
	}
}
}
