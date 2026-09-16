#pragma once
#include <Spark/Types.h>

namespace DbAdapters
{
class DbOperate
{
public:
	static DbOperate* Allocate();
	virtual void Deallocate() = 0;
	virtual void DeallocateRecord() = 0;


	DbOperateType Operate;
	unsigned int TableId;
	unsigned int IndexId;
	void* Record;
};
}
