#include <DBAdapters/DuckdbWrapper/DuckdbWrapper.h>
#include <duckdb.h>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>


namespace dbadapters
{

    class PreparedStatement
    {
    public:
        PreparedStatement(duckdb_connection connection, const char* sql)
        {
            if (duckdb_prepare(connection, sql, &stmt_) == DuckDBSuccess && stmt_)
            {
                valid_ = true;
            }
        }
        ~PreparedStatement()
        {
            if (stmt_)
            {
                duckdb_destroy_prepare(&stmt_);
            }
        }

        PreparedStatement(const PreparedStatement&) = delete;
        PreparedStatement& operator=(const PreparedStatement&) = delete;

        bool IsValid() const { return valid_; }
        duckdb_prepared_statement Get() const { return stmt_; }

    private:
        duckdb_prepared_statement stmt_ = nullptr;
        bool valid_ = false;
    };

    class StringGuard
    {
    public:
        explicit StringGuard(duckdb_string str) : str_(str) {}
        ~StringGuard()
        {
            if (str_.data)
            {
                duckdb_free(str_.data);
            }
        }
        StringGuard(const StringGuard&) = delete;
        StringGuard& operator=(const StringGuard&) = delete;

        const char* Data() const { return str_.data; }
        idx_t Size() const { return str_.size; }

    private:
        duckdb_string str_;
    };

    void BindField(duckdb_prepared_statement stmt, int index,
                   const FieldDescriptor& field, const void* record)
    {
        const char* data = static_cast<const char*>(record) + field.offset;
        switch (field.type)
        {
        case FieldType::Int:
            duckdb_bind_int32(stmt, index, *reinterpret_cast<const int*>(data));
            break;
        case FieldType::Int64:
            duckdb_bind_int64(stmt, index, *reinterpret_cast<const long long*>(data));
            break;
        case FieldType::Double:
            duckdb_bind_double(stmt, index, *reinterpret_cast<const double*>(data));
            break;
        case FieldType::Char:
            duckdb_bind_varchar_length(stmt, index, data, field.arraySize);
            break;
        case FieldType::Bool:
            duckdb_bind_boolean(stmt, index, *reinterpret_cast<const bool*>(data) ? 1 : 0);
            break;
        }
    }

    void BindAllFields(duckdb_prepared_statement stmt, const TableSchema* schema,
                       const void* record)
    {
        for (int i = 0; i < schema->fieldCount; ++i)
        {
            BindField(stmt, i + 1, schema->fields[i], record);
        }
    }

    void BindKeyFields(duckdb_prepared_statement stmt, const TableSchema* schema,
                       const void* record, const int* keyIndices, int keyCount)
    {
        for (int i = 0; i < keyCount; ++i)
        {
            BindField(stmt, i + 1, schema->fields[keyIndices[i]], record);
        }
    }

    void ReadRow(duckdb_result& result, idx_t row, const TableSchema* schema, void* record)
    {
        char* data = static_cast<char*>(record);
        for (int i = 0; i < schema->fieldCount; ++i)
        {
            const auto& field = schema->fields[i];
            char* dest = data + field.offset;
            switch (field.type)
            {
            case FieldType::Int:
                *reinterpret_cast<int*>(dest) = duckdb_value_int32(&result, i, row);
                break;
            case FieldType::Int64:
                *reinterpret_cast<long long*>(dest) = duckdb_value_int64(&result, i, row);
                break;
            case FieldType::Double:
                *reinterpret_cast<double*>(dest) = duckdb_value_double(&result, i, row);
                break;
            case FieldType::Char:
            {
                StringGuard str(duckdb_value_string(&result, i, row));
                if (str.Data())
                {
                    std::size_t copy_len = static_cast<std::size_t>(field.arraySize);
                    if (str.Size() < copy_len)
                        copy_len = str.Size();
                    std::memcpy(dest, str.Data(), copy_len);
                }
                break;
            }
            case FieldType::Bool:
                *reinterpret_cast<bool*>(dest) = duckdb_value_boolean(&result, i, row) != 0;
                break;
            }
        }
    }

    void WriteNullSentinel(char* dest, FieldType type)
    {
        switch (type)
        {
        case FieldType::Double:
            *reinterpret_cast<double*>(dest) = std::numeric_limits<double>::infinity();
            break;
        case FieldType::Int64:
            *reinterpret_cast<long long*>(dest) = 0;
            break;
        case FieldType::Int:
            *reinterpret_cast<int*>(dest) = 0;
            break;
        case FieldType::Bool:
            *reinterpret_cast<bool*>(dest) = false;
            break;
        case FieldType::Char:
            *dest = '\0';
            break;
        }
    }

