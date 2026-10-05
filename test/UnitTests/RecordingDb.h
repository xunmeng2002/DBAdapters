#pragma once

#include <DbAdapters/DbInterface/Db.h>

#include <atomic>
#include <stdexcept>

namespace DbAdapters
{
namespace RecordOwnershipTest
{
// 记录调用次数并可按需抛出后端的替身: 计数在同一线程内先写数据再自增, 观察方据自增后的
// acquire 语义即可安全读到前面写的数据.
class RecordingDb : public Db
{
public:
    bool Connect() override
    {
        connectCount.fetch_add(1);
        return connectSucceeds.load();
    }
    void DisConnect() override
    {
        disconnectCount.fetch_add(1);
    }
    void Exec(const char*) override {}
    void CreateTable(const TableSchema*) override {}
    void DropTable(const char*) override {}
    void TruncateTable(const char*) override
    {
        truncateTableCount.fetch_add(1);
    }
    void CreateTables(const TableSchema* const*, int) override {}
    void DropTables(const TableSchema* const*, int) override {}
    void TruncateTables(const TableSchema* const*, int) override {}
    void Insert(const TableSchema*, const void* record) override
    {
        insertedRecord = record;
        insertCount.fetch_add(1);
        if (insertThrows.load())
        {
            throw std::runtime_error("RecordingDb: Insert throws by request");
        }
    }
    void BatchInsert(const TableSchema*, const void* const* records, int count) override
    {
        lastBatchRecordCount = count;
        lastBatchFirstRecord = count > 0 ? records[0] : nullptr;
        lastBatchLastRecord = count > 0 ? records[count - 1] : nullptr;
        batchInsertCount.fetch_add(1);
        if (batchInsertThrows.load())
        {
            throw std::runtime_error("RecordingDb: BatchInsert throws by request");
        }
    }
    void Update(const TableSchema*, const void*) override
    {
        updateCount.fetch_add(1);
    }
    void Delete(const TableSchema*, const void*, const int*, int) override
    {
        deleteCount.fetch_add(1);
    }
    void SelectAll(const TableSchema*, void*, const RecordFactory&) override {}
    void SelectWithSql(const char*, const TableSchema*, void*, const RecordFactory&) override {}

    std::atomic<bool> connectSucceeds{ true };
    std::atomic<bool> insertThrows{ false };
    std::atomic<bool> batchInsertThrows{ false };
    std::atomic<int> connectCount{ 0 };
    std::atomic<int> disconnectCount{ 0 };
    std::atomic<int> insertCount{ 0 };
    std::atomic<int> batchInsertCount{ 0 };
    std::atomic<int> updateCount{ 0 };
    std::atomic<int> deleteCount{ 0 };
    std::atomic<int> truncateTableCount{ 0 };
    const void* insertedRecord = nullptr;
    const void* lastBatchFirstRecord = nullptr;
    const void* lastBatchLastRecord = nullptr;
    int lastBatchRecordCount = 0;
};
}
}
