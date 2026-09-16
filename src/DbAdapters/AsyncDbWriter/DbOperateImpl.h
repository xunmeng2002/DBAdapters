#pragma once
#include <DBAdapters/DBInterface/SchemaRegistry.h>
#include <DBAdapters/DBInterface/DbOperate.h>
#include <vector>


namespace DbAdapters
{
class DbOperateImpl : public DbOperate
{
public:
    void SetSchemaRegistry(SchemaRegistry* registry) { schema_registry_ = registry; }

    virtual void Deallocate() override;
    virtual void DeallocateRecord() override;

    std::vector<const void*>& GetBatchData() { return batch_data_; }

private:
    std::vector<const void*> batch_data_;
    SchemaRegistry* schema_registry_ = nullptr;
};
}
