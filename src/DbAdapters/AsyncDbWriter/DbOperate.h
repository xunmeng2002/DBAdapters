#pragma once
#include <DbAdapters/DbInterface/RecordHandle.h>
#include <Spark/Types.h>

namespace DbAdapters
{
class DbOperate
{
public:
	virtual ~DbOperate() = default;

	virtual void Deallocate() = 0;

	DbOperateType Operate;
	unsigned int TableId;
	unsigned int IndexId;
	RecordHandle Record;
};
}
