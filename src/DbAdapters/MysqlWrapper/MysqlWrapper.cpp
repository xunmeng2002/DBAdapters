#include <DbAdapters/MysqlWrapper/MysqlWrapper.h>

#include <DbAdapters/DbInterface/DbBackendFactory.h>

#include <Spark/Core/Logger/Logger.h>

#include <mysqlx/xdevapi.h>

#include <cstring>
#include <memory>
#include <sstream>
#include <vector>


namespace DbAdapters
{
    using Spark::Core::LogLevel;

    mysqlx::Value FieldToValue(const FieldDescriptor& field, const void* record)
    {
        const char* data = static_cast<const char*>(record) + field.offset;
        switch (field.type)
        {
        case FieldType::Int8:
            return mysqlx::Value(static_cast<int64_t>(*reinterpret_cast<const int8_t*>(data)));
        case FieldType::UInt8:
            return mysqlx::Value(static_cast<int64_t>(*reinterpret_cast<const uint8_t*>(data)));
        case FieldType::Int16:
            return mysqlx::Value(static_cast<int64_t>(*reinterpret_cast<const int16_t*>(data)));
        case FieldType::UInt16:
            return mysqlx::Value(static_cast<int64_t>(*reinterpret_cast<const uint16_t*>(data)));
        case FieldType::Int32:
            return mysqlx::Value(static_cast<int64_t>(*reinterpret_cast<const int32_t*>(data)));
        case FieldType::UInt32:
            return mysqlx::Value(static_cast<int64_t>(*reinterpret_cast<const uint32_t*>(data)));
        case FieldType::Int64:
            return mysqlx::Value(static_cast<int64_t>(*reinterpret_cast<const int64_t*>(data)));
        case FieldType::UInt64:
            // 只有 UInt64 需要无符号入口：≥2^63 的值经 int64_t 会变号
            return mysqlx::Value(*reinterpret_cast<const uint64_t*>(data));
        case FieldType::Double:
            return mysqlx::Value(*reinterpret_cast<const double*>(data));
        case FieldType::Char:
        {
            std::string str(data, field.arraySize);
            auto null_pos = str.find('\0');
            if (null_pos != std::string::npos)
                str = str.substr(0, null_pos);
            return mysqlx::Value(str);
        }
        case FieldType::Bool:
            return mysqlx::Value(*reinterpret_cast<const bool*>(data));
        }
        return mysqlx::Value();
    }

    std::string MakeCreateTableSql(const TableSchema* schema)
    {
        std::ostringstream sql;
        sql << "CREATE TABLE IF NOT EXISTS `" << schema->tableName << "`(";
        for (int i = 0; i < schema->fieldCount; ++i)
        {
            if (i > 0) sql << ", ";
            const auto& f = schema->fields[i];
            sql << "`" << f.name << "` ";
            switch (f.type)
            {
            case FieldType::Int8:   sql << "tinyint"; break;
            case FieldType::UInt8:  sql << "tinyint unsigned"; break;
            case FieldType::Int16:  sql << "smallint"; break;
            case FieldType::UInt16: sql << "smallint unsigned"; break;
            case FieldType::Int32:  sql << "int"; break;
            case FieldType::UInt32: sql << "int unsigned"; break;
            case FieldType::Int64:  sql << "bigint"; break;
            case FieldType::UInt64: sql << "bigint unsigned"; break;
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
                sql << "`" << schema->fields[schema->primaryKeyIndices[i]].name << "`";
            }
            sql << ")";
        }
        sql << ") ENGINE=MyISAM DEFAULT COLLATE='utf8mb4_bin';";
        return sql.str();
    }

    std::string MakeInsertSql(const TableSchema* schema)
    {
        std::ostringstream sql;
        sql << "INSERT INTO `" << schema->tableName << "` (";
        for (int i = 0; i < schema->fieldCount; ++i)
        {
            if (i > 0) sql << ", ";
            sql << "`" << schema->fields[i].name << "`";
        }
        sql << ") VALUES (";
        for (int i = 0; i < schema->fieldCount; ++i)
        {
            if (i > 0) sql << ", ";
            sql << "?";
        }
        sql << ");";
        return sql.str();
    }

