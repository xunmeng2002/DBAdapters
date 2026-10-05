#pragma once
#include <DbAdapters/DbInterface/DbOperate.h>
#include <utility>
#include <vector>


namespace DbAdapters
{
class DbOperateImpl : public DbOperate
{
public:
    virtual void Deallocate() override;

    const std::vector<RecordHandle>& GetBatchRecords() const { return batchRecords_; }
    void SetBatchRecords(std::vector<RecordHandle> records) { batchRecords_ = std::move(records); }

private:
    std::vector<RecordHandle> batchRecords_;
};
}