    // 从 chunk 向量按源类型取 double；DECIMAL 用向量自带的宽度/精度精确转换。
    // 注意：duckdb_value_* 访问器在流式（duckdb_fetch_chunk）结果上不可用，
    // 因此类型转换一律基于 chunk 向量原始数据，绝不回退到结果级访问器。
    double ReadCellAsDouble(duckdb_type sourceType, void* rawData, idx_t row,
                            uint8_t decimalWidth, uint8_t decimalScale)
    {
        switch (sourceType)
        {
        case DUCKDB_TYPE_DOUBLE:
            return static_cast<double*>(rawData)[row];
        case DUCKDB_TYPE_FLOAT:
            return static_cast<float*>(rawData)[row];
        case DUCKDB_TYPE_DECIMAL:
        {
            duckdb_hugeint value = static_cast<duckdb_hugeint*>(rawData)[row];
            return duckdb_decimal_to_double(duckdb_decimal{ decimalWidth, decimalScale, value });
        }
        case DUCKDB_TYPE_BIGINT:
            return static_cast<double>(static_cast<long long*>(rawData)[row]);
        case DUCKDB_TYPE_INTEGER:
            return static_cast<double>(static_cast<int*>(rawData)[row]);
        case DUCKDB_TYPE_SMALLINT:
            return static_cast<double>(static_cast<short*>(rawData)[row]);
        case DUCKDB_TYPE_TINYINT:
            return static_cast<double>(static_cast<signed char*>(rawData)[row]);
        case DUCKDB_TYPE_UBIGINT:
            return static_cast<double>(static_cast<unsigned long long*>(rawData)[row]);
        case DUCKDB_TYPE_UINTEGER:
            return static_cast<double>(static_cast<unsigned int*>(rawData)[row]);
        case DUCKDB_TYPE_HUGEINT:
            return duckdb_hugeint_to_double(static_cast<duckdb_hugeint*>(rawData)[row]);
        case DUCKDB_TYPE_TIMESTAMP:
            return static_cast<double>(static_cast<duckdb_timestamp*>(rawData)[row].micros);
        case DUCKDB_TYPE_DATE:
            return static_cast<double>(static_cast<duckdb_date*>(rawData)[row].days);
        case DUCKDB_TYPE_BOOLEAN:
            return static_cast<bool*>(rawData)[row] ? 1.0 : 0.0;
        default:
            return 0.0;
        }
    }

    // 从 chunk 向量按源类型取 int64；DOUBLE/FLOAT/DECIMAL 截断为整数。
    long long ReadCellAsInt64(duckdb_type sourceType, void* rawData, idx_t row,
                              uint8_t decimalWidth, uint8_t decimalScale)
    {
        switch (sourceType)
        {
        case DUCKDB_TYPE_BIGINT:
            return static_cast<long long*>(rawData)[row];
        case DUCKDB_TYPE_INTEGER:
            return static_cast<int*>(rawData)[row];
        case DUCKDB_TYPE_SMALLINT:
            return static_cast<short*>(rawData)[row];
        case DUCKDB_TYPE_TINYINT:
            return static_cast<signed char*>(rawData)[row];
        case DUCKDB_TYPE_UBIGINT:
            return static_cast<long long>(static_cast<unsigned long long*>(rawData)[row]);
        case DUCKDB_TYPE_UINTEGER:
            return static_cast<unsigned int*>(rawData)[row];
        case DUCKDB_TYPE_USMALLINT:
            return static_cast<unsigned short*>(rawData)[row];
        case DUCKDB_TYPE_UTINYINT:
            return static_cast<unsigned char*>(rawData)[row];
        case DUCKDB_TYPE_DOUBLE:
            return static_cast<long long>(static_cast<double*>(rawData)[row]);
        case DUCKDB_TYPE_FLOAT:
            return static_cast<long long>(static_cast<float*>(rawData)[row]);
        case DUCKDB_TYPE_DECIMAL:
        {
            duckdb_hugeint value = static_cast<duckdb_hugeint*>(rawData)[row];
            return static_cast<long long>(
                duckdb_decimal_to_double(duckdb_decimal{ decimalWidth, decimalScale, value }));
        }
        case DUCKDB_TYPE_HUGEINT:
            return static_cast<long long>(
                duckdb_hugeint_to_double(static_cast<duckdb_hugeint*>(rawData)[row]));
        case DUCKDB_TYPE_TIMESTAMP:
            return static_cast<duckdb_timestamp*>(rawData)[row].micros;
        case DUCKDB_TYPE_BOOLEAN:
            return static_cast<bool*>(rawData)[row] ? 1 : 0;
        default:
            return 0;
        }
    }