    std::string MakeUpdateSql(const TableSchema* schema)
    {
        std::ostringstream sql;
        sql << "UPDATE `" << schema->tableName << "` SET ";
        for (int i = 0; i < schema->fieldCount; ++i)
        {
            if (i > 0) sql << ", ";
            sql << "`" << schema->fields[i].name << "`=?";
        }
        sql << " WHERE ";
        for (int i = 0; i < schema->primaryKeyCount; ++i)
        {
            if (i > 0) sql << " AND ";
            int idx = schema->primaryKeyIndices[i];
            sql << "`" << schema->fields[idx].name << "`=?";
        }
        sql << ";";
        return sql.str();
    }

    std::string MakeDeleteSql(const TableSchema* schema, const int* keyFieldIndices, int keyFieldCount)
    {
        std::ostringstream sql;
        sql << "DELETE FROM `" << schema->tableName << "` WHERE ";
        for (int i = 0; i < keyFieldCount; ++i)
        {
            if (i > 0) sql << " AND ";
            sql << "`" << schema->fields[keyFieldIndices[i]].name << "`=?";
        }
        sql << ";";
        return sql.str();
    }

    void ReadRow(const mysqlx::Row& row, const TableSchema* schema, void* record, int& clampedCount)
    {
        char* data = static_cast<char*>(record);
        for (int i = 0; i < schema->fieldCount; ++i)
        {
            const auto& field = schema->fields[i];
            char* dest = data + field.offset;
            int colIndex = i;
            switch (field.type)
            {
            case FieldType::Int8:
            case FieldType::UInt8:
            case FieldType::Int16:
            case FieldType::UInt16:
            case FieldType::Int32:
            case FieldType::UInt32:
            case FieldType::Int64:
                if (!TryWriteIntegerFromSigned(row[colIndex].get<int64_t>(), field.type, dest))
                {
                    ++clampedCount;
                }
                break;
            case FieldType::UInt64:
                // UInt64 只能走无符号入口，同 FieldToValue
                *reinterpret_cast<uint64_t*>(dest) = row[colIndex].get<uint64_t>();
                break;
            case FieldType::Double:
                *reinterpret_cast<double*>(dest) = row[colIndex].get<double>();
                break;
            case FieldType::Char:
            {
                std::string val = row[colIndex].get<std::string>();
                std::size_t copy_len = val.size();
                if (copy_len > static_cast<std::size_t>(field.arraySize))
                    copy_len = static_cast<std::size_t>(field.arraySize);
                std::memcpy(dest, val.c_str(), copy_len);
                break;
            }
            case FieldType::Bool:
                *reinterpret_cast<bool*>(dest) = row[colIndex].get<bool>();
                break;
            }
        }
    }

    // 读取侧收窄饱和在整次 SELECT 结束后汇总上报一条 Warning：逐格上报会被行数淹没。
    void LogClampedCells(const TableSchema* schema, int clampedCount, int totalRowCount)
    {
        if (clampedCount > 0)
        {
            WriteLog(LogLevel::Warning,
                "MysqlWrapper: SELECT narrowed out-of-range values. Table:%s, ClampedCells:%d/%d",
                schema->tableName, clampedCount, totalRowCount * schema->fieldCount);
        }
    }

    // 把结果集里的全部行交给 factory。写入侧不会饱和：窄化交给列类型，越界由服务器报错抛出。
    void ReadResultRows(mysqlx::RowResult& result, const TableSchema* schema, void* recordsList, const RecordFactory& factory)
    {
        int clampedCount = 0;
        int totalRowCount = 0;
        while (auto row = result.fetchOne())
        {
            void* record = factory.Allocate();
            ReadRow(row, schema, record, clampedCount);
            factory.PushBack(recordsList, record);
            ++totalRowCount;
        }
        LogClampedCells(schema, clampedCount, totalRowCount);
    }

struct MysqlWrapper::Impl
{
    explicit Impl(mysqlx::Session&& session)
        : session(std::move(session))
    {
    }

