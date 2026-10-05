#pragma once
#include <DbAdapters/DuckdbWrapper/DuckdbWrapperExport.h>
#include <DbAdapters/DbInterface/Db.h>
#include <memory>
#include <string>


namespace DbAdapters
{
class DUCKDBWRAPPER_EXPORTS DuckdbWrapper : public Db
{
public:
    explicit DuckdbWrapper(const std::string& dbName);
    ~DuckdbWrapper() override;

    bool Connect() override;
    void DisConnect() override;
    void Exec(const char* sql) override;

    void CreateTable(const TableSchema* schema) override;
    void DropTable(const char* tableName) override;
    void TruncateTable(const char* tableName) override;

    void CreateTables(const TableSchema* const* schemas, int count) override;
    void DropTables(const TableSchema* const* schemas, int count) override;
    void TruncateTables(const TableSchema* const* schemas, int count) override;

    void Insert(const TableSchema* schema, const void* record) override;
    void BatchInsert(const TableSchema* schema, const void* const* records, int count) override;
    void Update(const TableSchema* schema, const void* record) override;
    void Delete(const TableSchema* schema, const void* record, const int* keyFieldIndices, int keyFieldCount) override;

    void SelectAll(const TableSchema* schema, void* recordsList, const RecordFactory& factory) override;
    void SelectWithSql(const char* sql, const TableSchema* schema, void* recordsList, const RecordFactory& factory) override;

    // 向量化批量读：SQL 列序必须与 schema 字段序一一对应；
    // 数据以 chunk 向量方式逐列绑定（比 SelectWithSql 的逐值转换快，且保留 NULL 区分）。
    // NULL 写入类型哨兵：Double → +inf，Int/Int64 → 0，Char → 空串，Bool → false。
    // 返回空串表示成功，否则返回 duckdb 错误信息。
    std::string SelectWithSqlVectorized(const char* sql, const TableSchema* schema, void* recordsList, const RecordFactory& factory);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
