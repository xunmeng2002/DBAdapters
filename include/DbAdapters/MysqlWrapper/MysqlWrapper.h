#pragma once
#include <DbAdapters/MysqlWrapper/MysqlWrapperExport.h>
#include <DbAdapters/DbInterface/Db.h>
#include <memory>
#include <string>


namespace DbAdapters
{
class MYSQLWRAPPER_EXPORTS MysqlWrapper : public Db
{
public:
    MysqlWrapper(const std::string& host);
    ~MysqlWrapper() override;

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string host_;
};
}
