#pragma once
#include "DbOperate.h"

#include <utility>
#include <vector>


namespace DbAdapters
{
class DbOperateImpl : public DbOperate
{
public:
    static DbOperateImpl* Allocate();

    virtual void Deallocate() override;

    const std::vector<RecordHandle>& GetBatchRecords() const { return batchRecords_; }
    void SetBatchRecords(std::vector<RecordHandle> records) { batchRecords_ = std::move(records); }

private:
    std::vector<RecordHandle> batchRecords_;
};
}
