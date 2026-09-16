#include <DBAdapters/DuckdbWrapper/DuckdbWrapper.h>

#include <DBAdapters/DBInterface/FailureLogThrottle.h>

#include <Spark/Core/Logger/Logger.h>

#include <duckdb.h>

#include <cstring>
#include <limits>
#include <sstream>
#include <vector>


namespace DbAdapters
{
    using Spark::Core::LogLevel;

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
        const char* GetPrepareError() const { return stmt_ != nullptr ? duckdb_prepare_error(stmt_) : nullptr; }

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
        case FieldType::Int8:
            duckdb_bind_int8(stmt, index, *reinterpret_cast<const int8_t*>(data));
            break;
        case FieldType::UInt8:
            duckdb_bind_uint8(stmt, index, *reinterpret_cast<const uint8_t*>(data));
            break;
        case FieldType::Int16:
            duckdb_bind_int16(stmt, index, *reinterpret_cast<const int16_t*>(data));
            break;
        case FieldType::UInt16:
            duckdb_bind_uint16(stmt, index, *reinterpret_cast<const uint16_t*>(data));
            break;
        case FieldType::Int32:
            duckdb_bind_int32(stmt, index, *reinterpret_cast<const int32_t*>(data));
            break;
        case FieldType::UInt32:
            duckdb_bind_uint32(stmt, index, *reinterpret_cast<const uint32_t*>(data));
            break;
        case FieldType::Int64:
            duckdb_bind_int64(stmt, index, *reinterpret_cast<const int64_t*>(data));
            break;
        case FieldType::UInt64:
            duckdb_bind_uint64(stmt, index, *reinterpret_cast<const uint64_t*>(data));
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

    // duckdb 的 UBIGINT/UINTEGER/USMALLINT/UTINYINT 是无符号列类型，其余整数列按有符号处理。
    bool IsUnsignedDuckdbType(duckdb_type type)
    {
        return type == DUCKDB_TYPE_UBIGINT || type == DUCKDB_TYPE_UINTEGER ||
               type == DUCKDB_TYPE_USMALLINT || type == DUCKDB_TYPE_UTINYINT;
    }

    // 结果级（duckdb_value_* 路径）读一个整数单元格并收窄写入 dest，返回 false 表示按目标类型饱和。
    // 按列的实际符号选最宽访问器：duckdb_value_int8/int16/int32 一类在值越界时静默返回 0，
    // 会把 0 当成读到的值写进记录，这里一律不用。
    bool ReadResultCellAsInteger(duckdb_result& result, idx_t columnIndex, idx_t row,
                                 FieldType targetType, char* dest)
    {
        if (IsUnsignedDuckdbType(duckdb_column_type(&result, columnIndex)))
        {
            return TryWriteIntegerFromUnsigned(duckdb_value_uint64(&result, columnIndex, row),
                                               targetType, dest);
        }
        return TryWriteIntegerFromSigned(duckdb_value_int64(&result, columnIndex, row),
                                         targetType, dest);
    }

    void ReadRow(duckdb_result& result, idx_t row, const TableSchema* schema, void* record,
                 int& clampedCount)
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
                if (!ReadResultCellAsInteger(result, i, row, field.type, dest))
                {
                    ++clampedCount;
                }
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

    // 读取侧收窄饱和在整次 SELECT 结束后汇总上报一条 Warning：逐格上报会被行数淹没。
    void LogClampedCells(const TableSchema* schema, int clampedCount, int totalRowCount)
    {
        if (clampedCount > 0)
        {
            WriteLog(LogLevel::Warning,
                "DuckdbWrapper: SELECT narrowed out-of-range values. Table:%s, ClampedCells:%d/%d",
                schema->tableName, clampedCount, totalRowCount * schema->fieldCount);
        }
    }

    // 把结果级读到的全部行交给 factory。
    void ReadResultRows(duckdb_result& result, const TableSchema* schema, void* recordsList,
                        const RecordFactory& factory)
    {
        const idx_t rowCount = duckdb_row_count(&result);
        int clampedCount = 0;
        for (idx_t row = 0; row < rowCount; ++row)
        {
            void* record = factory.Allocate();
            ReadRow(result, row, schema, record, clampedCount);
            factory.PushBack(recordsList, record);
        }
        LogClampedCells(schema, clampedCount, static_cast<int>(rowCount));
    }