    // 从 chunk 向量按源类型取 int32；DOUBLE/FLOAT/DECIMAL 截断为整数。
    int ReadCellAsInt32(duckdb_type sourceType, void* rawData, idx_t row,
                        uint8_t decimalWidth, uint8_t decimalScale)
    {
        switch (sourceType)
        {
        case DUCKDB_TYPE_INTEGER:
            return static_cast<int*>(rawData)[row];
        case DUCKDB_TYPE_SMALLINT:
            return static_cast<short*>(rawData)[row];
        case DUCKDB_TYPE_TINYINT:
            return static_cast<signed char*>(rawData)[row];
        case DUCKDB_TYPE_BIGINT:
            return static_cast<int>(static_cast<long long*>(rawData)[row]);
        case DUCKDB_TYPE_DOUBLE:
            return static_cast<int>(static_cast<double*>(rawData)[row]);
        case DUCKDB_TYPE_FLOAT:
            return static_cast<int>(static_cast<float*>(rawData)[row]);
        case DUCKDB_TYPE_DECIMAL:
        {
            duckdb_hugeint value = static_cast<duckdb_hugeint*>(rawData)[row];
            return static_cast<int>(
                duckdb_decimal_to_double(duckdb_decimal{ decimalWidth, decimalScale, value }));
        }
        case DUCKDB_TYPE_BOOLEAN:
            return static_cast<bool*>(rawData)[row] ? 1 : 0;
        default:
            return 0;
        }
    }

    // 从 chunk 向量按源类型取 bool；数值类型按非零判定。
    bool ReadCellAsBool(duckdb_type sourceType, void* rawData, idx_t row,
                        uint8_t decimalWidth, uint8_t decimalScale)
    {
        switch (sourceType)
        {
        case DUCKDB_TYPE_BOOLEAN:
            return static_cast<bool*>(rawData)[row];
        case DUCKDB_TYPE_TINYINT:
            return static_cast<signed char*>(rawData)[row] != 0;
        case DUCKDB_TYPE_SMALLINT:
            return static_cast<short*>(rawData)[row] != 0;
        case DUCKDB_TYPE_INTEGER:
            return static_cast<int*>(rawData)[row] != 0;
        case DUCKDB_TYPE_BIGINT:
            return static_cast<long long*>(rawData)[row] != 0;
        case DUCKDB_TYPE_DOUBLE:
            return static_cast<double*>(rawData)[row] != 0.0;
        case DUCKDB_TYPE_DECIMAL:
        {
            duckdb_hugeint value = static_cast<duckdb_hugeint*>(rawData)[row];
            return duckdb_decimal_to_double(duckdb_decimal{ decimalWidth, decimalScale, value }) != 0.0;
        }
        default:
            return false;
        }
    }

    // 把非 VARCHAR 源的 chunk 向量单元格渲染为字符串；整数/时间戳直接 to_string，
    // DOUBLE/FLOAT 用 std::to_string（6 位小数），DECIMAL 先转 double。
    std::string RenderCellAsString(duckdb_type sourceType, void* rawData, idx_t row,
                                   uint8_t decimalWidth, uint8_t decimalScale)
    {
        switch (sourceType)
        {
        case DUCKDB_TYPE_BIGINT:
            return std::to_string(static_cast<long long*>(rawData)[row]);
        case DUCKDB_TYPE_INTEGER:
            return std::to_string(static_cast<int*>(rawData)[row]);
        case DUCKDB_TYPE_SMALLINT:
            return std::to_string(static_cast<short*>(rawData)[row]);
        case DUCKDB_TYPE_TINYINT:
            return std::to_string(static_cast<signed char*>(rawData)[row]);
        case DUCKDB_TYPE_UBIGINT:
            return std::to_string(static_cast<unsigned long long*>(rawData)[row]);
        case DUCKDB_TYPE_UINTEGER:
            return std::to_string(static_cast<unsigned int*>(rawData)[row]);
        case DUCKDB_TYPE_USMALLINT:
            return std::to_string(static_cast<unsigned short*>(rawData)[row]);
        case DUCKDB_TYPE_UTINYINT:
            return std::to_string(static_cast<unsigned char*>(rawData)[row]);
        case DUCKDB_TYPE_DOUBLE:
            return std::to_string(static_cast<double*>(rawData)[row]);
        case DUCKDB_TYPE_FLOAT:
            return std::to_string(static_cast<float*>(rawData)[row]);
        case DUCKDB_TYPE_DECIMAL:
        {
            duckdb_hugeint value = static_cast<duckdb_hugeint*>(rawData)[row];
            return std::to_string(
                duckdb_decimal_to_double(duckdb_decimal{ decimalWidth, decimalScale, value }));
        }
        case DUCKDB_TYPE_BOOLEAN:
            return static_cast<bool*>(rawData)[row] ? "true" : "false";
        case DUCKDB_TYPE_TIMESTAMP:
            return std::to_string(static_cast<duckdb_timestamp*>(rawData)[row].micros);
        case DUCKDB_TYPE_DATE:
            return std::to_string(static_cast<duckdb_date*>(rawData)[row].days);
        case DUCKDB_TYPE_TIME:
            return std::to_string(static_cast<duckdb_time*>(rawData)[row].micros);
        default:
            return std::string();
        }
    }

