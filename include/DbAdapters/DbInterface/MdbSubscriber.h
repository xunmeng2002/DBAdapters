#pragma once
#include <DbAdapters/DbInterface/RecordHandle.h>
#include <Spark/Types.h>
#include <atomic>
#include <vector>


namespace DbAdapters
{
class MdbSubscriber
{
public:
	MdbSubscriber()
		:connected_(false)
	{
	}
	virtual ~MdbSubscriber() = default;

	virtual void OnTableOp(DbOperateType op) {}
	virtual void OnRecordInsert(unsigned int tableID, RecordHandle record) {}
	virtual void OnRecordBatchInsert(unsigned int tableID, std::vector<RecordHandle> records) {}
	virtual void OnRecordErase(unsigned int tableID, RecordHandle record) {}
	virtual void OnRecordEraseByIndex(unsigned int tableID, unsigned int indexId, RecordHandle record) {}
	virtual void OnRecordUpdate(unsigned int tableID, RecordHandle record) {}
	virtual void OnRecordTruncate(unsigned int tableID) {}

public:
	std::atomic<bool> connected_;
};
}