    void WriteNullSentinel(char* dest, FieldType type)
    {
        switch (type)
        {
        case FieldType::Double:
            *reinterpret_cast<double*>(dest) = std::numeric_limits<double>::infinity();
            break;
        case FieldType::Int8:
            *reinterpret_cast<int8_t*>(dest) = 0;
            break;
        case FieldType::UInt8:
            *reinterpret_cast<uint8_t*>(dest) = 0;
            break;
        case FieldType::Int16:
            *reinterpret_cast<int16_t*>(dest) = 0;
            break;
        case FieldType::UInt16:
            *reinterpret_cast<uint16_t*>(dest) = 0;
            break;
        case FieldType::Int32:
            *reinterpret_cast<int32_t*>(dest) = 0;
            break;
        case FieldType::UInt32:
            *reinterpret_cast<uint32_t*>(dest) = 0;
            break;
        case FieldType::Int64:
            *reinterpret_cast<int64_t*>(dest) = 0;
            break;
        case FieldType::UInt64:
            *reinterpret_cast<uint64_t*>(dest) = 0;
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

    // 从 chunk 向量按源类型取 uint64；带符号源为负、或源类型无法无损映射时置 0 并把 clamped 置真。
    unsigned long long ReadCellAsUInt64(duckdb_type sourceType, void* rawData, idx_t row,
                                        uint8_t decimalWidth, uint8_t decimalScale, bool& clamped)
    {
        switch (sourceType)
        {
        case DUCKDB_TYPE_UBIGINT:
            return static_cast<unsigned long long*>(rawData)[row];
        case DUCKDB_TYPE_UINTEGER:
            return static_cast<unsigned int*>(rawData)[row];
        case DUCKDB_TYPE_USMALLINT:
            return static_cast<unsigned short*>(rawData)[row];
        case DUCKDB_TYPE_UTINYINT:
            return static_cast<unsigned char*>(rawData)[row];
        case DUCKDB_TYPE_BIGINT:
            return SaturatingToUInt64(static_cast<long long*>(rawData)[row], clamped);
        case DUCKDB_TYPE_INTEGER:
            return SaturatingToUInt64(static_cast<int*>(rawData)[row], clamped);
        case DUCKDB_TYPE_SMALLINT:
            return SaturatingToUInt64(static_cast<short*>(rawData)[row], clamped);
        case DUCKDB_TYPE_TINYINT:
            return SaturatingToUInt64(static_cast<signed char*>(rawData)[row], clamped);
        case DUCKDB_TYPE_DOUBLE:
            return SaturatingToUInt64(static_cast<long long>(static_cast<double*>(rawData)[row]), clamped);
        case DUCKDB_TYPE_FLOAT:
            return SaturatingToUInt64(static_cast<long long>(static_cast<float*>(rawData)[row]), clamped);
        case DUCKDB_TYPE_DECIMAL:
        {
            duckdb_hugeint value = static_cast<duckdb_hugeint*>(rawData)[row];
            return SaturatingToUInt64(static_cast<long long>(
                duckdb_decimal_to_double(duckdb_decimal{ decimalWidth, decimalScale, value })), clamped);
        }
        case DUCKDB_TYPE_HUGEINT:
            return SaturatingToUInt64(static_cast<long long>(
                duckdb_hugeint_to_double(static_cast<duckdb_hugeint*>(rawData)[row])), clamped);
        case DUCKDB_TYPE_BOOLEAN:
            return static_cast<bool*>(rawData)[row] ? 1ULL : 0ULL;
        default:
            clamped = true;
            return 0ULL;
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
    void BindChunkToRecords(duckdb_data_chunk chunk, const TableSchema* schema, void** records,
                            int& clampedCount)
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
                case FieldType::UInt64:
                {
                    bool clampedFromSource = false;
                    *reinterpret_cast<uint64_t*>(dest) = ReadCellAsUInt64(
                        sourceType, rawData, row, decimalWidth, decimalScale, clampedFromSource);
                    if (clampedFromSource)
                    {
                        ++clampedCount;
                    }
                    break;
                }
                case FieldType::Int8:
                case FieldType::UInt8:
                case FieldType::Int16:
                case FieldType::UInt16:
                case FieldType::Int32:
                case FieldType::UInt32:
                case FieldType::Int64:
                    if (!TryWriteIntegerFromSigned(
                            ReadCellAsInt64(sourceType, rawData, row, decimalWidth, decimalScale),
                            field.type, dest))
                    {
                        ++clampedCount;
                    }
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
            case FieldType::Int8:   sql << "TINYINT"; break;
            case FieldType::UInt8:  sql << "UTINYINT"; break;
            case FieldType::Int16:  sql << "SMALLINT"; break;
            case FieldType::UInt16: sql << "USMALLINT"; break;
            case FieldType::Int32:  sql << "INTEGER"; break;
            case FieldType::UInt32: sql << "UINTEGER"; break;
            case FieldType::Int64:  sql << "BIGINT"; break;
            case FieldType::UInt64: sql << "UBIGINT"; break;
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
    FailureLogThrottle failureLogThrottle;
};

DuckdbWrapper::DuckdbWrapper(const std::string& dbName)
    : impl_(new Impl)
{
    if (duckdb_open(dbName.c_str(), &impl_->database) != DuckDBSuccess)
    {
        WriteLog(LogLevel::Error, "DuckdbWrapper: Open database failed. Path:%s", dbName.c_str());
        impl_->database = nullptr;
        return;
    }
    if (duckdb_connect(impl_->database, &impl_->connection) != DuckDBSuccess)
    {
        // 连接失败后连接句柄为空，后续所有语句都会静默失效，必须显式记录
        WriteLog(LogLevel::Error, "DuckdbWrapper: Connect failed. Path:%s", dbName.c_str());
        impl_->connection = nullptr;
    }
}
DuckdbWrapper::~DuckdbWrapper()
{
    DisConnect();
    delete impl_;
}

bool DuckdbWrapper::Connect()
{
    return impl_->connection != nullptr;
}
void DuckdbWrapper::DisConnect()
{
    if (impl_->connection)
    {
        duckdb_disconnect(&impl_->connection);
        impl_->connection = nullptr;
    }
    if (impl_->database)
    {
        duckdb_close(&impl_->database);
        impl_->database = nullptr;
    }
}
void DuckdbWrapper::Exec(const char* sql)
{
    if (impl_->connection == nullptr)
    {
        // 连接为空时所有语句都会静默失效（含建表与批量写的事务控制），必须显式记录
        WriteLog(LogLevel::Error, "DuckdbWrapper: EXEC skipped, database is not open. Sql:%s", sql);
        return;
    }
    duckdb_result result;
    if (duckdb_query(impl_->connection, sql, &result) != DuckDBSuccess)
    {
        const char* errorDetail = duckdb_result_error(&result);
        WriteLog(LogLevel::Error, "DuckdbWrapper: EXEC failed. Error:%s, Sql:%s",
            errorDetail != nullptr ? errorDetail : "unknown", sql);
    }
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
    if (impl_->connection == nullptr)
    {
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "INSERT", schema->tableName, "database is not open");
        return;
    }
    PreparedStatement stmt(impl_->connection, sql.c_str());
    if (!stmt.IsValid())
    {
        const char* prepareError = stmt.GetPrepareError();
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "INSERT", schema->tableName,
            prepareError != nullptr ? prepareError : "prepare failed");
        return;
    }
    BindAllFields(stmt.Get(), schema, record);
    duckdb_result result;
    if (duckdb_execute_prepared(stmt.Get(), &result) != DuckDBSuccess)
    {
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "INSERT", schema->tableName,
            duckdb_result_error(&result));
    }
    duckdb_destroy_result(&result);
}
void DuckdbWrapper::BatchInsert(const TableSchema* schema, const void* const* records, int count)
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
        WriteLog(LogLevel::Error, "DuckdbWrapper: BATCH INSERT incomplete. Table:%s, FailedRecords:%d/%d",
            schema->tableName, failedRecordCount, count);
    }
}
void DuckdbWrapper::Update(const TableSchema* schema, const void* record)
{
    std::string sql = MakeUpdateSql(schema);
    if (impl_->connection == nullptr)
    {
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "UPDATE", schema->tableName, "database is not open");
        return;
    }
    PreparedStatement stmt(impl_->connection, sql.c_str());
    if (!stmt.IsValid())
    {
        const char* prepareError = stmt.GetPrepareError();
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "UPDATE", schema->tableName,
            prepareError != nullptr ? prepareError : "prepare failed");
        return;
    }

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
    if (duckdb_execute_prepared(stmt.Get(), &result) != DuckDBSuccess)
    {
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "UPDATE", schema->tableName,
            duckdb_result_error(&result));
    }
    duckdb_destroy_result(&result);
}
void DuckdbWrapper::Delete(const TableSchema* schema, const void* record,
                           const int* keyFieldIndices, int keyFieldCount)
{
    std::string sql = MakeDeleteSql(schema, keyFieldIndices, keyFieldCount);
    if (impl_->connection == nullptr)
    {
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "DELETE", schema->tableName, "database is not open");
        return;
    }
    PreparedStatement stmt(impl_->connection, sql.c_str());
    if (!stmt.IsValid())
    {
        const char* prepareError = stmt.GetPrepareError();
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "DELETE", schema->tableName,
            prepareError != nullptr ? prepareError : "prepare failed");
        return;
    }
    BindKeyFields(stmt.Get(), schema, record, keyFieldIndices, keyFieldCount);

    duckdb_result result;
    if (duckdb_execute_prepared(stmt.Get(), &result) != DuckDBSuccess)
    {
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "DELETE", schema->tableName,
            duckdb_result_error(&result));
    }
    duckdb_destroy_result(&result);
}