    void ReadCellAsChar(char* dest, std::size_t capacity, duckdb_type sourceType,
                        void* rawData, idx_t row, uint8_t decimalWidth, uint8_t decimalScale)
    {
        if (sourceType == DUCKDB_TYPE_VARCHAR)
        {
            duckdb_string_t source = static_cast<duckdb_string_t*>(rawData)[row];
            std::size_t length = duckdb_string_t_length(source);
            if (length >= capacity)
            {
                length = capacity - 1;
            }
            std::memcpy(dest, duckdb_string_t_data(&source), length);
            dest[length] = '\0';
            return;
        }
        std::string value = RenderCellAsString(sourceType, rawData, row, decimalWidth, decimalScale);
        std::size_t length = value.size();
        if (length >= capacity)
        {
            length = capacity - 1;
        }
        std::memcpy(dest, value.data(), length);
        dest[length] = '\0';
    }

    // 按 schema 列主序绑定一个 chunk 的所有行到已分配的记录。
    void BindChunkToRecords(duckdb_data_chunk chunk, const TableSchema* schema, void** records)
    {
        idx_t rowCount = duckdb_data_chunk_get_size(chunk);
        for (int fieldIndex = 0; fieldIndex < schema->fieldCount; ++fieldIndex)
        {
            const auto& field = schema->fields[fieldIndex];
            duckdb_vector vector = duckdb_data_chunk_get_vector(chunk, fieldIndex);
            void* rawData = duckdb_vector_get_data(vector);
            uint64_t* validity = duckdb_vector_get_validity(vector);
            duckdb_logical_type logicalType = duckdb_vector_get_column_type(vector);
            duckdb_type sourceType = duckdb_get_type_id(logicalType);

            uint8_t decimalWidth = 0;
            uint8_t decimalScale = 0;
            if (sourceType == DUCKDB_TYPE_DECIMAL)
            {
                decimalWidth = duckdb_decimal_width(logicalType);
                decimalScale = duckdb_decimal_scale(logicalType);
            }

            for (idx_t row = 0; row < rowCount; ++row)
            {
                char* dest = static_cast<char*>(records[row]) + field.offset;
                bool isNull = validity != nullptr && !duckdb_validity_row_is_valid(validity, row);
                if (isNull)
                {
                    WriteNullSentinel(dest, field.type);
                    continue;
                }
                switch (field.type)
                {
                case FieldType::Double:
                    *reinterpret_cast<double*>(dest) =
                        ReadCellAsDouble(sourceType, rawData, row, decimalWidth, decimalScale);
                    break;
                case FieldType::Int64:
                    *reinterpret_cast<long long*>(dest) =
                        ReadCellAsInt64(sourceType, rawData, row, decimalWidth, decimalScale);
                    break;
                case FieldType::Int:
                    *reinterpret_cast<int*>(dest) =
                        ReadCellAsInt32(sourceType, rawData, row, decimalWidth, decimalScale);
                    break;
                case FieldType::Bool:
                    *reinterpret_cast<bool*>(dest) =
                        ReadCellAsBool(sourceType, rawData, row, decimalWidth, decimalScale);
                    break;
                case FieldType::Char:
                    ReadCellAsChar(dest, field.arraySize, sourceType, rawData, row,
                                   decimalWidth, decimalScale);
                    break;
                }
            }
            duckdb_destroy_logical_type(&logicalType);
        }
    }

