#include "RecordOwnershipTestSupport.h"
#include "RecordingDb.h"

#include <DbAdapters/AsyncDbWriter/AsyncDbWriter.h>
#include <DbAdapters/DbInterface/RecordHandle.h>

#include "doctest/doctest.h"

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

using namespace DbAdapters;
using namespace DbAdapters::RecordOwnershipTest;

namespace
{
// AsyncDbWriter 析构会 delete 它持有的 Db, 故 Db 必须与 writer 同生共死:
// 若把 RecordingDb 做成并列的成员对象, 成员析构顺序会让 writer 先析构再去 delete 一个已析构的 Db.
class WriterHarness
{
public:
    explicit WriterHarness(bool connectSucceeds = true)
        : probeSchemas_{ &OwnershipProbeRecord::GetSchema() },
          schemaRegistry_(probeSchemas_, 1),
          writer_(new AsyncDbWriter(new RecordingDb(), &schemaRegistry_))
    {
        Recorder().connectSucceeds = connectSucceeds;
        writer_->Start();
    }
    ~WriterHarness()
    {
        writer_->Stop();
        writer_->Join();
    }
    WriterHarness(const WriterHarness&) = delete;
    WriterHarness& operator=(const WriterHarness&) = delete;

    RecordingDb& Recorder() { return *static_cast<RecordingDb*>(writer_->GetDb()); }
    AsyncDbWriter& Writer() { return *writer_; }

private:
    const TableSchema* probeSchemas_[1];
    StaticSchemaRegistry schemaRegistry_;
    std::unique_ptr<AsyncDbWriter> writer_;
};
}

TEST_SUITE("RecordOwnership")
{
TEST_CASE("借用不归还")
{
    WriterHarness harness;
    std::atomic<int> releaseCount{ 0 };
    std::unique_ptr<OwnershipProbeRecord> borrowedRecord{ AllocateProbeRecord(releaseCount, 1) };
    harness.Writer().OnRecordInsert(0, BorrowRecord(borrowedRecord.get()));
    REQUIRE(WaitUntil([&] { return harness.Recorder().insertCount.load() >= 1; }, 3000));
    CHECK(harness.Recorder().insertedRecord == borrowedRecord.get());
    CHECK(releaseCount.load() == 0);
}

TEST_CASE("移交恰好归还一次")
{
    WriterHarness harness;
    std::atomic<int> releaseCount{ 0 };
    harness.Writer().OnRecordInsert(0, AdoptRecord(AllocateProbeRecord(releaseCount, 2)));
    CHECK(WaitUntil([&] { return releaseCount.load() == 1; }, 3000));
}

TEST_CASE("批元素逐个恰好归还一次")
{
    WriterHarness harness;
    std::atomic<int> releaseCount{ 0 };
    OwnershipProbeRecord* firstRecord = AllocateProbeRecord(releaseCount, 20);
    OwnershipProbeRecord* middleRecord = AllocateProbeRecord(releaseCount, 21);
    OwnershipProbeRecord* lastRecord = AllocateProbeRecord(releaseCount, 22);
    std::vector<RecordHandle> batchRecords;
    batchRecords.push_back(AdoptRecord(firstRecord));
    batchRecords.push_back(AdoptRecord(middleRecord));
    batchRecords.push_back(AdoptRecord(lastRecord));
    harness.Writer().OnRecordBatchInsert(0, std::move(batchRecords));
    REQUIRE(WaitUntil([&] { return harness.Recorder().batchInsertCount.load() >= 1; }, 3000));
    CHECK(WaitUntil([&] { return releaseCount.load() == 3; }, 3000));
    CHECK(harness.Recorder().lastBatchRecordCount == 3);
    CHECK(harness.Recorder().lastBatchFirstRecord == firstRecord);
    CHECK(harness.Recorder().lastBatchLastRecord == lastRecord);
}

TEST_CASE("执行抛异常仍恰好归还一次")
{
    WriterHarness harness;
    harness.Recorder().insertThrows = true;
    std::atomic<int> releaseCount{ 0 };
    harness.Writer().OnRecordInsert(0, AdoptRecord(AllocateProbeRecord(releaseCount, 30)));
    CHECK(WaitUntil([&] { return releaseCount.load() == 1; }, 15000));
    CHECK(harness.Recorder().disconnectCount.load() >= 1);
}

TEST_CASE("断开丢弃待办时恰好归还一次")
{
    WriterHarness harness(false);
    std::atomic<int> releaseCount{ 0 };
    harness.Writer().OnRecordInsert(0, AdoptRecord(AllocateProbeRecord(releaseCount, 40)));
    harness.Writer().OnRecordInsert(0, AdoptRecord(AllocateProbeRecord(releaseCount, 41)));
    harness.Writer().DisConnect();
    CHECK(WaitUntil([&] { return releaseCount.load() == 2; }, 3000));
}
}