    mysqlx::Session session;
};

MysqlWrapper::MysqlWrapper(const std::string& host)
    : impl_(nullptr), host_(host)
{
    // Session 构造即建连，失败抛异常；记录后继续抛出，保持调用方原有的失败感知（本 wrapper 的 Connect 不建连）
    try
    {
        impl_ = std::make_unique<Impl>(mysqlx::Session(host));
    }
    catch (const std::exception& e)
    {
        WriteLog(LogLevel::Error, "MysqlWrapper: Connect failed. Host:%s, Message:%s", host.c_str(), e.what());
        throw;
    }
}
MysqlWrapper::~MysqlWrapper()
{
    DisConnect();
}

bool MysqlWrapper::Connect()
{
    return impl_ != nullptr;
}
void MysqlWrapper::DisConnect()
{
    if (impl_)
    {
        impl_->session.close();
    }
}
void MysqlWrapper::Exec(const char* sql)
{
    impl_->session.sql(sql).execute();
}

void MysqlWrapper::CreateTable(const TableSchema* schema)
{
    Exec(MakeCreateTableSql(schema).c_str());
}
void MysqlWrapper::DropTable(const char* tableName)
{
    std::string sql = "DROP TABLE IF EXISTS `";
    sql += tableName;
    sql += "`;";
    Exec(sql.c_str());
}
void MysqlWrapper::TruncateTable(const char* tableName)
{
    std::string sql = "TRUNCATE TABLE `";
    sql += tableName;
    sql += "`;";
    Exec(sql.c_str());
}

void MysqlWrapper::CreateTables(const TableSchema* const* schemas, int count)
{
    for (int i = 0; i < count; ++i)
        CreateTable(schemas[i]);
}
void MysqlWrapper::DropTables(const TableSchema* const* schemas, int count)
{
    for (int i = 0; i < count; ++i)
        DropTable(schemas[i]->tableName);
}
void MysqlWrapper::TruncateTables(const TableSchema* const* schemas, int count)
{
    for (int i = 0; i < count; ++i)
        TruncateTable(schemas[i]->tableName);
}

void MysqlWrapper::Insert(const TableSchema* schema, const void* record)
{
    std::string sql = MakeInsertSql(schema);
    std::vector<mysqlx::Value> params;
    params.reserve(schema->fieldCount);
    for (int i = 0; i < schema->fieldCount; ++i)
        params.push_back(FieldToValue(schema->fields[i], record));
    impl_->session.sql(sql).bind(params).execute();
}
void MysqlWrapper::BatchInsert(const TableSchema* schema, const void* const* records, int count)
{
    Exec("START TRANSACTION;");
    for (int i = 0; i < count; ++i)
        Insert(schema, records[i]);
    Exec("COMMIT;");
}
void MysqlWrapper::Update(const TableSchema* schema, const void* record)
{
    std::string sql = MakeUpdateSql(schema);
    std::vector<mysqlx::Value> params;
    params.reserve(schema->fieldCount + schema->primaryKeyCount);
    for (int i = 0; i < schema->fieldCount; ++i)
        params.push_back(FieldToValue(schema->fields[i], record));
    for (int i = 0; i < schema->primaryKeyCount; ++i)
    {
        int idx = schema->primaryKeyIndices[i];
        params.push_back(FieldToValue(schema->fields[idx], record));
    }
    impl_->session.sql(sql).bind(params).execute();
}
void MysqlWrapper::Delete(const TableSchema* schema, const void* record, const int* keyFieldIndices, int keyFieldCount)
{
    std::string sql = MakeDeleteSql(schema, keyFieldIndices, keyFieldCount);
    std::vector<mysqlx::Value> params;
    params.reserve(keyFieldCount);
    for (int i = 0; i < keyFieldCount; ++i)
        params.push_back(FieldToValue(schema->fields[keyFieldIndices[i]], record));
    impl_->session.sql(sql).bind(params).execute();
}
void MysqlWrapper::SelectAll(const TableSchema* schema, void* recordsList, const RecordFactory& factory)
{
    std::string sql = "SELECT * FROM `";
    sql += schema->tableName;
    sql += "`;";

    auto result = impl_->session.sql(sql).execute();
    ReadResultRows(result, schema, recordsList, factory);
}
void MysqlWrapper::SelectWithSql(const char* sql, const TableSchema* schema, void* recordsList, const RecordFactory& factory)
{
    auto result = impl_->session.sql(sql).execute();
    ReadResultRows(result, schema, recordsList, factory);
}
}

// 契约见 DbAdapters/DbInterface/DbBackendFactory.h. 必须留在 namespace 之外, 且异常不得逃出去:
// 让 C++ 异常穿过 extern "C" 边界是未定义行为 (本后端构造函数的连接失败就是 throw 的).
extern "C" MYSQLWRAPPER_EXPORTS DbAdapters::Db* DbAdapters_CreateBackend(const char* connectionTarget,
    const char* /*userName*/, const char* /*password*/, char* failureText, int failureTextCapacity)
{
    return DbAdapters::CreateBackendOrReportFailure(failureText, failureTextCapacity,
        [connectionTarget]
        {
            return new DbAdapters::MysqlWrapper(DbAdapters::ConnectionTargetOrEmpty(connectionTarget));
        });
}