    std::string MakeCreateTableSql(const TableSchema* schema)
    {
        std::ostringstream sql;
        sql << "CREATE TABLE IF NOT EXISTS " << schema->tableName << "(";
        for (int i = 0; i < schema->fieldCount; ++i)
        {
            if (i > 0) sql << ", ";
            const auto& f = schema->fields[i];
            sql << f.name << " ";
            switch (f.type)
            {
            case FieldType::Int:    sql << "INTEGER"; break;
            case FieldType::Int64:  sql << "BIGINT"; break;
            case FieldType::Double: sql << "DOUBLE"; break;
            case FieldType::Char:   sql << "VARCHAR"; break;
            case FieldType::Bool:   sql << "BOOLEAN"; break;
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
        return sql.str();
    }

    std::string MakeInsertSql(const TableSchema* schema)
    {
        std::ostringstream sql;
        sql << "INSERT INTO " << schema->tableName << " (";
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
        return sql.str();
    }

    std::string MakeUpdateSql(const TableSchema* schema)
    {
        std::ostringstream sql;
        sql << "UPDATE " << schema->tableName << " SET ";
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
        return sql.str();
    }

    std::string MakeDeleteSql(const TableSchema* schema,
                              const int* keyFieldIndices, int keyFieldCount)
    {
        std::ostringstream sql;
        sql << "DELETE FROM " << schema->tableName << " WHERE ";
        for (int i = 0; i < keyFieldCount; ++i)
        {
            if (i > 0) sql << " AND ";
            sql << schema->fields[keyFieldIndices[i]].name << "=?";
        }
        sql << ";";
        return sql.str();
    }

struct DuckdbWrapper::Impl
{
    duckdb_database database = nullptr;
    duckdb_connection connection = nullptr;
};

DuckdbWrapper::DuckdbWrapper(const std::string& dbName)
    : m_Impl(new Impl)
{
    if (duckdb_open(dbName.c_str(), &m_Impl->database) != DuckDBSuccess)
    {
        m_Impl->database = nullptr;
        return;
    }
    duckdb_connect(m_Impl->database, &m_Impl->connection);
}
DuckdbWrapper::~DuckdbWrapper()
{
    DisConnect();
    delete m_Impl;
}

bool DuckdbWrapper::Connect()
{
    return m_Impl->connection != nullptr;
}
void DuckdbWrapper::DisConnect()
{
    if (m_Impl->connection)
    {
        duckdb_disconnect(&m_Impl->connection);
        m_Impl->connection = nullptr;
    }
    if (m_Impl->database)
    {
        duckdb_close(&m_Impl->database);
        m_Impl->database = nullptr;
    }
}
void DuckdbWrapper::Exec(const char* sql)
{
    if (!m_Impl->connection) return;
    duckdb_result result;
    duckdb_query(m_Impl->connection, sql, &result);
    duckdb_destroy_result(&result);
}

void DuckdbWrapper::CreateTable(const TableSchema* schema)
{
    Exec(MakeCreateTableSql(schema).c_str());
}
void DuckdbWrapper::DropTable(const char* tableName)
{
    std::string sql = "DROP TABLE IF EXISTS ";
    sql += tableName;
    sql += ";";
    Exec(sql.c_str());
}
void DuckdbWrapper::TruncateTable(const char* tableName)
{
    std::string sql = "DELETE FROM ";
    sql += tableName;
    sql += ";";
    Exec(sql.c_str());
}

void DuckdbWrapper::CreateTables(const TableSchema* const* schemas, int count)
{
    for (int i = 0; i < count; ++i)
    {
        CreateTable(schemas[i]);
    }
}
void DuckdbWrapper::DropTables(const TableSchema* const* schemas, int count)
{
    for (int i = 0; i < count; ++i)
    {
        DropTable(schemas[i]->tableName);
    }
}
void DuckdbWrapper::TruncateTables(const TableSchema* const* schemas, int count)
{
    for (int i = 0; i < count; ++i)
    {
        TruncateTable(schemas[i]->tableName);
    }
}

void DuckdbWrapper::Insert(const TableSchema* schema, const void* record)
{
    std::string sql = MakeInsertSql(schema);
    PreparedStatement stmt(m_Impl->connection, sql.c_str());
    if (!stmt.IsValid()) return;
    BindAllFields(stmt.Get(), schema, record);
    duckdb_result result;
    duckdb_execute_prepared(stmt.Get(), &result);
    duckdb_destroy_result(&result);
}
void DuckdbWrapper::BatchInsert(const TableSchema* schema, const void* const* records, int count)
{
    Exec("BEGIN;");
    for (int i = 0; i < count; ++i)
    {
        Insert(schema, records[i]);
    }
    Exec("COMMIT;");
}
void DuckdbWrapper::Update(const TableSchema* schema, const void* record)
{
    std::string sql = MakeUpdateSql(schema);
    PreparedStatement stmt(m_Impl->connection, sql.c_str());
    if (!stmt.IsValid()) return;

    for (int i = 0; i < schema->fieldCount; ++i)
    {
        BindField(stmt.Get(), i + 1, schema->fields[i], record);
    }
    int paramIndex = schema->fieldCount + 1;
    for (int i = 0; i < schema->primaryKeyCount; ++i)
    {
        int idx = schema->primaryKeyIndices[i];
        BindField(stmt.Get(), paramIndex, schema->fields[idx], record);
        paramIndex++;
    }

    duckdb_result result;
    duckdb_execute_prepared(stmt.Get(), &result);
    duckdb_destroy_result(&result);
}
void DuckdbWrapper::Delete(const TableSchema* schema, const void* record,
                           const int* keyFieldIndices, int keyFieldCount)
{
    std::string sql = MakeDeleteSql(schema, keyFieldIndices, keyFieldCount);
    PreparedStatement stmt(m_Impl->connection, sql.c_str());
    if (!stmt.IsValid()) return;
    BindKeyFields(stmt.Get(), schema, record, keyFieldIndices, keyFieldCount);

    duckdb_result result;
    duckdb_execute_prepared(stmt.Get(), &result);
    duckdb_destroy_result(&result);
}

void DuckdbWrapper::SelectAll(const TableSchema* schema, void* recordsList,
                              const RecordFactory& factory)
{
    std::string sql = "SELECT * FROM ";
    sql += schema->tableName;
    sql += ";";

    duckdb_result result;
    if (duckdb_query(m_Impl->connection, sql.c_str(), &result) != DuckDBSuccess)
    {
        duckdb_destroy_result(&result);
        return;
    }

    idx_t rowCount = duckdb_row_count(&result);
    for (idx_t row = 0; row < rowCount; ++row)
    {
        void* record = factory.Allocate();
        ReadRow(result, row, schema, record);
        factory.PushBack(recordsList, record);
    }
    duckdb_destroy_result(&result);
}
void DuckdbWrapper::SelectWithSql(const char* sql, const TableSchema* schema,
                                  void* recordsList, const RecordFactory& factory)
{
    duckdb_result result;
    if (duckdb_query(m_Impl->connection, sql, &result) != DuckDBSuccess)
    {
        duckdb_destroy_result(&result);
        return;
    }

    idx_t rowCount = duckdb_row_count(&result);
    for (idx_t row = 0; row < rowCount; ++row)
    {
        void* record = factory.Allocate();
        ReadRow(result, row, schema, record);
        factory.PushBack(recordsList, record);
    }
    duckdb_destroy_result(&result);
}

std::string DuckdbWrapper::SelectWithSqlVectorized(const char* sql, const TableSchema* schema,
                                                   void* recordsList,
                                                   const RecordFactory& factory)
{
    duckdb_result result;
    if (duckdb_query(m_Impl->connection, sql, &result) != DuckDBSuccess)
    {
        std::string errorMessage;
        const char* error = duckdb_result_error(&result);
        if (error != nullptr)
        {
            errorMessage = error;
        }
        duckdb_destroy_result(&result);
        return errorMessage;
    }

    while (true)
    {
        duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
        if (chunk == nullptr)
        {
            break;
        }
        idx_t chunkRowCount = duckdb_data_chunk_get_size(chunk);
        if (chunkRowCount > 0)
        {
            std::vector<void*> rowRecords(chunkRowCount);
            for (idx_t row = 0; row < chunkRowCount; ++row)
            {
                rowRecords[row] = factory.Allocate();
            }
            BindChunkToRecords(chunk, schema, rowRecords.data());
            for (idx_t row = 0; row < chunkRowCount; ++row)
            {
                factory.PushBack(recordsList, rowRecords[row]);
            }
        }
        duckdb_destroy_data_chunk(&chunk);
    }
    duckdb_destroy_result(&result);
    return std::string();
}
}
