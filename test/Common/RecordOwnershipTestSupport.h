#pragma once

#include <DbAdapters/DbInterface/Schema.h>
#include <DbAdapters/DbInterface/SchemaRegistry.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

namespace DbAdapters
{
namespace RecordOwnershipTest
{
// 探针记录把"归还"落成一个外部计数: Deallocate 每被调用一次就自增, 于是"归还了没""归还了几次"
// 都成为可断言的事实, 不必依赖内存工具或 Debug 版对象池的内部登记表.
class OwnershipProbeRecord
{
public:
    int PK = 0;
    std::atomic<int>* releaseCount = nullptr;

    static OwnershipProbeRecord* Allocate()
    {
        return new OwnershipProbeRecord();
    }
    void Deallocate()
    {
        if (releaseCount != nullptr)
        {
            releaseCount->fetch_add(1);
        }
        delete this;
    }
    static const TableSchema& GetSchema();
};

inline const FieldDescriptor OwnershipProbeRecordFields[] = {
    {"PK", FieldType::Int32, offsetof(OwnershipProbeRecord, PK), 0},
};
inline const int OwnershipProbeRecordPKIndices[] = {0};

inline const TableSchema& OwnershipProbeRecord::GetSchema()
{
    static const TableSchema schema = {
        "t_ownership_probe", OwnershipProbeRecordFields, 1, OwnershipProbeRecordPKIndices, 1, nullptr, 0,
    };
    return schema;
}

inline OwnershipProbeRecord* AllocateProbeRecord(std::atomic<int>& releaseCount, int primaryKey)
{
    OwnershipProbeRecord* record = OwnershipProbeRecord::Allocate();
    record->PK = primaryKey;
    record->releaseCount = &releaseCount;
    return record;
}

// 按 schema 数组实现的注册表替身: TableId 即数组下标, 越界返回空 schema, 走"无 schema 则不落库"分支.
// 单元测试传一张探针表, 集成测试传一张真后端上的表, 共用这一份实现.
class StaticSchemaRegistry : public SchemaRegistry
{
public:
    StaticSchemaRegistry(const TableSchema* const* schemas, int schemaCount)
        : schemas_(schemas), schemaCount_(schemaCount)
    {
    }

    const TableSchema* GetSchema(unsigned int tableID) const override
    {
        return tableID < static_cast<unsigned int>(schemaCount_) ? schemas_[tableID] : nullptr;
    }
    const TableSchema* const* GetAllSchemas() const override
    {
        return schemas_;
    }
    int GetTableCount() const override
    {
        return schemaCount_;
    }

private:
    const TableSchema* const* schemas_;
    int schemaCount_;
};

template <typename Condition>
bool WaitUntil(Condition condition, int timeoutMilliseconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMilliseconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (condition())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}
}
}
