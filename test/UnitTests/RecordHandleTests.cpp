#include "RecordOwnershipTestSupport.h"

#include <DbAdapters/DbInterface/RecordHandle.h>

#include "doctest/doctest.h"

#include <atomic>
#include <utility>
#include <vector>

using namespace DbAdapters;
using namespace DbAdapters::RecordOwnershipTest;

TEST_SUITE("RecordHandle")
{
TEST_CASE("移动后源不再归还")
{
    std::atomic<int> releaseCount{ 0 };
    RecordHandle movedInto;
    {
        RecordHandle source = AdoptRecord(AllocateProbeRecord(releaseCount, 1));
        movedInto = std::move(source);
        CHECK(source.Get() == nullptr);
        CHECK(movedInto.Get() != nullptr);
        CHECK(releaseCount.load() == 0);
    }
    CHECK(releaseCount.load() == 0);
    movedInto.Reset();
    CHECK(releaseCount.load() == 1);
}

TEST_CASE("移动赋值先归还旧记录")
{
    std::atomic<int> destinationReleaseCount{ 0 };
    std::atomic<int> sourceReleaseCount{ 0 };
    RecordHandle destination = AdoptRecord(AllocateProbeRecord(destinationReleaseCount, 2));
    RecordHandle source = AdoptRecord(AllocateProbeRecord(sourceReleaseCount, 3));
    destination = std::move(source);
    CHECK(destinationReleaseCount.load() == 1);
    CHECK(sourceReleaseCount.load() == 0);
    CHECK(destination.Get() != nullptr);
    destination.Reset();
    CHECK(sourceReleaseCount.load() == 1);
    CHECK(destinationReleaseCount.load() == 1);
}

TEST_CASE("容器扩容不重复归还")
{
    std::atomic<int> releaseCount{ 0 };
    std::vector<RecordHandle> handles;
    handles.reserve(1);
    for (int index = 0; index < 3; ++index)
    {
        handles.push_back(AdoptRecord(AllocateProbeRecord(releaseCount, 10 + index)));
    }
    CHECK(releaseCount.load() == 0);
    handles.clear();
    CHECK(releaseCount.load() == 3);
}
}