void DuckdbWrapper::SelectAll(const TableSchema* schema, void* recordsList,
                              const RecordFactory& factory)
{
    std::string sql = "SELECT * FROM ";
    sql += schema->tableName;
    sql += ";";

    if (impl_->connection == nullptr)
    {
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "SELECT", schema->tableName, "database is not open");
        return;
    }
    duckdb_result result;
    if (duckdb_query(impl_->connection, sql.c_str(), &result) != DuckDBSuccess)
    {
        LogOperationFailure(impl_->failureLogThrottle, "DuckdbWrapper", "SELECT", schema->tableName,
            duckdb_result_error(&result));
        duckdb_destroy_result(&result);
        return;
    }

    ReadResultRows(result, schema, recordsList, factory);
    duckdb_destroy_result(&result);
}
void DuckdbWrapper::SelectWithSql(const char* sql, const TableSchema* schema,
                                  void* recordsList, const RecordFactory& factory)
{
    if (impl_->connection == nullptr)
    {
        WriteLog(LogLevel::Error, "DuckdbWrapper: SELECT skipped, database is not open. Sql:%s", sql);
        return;
    }
    duckdb_result result;
    if (duckdb_query(impl_->connection, sql, &result) != DuckDBSuccess)
    {
        const char* errorDetail = duckdb_result_error(&result);
        WriteLog(LogLevel::Error, "DuckdbWrapper: SELECT failed. Error:%s, Sql:%s",
            errorDetail != nullptr ? errorDetail : "unknown", sql);
        duckdb_destroy_result(&result);
        return;
    }

    ReadResultRows(result, schema, recordsList, factory);
    duckdb_destroy_result(&result);
}

