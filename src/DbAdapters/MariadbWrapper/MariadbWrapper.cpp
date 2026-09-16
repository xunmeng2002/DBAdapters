#include <DBAdapters/MariadbWrapper/MariadbWrapper.h>

#include <Spark/Core/Logger/Logger.h>

#include <mariadb/conncpp.hpp>

#include <cstring>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <vector>


namespace DbAdapters
{
    using Spark::Core::LogLevel;

    void BindField(sql::PreparedStatement* pstmt, int paramIndex, const FieldDescriptor& field, const void* record)
    {
        const char* data = static_cast<const char*>(record) + field.offset;
        switch (field.type)
        {
        case FieldType::Int8:
            pstmt->setByte(paramIndex, *reinterpret_cast<const int8_t*>(data));
            break;
        case FieldType::UInt8:
            // 连接器没有 setUByte：UInt8 的最大值 255 能无损放进 setShort 的 int16_t
            pstmt->setShort(paramIndex, *reinterpret_cast<const uint8_t*>(data));
            break;
        case FieldType::Int16:
            pstmt->setShort(paramIndex, *reinterpret_cast<const int16_t*>(data));
            break;
        case FieldType::UInt16:
            // 连接器没有 setUShort：UInt16 的最大值 65535 能无损放进 setInt 的 int32_t
            pstmt->setInt(paramIndex, *reinterpret_cast<const uint16_t*>(data));
            break;
        case FieldType::Int32:
            pstmt->setInt(paramIndex, *reinterpret_cast<const int32_t*>(data));
            break;
        case FieldType::UInt32:
            pstmt->setUInt(paramIndex, *reinterpret_cast<const uint32_t*>(data));
            break;
        case FieldType::Int64:
            pstmt->setInt64(paramIndex, *reinterpret_cast<const int64_t*>(data));
            break;
        case FieldType::UInt64:
            pstmt->setUInt64(paramIndex, *reinterpret_cast<const uint64_t*>(data));
            break;
        case FieldType::Double:
            pstmt->setDouble(paramIndex, *reinterpret_cast<const double*>(data));
            break;
        case FieldType::Char:
        {
            std::string str(data, field.arraySize);
            auto null_pos = str.find('\0');
            if (null_pos != std::string::npos)
                str = str.substr(0, null_pos);
            pstmt->setString(paramIndex, str);
            break;
        }
        case FieldType::Bool:
            pstmt->setBoolean(paramIndex, *reinterpret_cast<const bool*>(data));
            break;
        }
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

    void ReadRow(sql::ResultSet* result, const TableSchema* schema, void* record, int& clampedCount)
    {
        char* data = static_cast<char*>(record);
        for (int i = 0; i < schema->fieldCount; ++i)
        {
            const auto& field = schema->fields[i];
            char* dest = data + field.offset;
            int colIndex = i + 1;
            switch (field.type)
            {
            case FieldType::Int8:
            case FieldType::UInt8:
            case FieldType::Int16:
            case FieldType::UInt16:
            case FieldType::Int32:
            case FieldType::UInt32:
            case FieldType::Int64:
                if (!TryWriteIntegerFromSigned(result->getInt64(colIndex), field.type, dest))
                {
                    ++clampedCount;
                }
                break;
            case FieldType::UInt64:
                // UInt64 只能走无符号入口，同 BindField
                *reinterpret_cast<uint64_t*>(dest) = result->getUInt64(colIndex);
                break;
            case FieldType::Double:
                *reinterpret_cast<double*>(dest) = static_cast<double>(result->getDouble(colIndex));
                break;
            case FieldType::Char:
            {
                sql::SQLString val = result->getString(colIndex);
                std::size_t copy_len = val.length();
                if (copy_len > static_cast<std::size_t>(field.arraySize))
                    copy_len = static_cast<std::size_t>(field.arraySize);
                std::memcpy(dest, val.c_str(), copy_len);
                break;
            }
            case FieldType::Bool:
                *reinterpret_cast<bool*>(dest) = result->getBoolean(colIndex);
                break;
            }
        }
    }

    // 读取侧收窄饱和在整次 SELECT 结束后汇总上报一条 Warning：逐格上报会被行数淹没。
    void LogClampedCells(const TableSchema* schema, int clampedCount, int totalRowCount)
    {
        if (clampedCount > 0)
        {
            WriteLog(LogLevel::Warning, "MariadbWrapper: SELECT narrowed out-of-range values. Table:%s, ClampedCells:%d/%d",
                schema->tableName, clampedCount, totalRowCount * schema->fieldCount);
        }
    }

    // 把结果集里的全部行交给 factory。写入侧不会饱和：窄化交给列类型，越界由服务器报错抛出。
    void ReadResultRows(sql::ResultSet* result, const TableSchema* schema, void* recordsList, const RecordFactory& factory)
    {
        int clampedCount = 0;
        int totalRowCount = 0;
        while (result->next())
        {
            void* record = factory.Allocate();
            ReadRow(result, schema, record, clampedCount);
            factory.PushBack(recordsList, record);
            ++totalRowCount;
        }
        LogClampedCells(schema, clampedCount, totalRowCount);
    }

    struct SqlConnectionDeleter
    {
        void operator()(sql::Connection* conn) const
        {
            if (conn) { conn->close(); delete conn; }
        }
    };
    struct SqlStatementDeleter
    {
        void operator()(sql::Statement* stmt) const
        {
            if (stmt) { stmt->close(); delete stmt; }
        }
    };
    struct SqlPreparedStatementDeleter
    {
        void operator()(sql::PreparedStatement* pstmt) const
        {
            if (pstmt) { pstmt->close(); delete pstmt; }
        }
    };

struct MariadbWrapper::Impl
{
    Impl()
    {
        m_Driver = sql::mariadb::get_driver_instance();
    }

    void DisConnect()
    {
        m_Statement.reset();
        m_DBConnection.reset();
    }

    sql::Driver* m_Driver = nullptr;
    std::unique_ptr<sql::Connection, SqlConnectionDeleter> m_DBConnection;
    std::unique_ptr<sql::Statement, SqlStatementDeleter> m_Statement;
};


MariadbWrapper::MariadbWrapper(const std::string& host, const std::string& user, const std::string& passwd)
    : host_(host), user_(user), passwd_(passwd), impl_(nullptr)
{
    // 驱动装载失败（客户端动态库缺失）抛异常；记录后继续抛出，保持调用方原有的失败感知
    try
    {
        impl_ = std::make_unique<Impl>();
    }
    catch (const std::exception& e)
    {
        WriteLog(LogLevel::Error, "MariadbWrapper: Load driver failed. Message:%s", e.what());
        throw;
    }
}

MariadbWrapper::~MariadbWrapper()
{
    DisConnect();
}

bool MariadbWrapper::Connect()
{
#ifdef _WIN32
    _putenv_s("MARIADB_PLUGIN_DIR", MARIADB_PLUGIN_DIR);
#endif
    // connect 失败抛异常而非返回空；若放任其穿出 AsyncDBWriter::Run()，会穿过 ThreadBase::ThreadFunc（无 catch）
    // 直达 std::terminate，故在此转为返回 false，交由写库线程按连接失败重试并上报
    try
    {
        impl_->m_DBConnection.reset(impl_->m_Driver->connect(host_, user_, passwd_));
    }
    catch (const std::exception& e)
    {
        WriteLog(LogLevel::Error, "MariadbWrapper: Connect failed. Host:%s, Message:%s", host_.c_str(), e.what());
        return false;
    }
    return impl_->m_DBConnection != nullptr;
}

void MariadbWrapper::DisConnect()
{
    impl_->DisConnect();
}

void MariadbWrapper::Exec(const char* sql)
{
    if (!impl_->m_Statement)
    {
        impl_->m_Statement.reset(impl_->m_DBConnection->createStatement());
    }
    impl_->m_Statement->executeUpdate(sql);
}

void MariadbWrapper::CreateTable(const TableSchema* schema)
{
    Exec(MakeCreateTableSql(schema).c_str());
}

void MariadbWrapper::DropTable(const char* tableName)
{
    std::string sql = "DROP TABLE IF EXISTS `";
    sql += tableName;
    sql += "`;";
    Exec(sql.c_str());
}

void MariadbWrapper::TruncateTable(const char* tableName)
{
    std::string sql = "TRUNCATE TABLE `";
    sql += tableName;
    sql += "`;";
    Exec(sql.c_str());
}

void MariadbWrapper::CreateTables(const TableSchema* const* schemas, int count)
{
    for (int i = 0; i < count; ++i)
        CreateTable(schemas[i]);
}

void MariadbWrapper::DropTables(const TableSchema* const* schemas, int count)
{
    for (int i = 0; i < count; ++i)
        DropTable(schemas[i]->tableName);
}

void MariadbWrapper::TruncateTables(const TableSchema* const* schemas, int count)
{
    for (int i = 0; i < count; ++i)
        TruncateTable(schemas[i]->tableName);
}

void MariadbWrapper::Insert(const TableSchema* schema, const void* record)
{
    std::string sql = MakeInsertSql(schema);
    auto pstmt = std::unique_ptr<sql::PreparedStatement, SqlPreparedStatementDeleter>(impl_->m_DBConnection->prepareStatement(sql));
    for (int i = 0; i < schema->fieldCount; ++i)
        BindField(pstmt.get(), i + 1, schema->fields[i], record);
    pstmt->executeUpdate();
}

void MariadbWrapper::BatchInsert(const TableSchema* schema, const void* const* records, int count)
{
    Exec("START TRANSACTION;");
    for (int i = 0; i < count; ++i)
        Insert(schema, records[i]);
    Exec("COMMIT;");
}

void MariadbWrapper::Update(const TableSchema* schema, const void* record)
{
    std::string sql = MakeUpdateSql(schema);
    auto pstmt = std::unique_ptr<sql::PreparedStatement, SqlPreparedStatementDeleter>(impl_->m_DBConnection->prepareStatement(sql));
    int paramIndex = 1;
    for (int i = 0; i < schema->fieldCount; ++i)
        BindField(pstmt.get(), paramIndex++, schema->fields[i], record);
    for (int i = 0; i < schema->primaryKeyCount; ++i)
    {
        int idx = schema->primaryKeyIndices[i];
        BindField(pstmt.get(), paramIndex++, schema->fields[idx], record);
    }
    pstmt->executeUpdate();
}

void MariadbWrapper::Delete(const TableSchema* schema, const void* record, const int* keyFieldIndices, int keyFieldCount)
{
    std::string sql = MakeDeleteSql(schema, keyFieldIndices, keyFieldCount);
    auto pstmt = std::unique_ptr<sql::PreparedStatement, SqlPreparedStatementDeleter>(impl_->m_DBConnection->prepareStatement(sql));
    for (int i = 0; i < keyFieldCount; ++i)
        BindField(pstmt.get(), i + 1, schema->fields[keyFieldIndices[i]], record);
    pstmt->executeUpdate();
}
void MariadbWrapper::SelectAll(const TableSchema* schema, void* recordsList, const RecordFactory& factory)
{
    std::string sql = "SELECT * FROM `";
    sql += schema->tableName;
    sql += "`;";

    if (!impl_->m_Statement)
    {
        impl_->m_Statement.reset(impl_->m_DBConnection->createStatement());
    }
    auto result = std::unique_ptr<sql::ResultSet>(impl_->m_Statement->executeQuery(sql));
    ReadResultRows(result.get(), schema, recordsList, factory);
}
void MariadbWrapper::SelectWithSql(const char* sql, const TableSchema* schema, void* recordsList, const RecordFactory& factory)
{
    if (!impl_->m_Statement)
    {
        impl_->m_Statement.reset(impl_->m_DBConnection->createStatement());
    }
    auto result = std::unique_ptr<sql::ResultSet>(impl_->m_Statement->executeQuery(sql));
    ReadResultRows(result.get(), schema, recordsList, factory);
}
}