std::string DuckdbWrapper::SelectWithSqlVectorized(const char* sql, const TableSchema* schema,
                                                   void* recordsList,
                                                   const RecordFactory& factory)
{
    duckdb_result result;
    if (duckdb_query(impl_->connection, sql, &result) != DuckDBSuccess)
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

    int clampedCount = 0;
    idx_t totalRowCount = 0;
    while (true)
    {
        duckdb_data_chunk chunk = duckdb_fetch_chunk(result);
        if (chunk == nullptr)
        {
            break;
        }
        idx_t chunkRowCount = duckdb_data_chunk_get_size(chunk);
        totalRowCount += chunkRowCount;
        if (chunkRowCount > 0)
        {
            std::vector<void*> rowRecords(chunkRowCount);
            for (idx_t row = 0; row < chunkRowCount; ++row)
            {
                rowRecords[row] = factory.Allocate();
            }
            BindChunkToRecords(chunk, schema, rowRecords.data(), clampedCount);
            for (idx_t row = 0; row < chunkRowCount; ++row)
            {
                factory.PushBack(recordsList, rowRecords[row]);
            }
        }
        duckdb_destroy_data_chunk(&chunk);
    }
    LogClampedCells(schema, clampedCount, static_cast<int>(totalRowCount));
    duckdb_destroy_result(&result);
    return std::string();
}
}
